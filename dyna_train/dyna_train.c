// dyna_train.c — Standalone driver: random/zero/fixed-action or policy inference.
// Built by `bash build.sh dyna_train --fast`. Interactive raylib mode runs
// random actions by default; with --csv it dumps trajectories.
//
// Trajectory dump format is a small binary file (.bin); a Python post-processor
// converts to parquet (see ../tools/bake_traj_parquet.py).
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include "raylib.h"

#include "dyna_train.h"
#include "puffernet.h"

#define NUM_ACTIONS 2
#define HIDDEN_SIZE 128
#define NUM_LAYERS  2

static DynaTrain make_env(unsigned int seed,
                          float arena_size, int max_steps, float dt,
                          int n_obs_min, int n_obs_max,
                          int order_min, int order_max,
                          float speed_min, float speed_max,
                          float std_min, float std_max,
                          float goal_radius) {
    DynaTrain env = {
        .arena_size = arena_size,
        .max_steps = max_steps,
        .dt = dt,
        .num_obstacles_min = n_obs_min,
        .num_obstacles_max = n_obs_max,
        .order_min = order_min,
        .order_max = order_max,
        .speed_min = speed_min,
        .speed_max = speed_max,
        .std_min   = std_min,
        .std_max   = std_max,
        .min_init_goal_dist = 12.0f,
        .gamma_d = 1.0f,
        .beta = 1.0f,
        .sigma_o = 2.0f,
        .success_bonus = 1.0f,
        .collision_penalty = 1.0f,
        .goal_radius = goal_radius,
        // Reward extensions — see dyna_train.h.
        .time_penalty = 0.005f,
        .alpha_g = 0.05f, .sigma_g = 5.0f,
        .rng = seed,
        // Motion-family defaults: poly-only, matching the historical config.
        .mw_poly = 1, .mw_linear = 0, .mw_reciprocating = 0,
        .mw_sinusoidal = 0, .mw_random_walk = 0, .mw_stationary = 0,
        .amp_max = 3.0f, .freq_min = 0.05f, .freq_max = 0.5f,
        .walk_step_std = 0.4f, .reciprocate_min_dist = 4.0f,
        // Velocity envelope: matches paper move_base local planner.
        .train_v_max = 0.5f, .train_v_min = -0.5f, .train_w_max = (float)M_PI,
        // Acceleration envelope: matches paper base_local_planner_params.yaml.
        .train_a_max = 10.0f, .train_alpha_max = 20.0f,
    };
    return env;
}

// Binary trajectory dump (one record per step):
//   header: ep(int32) tick(int32) n_obs(int32) outcome(int32)
//   body:   rx ry theta v w gx gy
//           (ox,oy) × MAX_OBSTACLES   (NaN for unused slots)
//           reward done
// All floats are 32-bit. Endianness = host (we read on the same machine).
typedef struct {
    int   ep, tick, n_obs, outcome;
    float rx, ry, theta, v, w, gx, gy;
    float ox[MAX_OBSTACLES], oy[MAX_OBSTACLES];
    float reward;
    int   done;
} Row;

static void write_row(FILE* f, const Row* r) {
    fwrite(r, sizeof(Row), 1, f);
}

