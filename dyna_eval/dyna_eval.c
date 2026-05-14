// dyna_eval.c — Standalone driver for DynaBARN-style eval.
// CLI:
//   ./dyna_eval --csv out.bin --load ckpt.bin --difficulty {0|1|2}
//               --episodes N --world-seed-base S --max-steps K
// Random/zero baselines via --mode {random|zero|fixed}.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include "raylib.h"

#include "dyna_eval.h"
#include "puffernet.h"

#define NUM_ACTIONS 2
#define HIDDEN_SIZE 128
#define NUM_LAYERS  2

static DynaEval make_env(unsigned int seed, float arena_size, int max_steps,
                         float dt, int difficulty, int world_seed_base,
                         float goal_radius, const char* world_file) {
    DynaEval env = {
        .arena_size = arena_size,
        .max_steps = max_steps,
        .dt = dt,
        .difficulty = difficulty,
        .world_seed_base = world_seed_base,
        .gamma_d = 1.0f,
        .beta = 1.0f,
        .sigma_o = 2.0f,
        .success_bonus = 1.0f,
        .collision_penalty = 10.0f,
        .goal_radius = goal_radius,
        .rng = seed,
    };
    if (world_file && *world_file) {
        env.world_file_set = 1;
        strncpy(env.world_file_path, world_file, sizeof(env.world_file_path) - 1);
    }
    return env;
}

typedef struct {
    int   ep, tick, n_obs, outcome;
    float rx, ry, theta, v, w, gx, gy;
    float ox[MAX_OBSTACLES], oy[MAX_OBSTACLES];
    float reward;
    int   done;
} Row;

int main(int argc, char** argv) {
    const char* csv = NULL;
    const char* load = NULL;
    const char* world_file = NULL;
    int mode = 0;
    int episodes = 1;
    unsigned int seed = 42;
    int difficulty = 0;
    int world_seed_base = 0;       // 0 → deterministic from ep 0
    float arena_size = DEFAULT_ARENA_SIZE;
    int max_steps = DEFAULT_MAX_STEPS;
    float dt = 0.1f;
    float goal_radius = 0.5f;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--csv") && i+1 < argc)              csv = argv[++i];
        else if (!strcmp(argv[i], "--load") && i+1 < argc)             load = argv[++i];
        else if (!strcmp(argv[i], "--world-file") && i+1 < argc)       world_file = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i+1 < argc) {
            const char* m = argv[++i];
            if (!strcmp(m, "zero")) mode = 1;
            else if (!strcmp(m, "fixed")) mode = 2;
            else mode = 0;
        }
        else if (!strcmp(argv[i], "--episodes") && i+1 < argc)         episodes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i+1 < argc)             seed = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--difficulty") && i+1 < argc)       difficulty = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--world-seed-base") && i+1 < argc)  world_seed_base = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--arena") && i+1 < argc)            arena_size = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-steps") && i+1 < argc)        max_steps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dt") && i+1 < argc)               dt = atof(argv[++i]);
        else if (!strcmp(argv[i], "--goal-radius") && i+1 < argc)      goal_radius = atof(argv[++i]);
    }

    if (csv) {
        FILE* f = fopen(csv, "wb");
        if (!f) { fprintf(stderr, "fopen %s\n", csv); return 1; }
        int header[4] = {0x44594E45, episodes, MAX_OBSTACLES, LIDAR_BEAMS};
        fwrite(header, sizeof(int), 4, f);

        DynaEval env = make_env(seed, arena_size, max_steps, dt,
                                difficulty, world_seed_base, goal_radius,
                                world_file);
        allocate(&env);

        Weights* weights = NULL;
        PufferNet* net = NULL;
        if (load) {
            weights = load_weights(load);
            if (!weights) { fprintf(stderr, "load_weights failed: %s\n", load); fclose(f); return 1; }
            int logit_sizes[NUM_ACTIONS] = {1, 1};
            net = make_puffernet(weights, 1, OBS_DIM, HIDDEN_SIZE, NUM_LAYERS, logit_sizes, NUM_ACTIONS);
            if (!net) { fprintf(stderr, "make_puffernet failed\n"); fclose(f); return 1; }
        }

        for (int ep = 0; ep < episodes; ep++) {
            c_reset(&env);
            bool done = false;
            for (int tick = 0; tick < env.max_steps && !done; tick++) {
                float pre_succ = env.log.success;
                float pre_coll = env.log.collision;
                float pre_to   = env.log.timeout;

                if (net) {
                    forward_puffernet(net, env.observations, env.actions);
                } else if (mode == 0) {
                    env.actions[0] = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
                    env.actions[1] = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
                } else if (mode == 1) {
                    env.actions[0] = 0.0f;
                    env.actions[1] = 0.0f;
                } else {
                    env.actions[0] = 0.3f;
                    env.actions[1] = 0.0f;
                }

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
                fwrite(&row, sizeof(Row), 1, f);
            }
        }
        free_allocated(&env);
        fclose(f);
        (void)weights; (void)net;
        return 0;
    }

    // Interactive raylib mode
    srand(seed);
    DynaEval env = make_env(seed, arena_size, max_steps, dt,
                            difficulty, world_seed_base, goal_radius,
                            world_file);
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