static int main_csv_mode(const char* path, int mode, const char* load_path,
                         int episodes, unsigned int seed,
                         float arena_size, int max_steps, float dt,
                         int n_obs_min, int n_obs_max,
                         int order_min, int order_max,
                         float speed_min, float speed_max,
                         float std_min,  float std_max,
                         float goal_radius) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "fopen %s\n", path); return 1; }
    // Header: 4 ints — magic, n_episodes, max_obstacles, lidar_beams (for parquet baker)
    int header[4] = {0x44594E41, episodes, MAX_OBSTACLES, LIDAR_BEAMS};
    fwrite(header, sizeof(int), 4, f);

    DynaTrain env = make_env(seed, arena_size, max_steps, dt,
                             n_obs_min, n_obs_max, order_min, order_max,
                             speed_min, speed_max, std_min, std_max, goal_radius);
    allocate(&env);

    Weights* weights = NULL;
    PufferNet* net = NULL;
    if (load_path != NULL) {
        weights = load_weights(load_path);
        if (weights == NULL) {
            fprintf(stderr, "Failed to load weights from %s\n", load_path);
            fclose(f);
            return 1;
        }
        int logit_sizes[NUM_ACTIONS] = {1, 1};
        net = make_puffernet(weights, 1, OBS_DIM, HIDDEN_SIZE, NUM_LAYERS, logit_sizes, NUM_ACTIONS);
        if (net == NULL) {
            fprintf(stderr, "make_puffernet failed\n");
            fclose(f);
            return 1;
        }
    }

    for (int ep = 0; ep < episodes; ep++) {
        c_reset(&env);
        bool done = false;
        for (int tick = 0; tick < env.max_steps && !done; tick++) {
            // Pre-step Log snapshot — used to detect what kind of termination.
            float pre_succ = env.log.success;
            float pre_coll = env.log.collision;
            float pre_to   = env.log.timeout;

            if (net) {
                forward_puffernet(net, env.observations, env.actions);
            } else if (mode == 0) { // random
                env.actions[0] = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
                env.actions[1] = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
            } else if (mode == 1) { // zero
                env.actions[0] = 0.0f;
                env.actions[1] = 0.0f;
            } else { // fixed: gentle forward, slight turn
                env.actions[0] = 0.3f;
                env.actions[1] = 0.1f;
            }

            // Snapshot pre-step state for the row dump.
            Row row = {0};
            row.ep = ep; row.tick = tick;
            row.rx = env.robot.x; row.ry = env.robot.y;
            row.theta = env.robot.theta;
            row.v = env.robot.v; row.w = env.robot.w;
            row.gx = env.goal_x; row.gy = env.goal_y;
            row.n_obs = env.num_obstacles;
            for (int i = 0; i < MAX_OBSTACLES; i++) {
                if (i < env.num_obstacles) {
                    row.ox[i] = env.obs_x[i];
                    row.oy[i] = env.obs_y[i];
                } else {
                    row.ox[i] = NAN;
                    row.oy[i] = NAN;
                }
            }

            c_step(&env);

            int outcome = 0;
            if      (env.log.success   > pre_succ + 0.5f) outcome = 1;
            else if (env.log.collision > pre_coll + 0.5f) outcome = 2;
            else if (env.log.timeout   > pre_to   + 0.5f) outcome = 3;
            done = (outcome != 0);
            row.outcome = outcome;
            row.reward = env.rewards[0];
            row.done = done ? 1 : 0;
            write_row(f, &row);
        }
    }

    free_allocated(&env);
    fclose(f);
    (void)weights; (void)net;
    return 0;
}

int main(int argc, char** argv) {
    const char* csv = NULL;
    const char* load = NULL;
    int mode = 0;       // 0=random 1=zero 2=fixed
    int episodes = 1;
    unsigned int seed = 42;
    float arena_size = DEFAULT_ARENA_SIZE;
    int max_steps = DEFAULT_MAX_STEPS;
    float dt = 0.1f;
    int n_obs_min = 5, n_obs_max = 10;
    int order_min = 1, order_max = 2;
    float speed_min = 0.5f, speed_max = 1.0f;
    float std_min = 0.01f, std_max = 0.1f;
    float goal_radius = 0.5f;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--csv") && i+1 < argc)              csv = argv[++i];
        else if (!strcmp(argv[i], "--load") && i+1 < argc)             load = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i+1 < argc) {
            const char* m = argv[++i];
            if (!strcmp(m, "zero")) mode = 1;
            else if (!strcmp(m, "fixed")) mode = 2;
            else mode = 0;
        }
        else if (!strcmp(argv[i], "--episodes") && i+1 < argc)         episodes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i+1 < argc)             seed = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--arena") && i+1 < argc)            arena_size = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-steps") && i+1 < argc)        max_steps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dt") && i+1 < argc)               dt = atof(argv[++i]);
        else if (!strcmp(argv[i], "--min-obstacles") && i+1 < argc)    n_obs_min = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-obstacles") && i+1 < argc)    n_obs_max = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--min-order") && i+1 < argc)        order_min = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-order") && i+1 < argc)        order_max = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--min-speed") && i+1 < argc)        speed_min = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-speed") && i+1 < argc)        speed_max = atof(argv[++i]);
        else if (!strcmp(argv[i], "--min-std") && i+1 < argc)          std_min = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-std") && i+1 < argc)          std_max = atof(argv[++i]);
        else if (!strcmp(argv[i], "--goal-radius") && i+1 < argc)      goal_radius = atof(argv[++i]);
    }

    if (csv) {
        return main_csv_mode(csv, mode, load, episodes, seed,
                             arena_size, max_steps, dt,
                             n_obs_min, n_obs_max,
                             order_min, order_max,
                             speed_min, speed_max,
                             std_min,   std_max,
                             goal_radius);
    }

    // Interactive raylib mode
    srand(seed);
    DynaTrain env = make_env(seed, arena_size, max_steps, dt,
                             n_obs_min, n_obs_max,
                             order_min, order_max,
                             speed_min, speed_max,
                             std_min,   std_max,
                             goal_radius);
    allocate(&env);
    c_reset(&env);
    c_render(&env);
    while (!WindowShouldClose()) {
        env.actions[0] = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        env.actions[1] = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        c_step(&env);
        c_render(&env);
    }
    c_close(&env);
    free_allocated(&env);
    return 0;
}
