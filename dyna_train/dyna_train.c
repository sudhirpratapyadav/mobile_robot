// dyna_train.c — Standalone driver: random/zero/fixed-action or policy inference.
// Built by `bash build.sh dyna_train --fast`. Interactive raylib mode runs
// random actions by default; with --traj it dumps trajectories (--csv is a
// back-compat alias).
//
// Env config is read from dyna_train.ini via shared/ini.h — the same file
// PufferLib's training path consumes, so the standalone always sees the
// same defaults the policy was trained against. CLI flags override per-knob.
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
#include "../shared/ini.h"

#define NUM_ACTIONS 2
#define HIDDEN_SIZE 128
#define NUM_LAYERS  2

// Default .ini path — relative to repo root. The standalone binary is built
// inside /puffertank/pufferlib so the absolute host path is the most reliable
// default; CLI --ini overrides it.
#define DEFAULT_INI_PATH "/puffertank/host/dyna_barn/dyna_train/dyna_train.ini"

// Build a DynaTrain from an INI file. All env knobs are read from the [env]
// section of `ini_path` (typically dyna_train/dyna_train.ini, the same file
// PufferLib's Python training path consumes). Missing keys fall back to the
// hard-coded defaults below; missing files fall back to all defaults (with a
// stderr warning) so the binary still runs but the user knows to fix it.
//
// CLI flags in main() can override any field post-construction — keeps the
// "ini = canonical config, CLI = ad-hoc overrides" precedence.
static DynaTrain make_env(unsigned int seed, const char* ini_path) {
    IniEntry entries[256];
    int n = ini_load(ini_path, entries, 256);
    if (n < 0) {
        fprintf(stderr, "make_env: could not open %s; using built-in defaults\n",
                ini_path);
        n = 0;
    }
    #define G_F(k, fb) ini_get_f(entries, n, "env", (k), (fb))
    #define G_I(k, fb) ini_get_i(entries, n, "env", (k), (fb))

    DynaTrain env = {
        .arena_size         = G_F("arena_size",         (float)DEFAULT_ARENA_SIZE),
        .max_steps          = G_I("max_steps",          DEFAULT_MAX_STEPS),
        .dt                 = G_F("dt",                 0.1f),
        .num_obstacles_min  = G_I("num_obstacles_min",  5),
        .num_obstacles_max  = G_I("num_obstacles_max",  10),
        .order_min          = G_I("order_min",          1),
        .order_max          = G_I("order_max",          2),
        .speed_min          = G_F("speed_min",          0.5f),
        .speed_max          = G_F("speed_max",          1.0f),
        .std_min            = G_F("std_min",            0.0f),
        .std_max            = G_F("std_max",            0.0f),
        .min_init_goal_dist = G_F("min_init_goal_dist", 12.0f),
        .gamma_d            = G_F("gamma_d",            0.0f),
        .beta               = G_F("beta",               0.0f),
        .sigma_o            = G_F("sigma_o",            2.0f),
        .success_bonus      = G_F("success_bonus",      0.0f),
        .collision_penalty  = G_F("collision_penalty",  0.0f),
        .goal_radius        = G_F("goal_radius",        0.5f),
        .goal_box_half      = G_F("goal_box_half",      0.3f),
        .time_penalty       = G_F("time_penalty",       0.0f),
        .alpha_g            = G_F("alpha_g",            0.0f),
        .sigma_g            = G_F("sigma_g",            5.0f),
        .alpha_short        = G_F("alpha_short",        0.3f),
        .sigma_short        = G_F("sigma_short",        2.5f),
        .alpha_med          = G_F("alpha_med",          0.3f),
        .sigma_med          = G_F("sigma_med",         10.0f),
        .alpha_long         = G_F("alpha_long",         0.4f),
        .sigma_long         = G_F("sigma_long",        20.0f),
        .mute_sigma_d       = G_F("mute_sigma_d",       1.0f),
        .mute_cone_kappa    = G_F("mute_cone_kappa",    1.0f),
        .mute_lambda        = G_F("mute_lambda",        0.0f),
        .alpha_ttc          = G_F("alpha_ttc",          0.0f),
        .tau_ttc            = G_F("tau_ttc",            1.0f),
        .gamma_fwd          = G_F("gamma_fwd",          0.0f),
        .alpha_wait         = G_F("alpha_wait",         0.0f),
        .sigma_v            = G_F("sigma_v",            0.5f),
        .open_side_mode     = G_I("open_side_mode",     0),
        .cur_open_side      = -1,
        .terminate_on_goal      = G_I("terminate_on_goal",      0),
        .terminate_on_collision = G_I("terminate_on_collision", 0),
        .mw_poly            = G_I("mw_poly",            1),
        .mw_linear          = G_I("mw_linear",          0),
        .mw_reciprocating   = G_I("mw_reciprocating",   0),
        .mw_sinusoidal      = G_I("mw_sinusoidal",      0),
        .mw_random_walk     = G_I("mw_random_walk",     0),
        .mw_stationary      = G_I("mw_stationary",      0),
        .amp_max            = G_F("amp_max",            3.0f),
        .freq_min           = G_F("freq_min",           0.05f),
        .freq_max           = G_F("freq_max",           0.5f),
        .walk_step_std      = G_F("walk_step_std",      0.4f),
        .reciprocate_min_dist = G_F("reciprocate_min_dist", 4.0f),
        .train_v_max        = G_F("train_v_max",        0.5f),
        .train_v_min        = G_F("train_v_min",       -0.5f),
        .train_w_max        = G_F("train_w_max",        (float)M_PI),
        .train_a_max        = G_F("train_a_max",        10.0f),
        .train_alpha_max    = G_F("train_alpha_max",    20.0f),
        .rng                = seed,
    };
    #undef G_F
    #undef G_I
    return env;
}

// Binary trajectory dump (one record per step):
//   header (4 ints): ep tick n_obs outcome
//   body:   rx ry theta v w gx gy
//           (ox,oy) × MAX_OBSTACLES   (NaN for unused slots)
//           reward done
//   episode-final (5 ints, zero on all but the final tick of each episode):
//           ep_reached ep_collided ep_clean_reach ep_strict ep_n_collisions
// All floats are 32-bit. Endianness = host (we read on the same machine).
//
// Magic value bumped to 0x44594E42 ("DYNB") when this format went live so
// older parsers fail loudly. bake_traj_parquet.py broadcasts the episode-
// final fields to all ticks of each episode so the parquet has them as
// per-tick columns (constant within an episode).
typedef struct {
    int   ep, tick, n_obs, outcome;
    float rx, ry, theta, v, w, gx, gy;
    float ox[MAX_OBSTACLES], oy[MAX_OBSTACLES];
    float reward;
    int   done;
    // Episode-final fields. Set only on the row where `done==1`; zero on
    // all other ticks of the same episode.
    int   ep_reached;          // robot touched the goal at least once
    int   ep_collided;         // at least one collision happened
    int   ep_clean_reach;      // reached AND no collision before first reach
    int   ep_strict;           // reached AND zero collisions in the whole episode
    int   ep_n_collisions;     // collision-event count this episode
} Row;

static void write_row(FILE* f, const Row* r) {
    fwrite(r, sizeof(Row), 1, f);
}

static int main_traj_mode(const char* path, int mode, const char* load_path,
                          int episodes, const DynaTrain* template_env) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "fopen %s\n", path); return 1; }
    // Header: 4 ints — magic, n_episodes, max_obstacles, lidar_beams.
    // Magic 0x44594E42 ('DYNB') marks the format that includes the 5
    // ep_* fields appended to Row (2026-05-17). Older 'DYNA' (0x44594E41)
    // readers will see a 20-byte size mismatch and fail loudly.
    int header[4] = {0x44594E42, episodes, MAX_OBSTACLES, LIDAR_BEAMS};
    fwrite(header, sizeof(int), 4, f);

    DynaTrain env = *template_env;
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
            // Pre-step Log snapshot. We detect what happened this tick by
            // diffing cumulative counters before/after c_step. c_step calls
            // c_reset internally at end-of-episode, which clears the latched
            // flags — so we can't read env.reached_once etc. after the step.
            // Diffing the log counters works because the env increments them
            // BEFORE the reset (see dyna_train.h:565-605).
            float pre_succ        = env.log.success;
            float pre_coll        = env.log.collision;
            float pre_to          = env.log.timeout;
            float pre_clean       = env.log.success_clean_reach;
            float pre_strict      = env.log.success_strict;
            float pre_n_coll_ev   = env.log.n_collision_events;

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
            // Episode-final telemetry: populate only on the row that closes
            // the episode. Derive from the diff of cumulative log counters
            // (which the env increments before its internal c_reset).
            if (done) {
                row.ep_reached      = (env.log.success            > pre_succ      + 0.5f) ? 1 : 0;
                row.ep_collided     = (env.log.collision          > pre_coll      + 0.5f) ? 1 : 0;
                row.ep_clean_reach  = (env.log.success_clean_reach > pre_clean    + 0.5f) ? 1 : 0;
                row.ep_strict       = (env.log.success_strict      > pre_strict   + 0.5f) ? 1 : 0;
                row.ep_n_collisions = (int)(env.log.n_collision_events - pre_n_coll_ev + 0.5f);
            }
            write_row(f, &row);
        }
    }

    free_allocated(&env);
    fclose(f);
    (void)weights; (void)net;
    return 0;
}

int main(int argc, char** argv) {
    // Config precedence: built-in fallback < INI < CLI override.
    // Step 1: build env from INI (or fallbacks). Step 2: apply CLI overrides
    // as direct field writes. Step 3: dispatch to traj-dump or interactive.
    const char* traj = NULL;
    const char* load = NULL;
    const char* ini_path = DEFAULT_INI_PATH;
    int mode = 0;       // 0=random 1=zero 2=fixed
    int episodes = 1;
    unsigned int seed = 42;

    // Pre-pass: pick up --ini and --seed first so make_env sees the right
    // file and the env RNG starts where the user asked.
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--ini")  && i+1 < argc) ini_path = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i+1 < argc) seed = (unsigned)atoi(argv[++i]);
    }
    DynaTrain env = make_env(seed, ini_path);

    // Main pass: parse the rest. Driver-only flags (traj/load/mode/episodes)
    // set local vars; env-config flags overwrite env fields in place.
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--ini")  && i+1 < argc) { i++; continue; }
        else if (!strcmp(argv[i], "--seed") && i+1 < argc) { i++; continue; }
        else if ((!strcmp(argv[i], "--traj") || !strcmp(argv[i], "--csv")) && i+1 < argc) traj = argv[++i];
        else if (!strcmp(argv[i], "--load") && i+1 < argc)             load = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i+1 < argc) {
            const char* m = argv[++i];
            if (!strcmp(m, "zero")) mode = 1;
            else if (!strcmp(m, "fixed")) mode = 2;
            else mode = 0;
        }
        else if (!strcmp(argv[i], "--episodes") && i+1 < argc)         episodes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--arena") && i+1 < argc)            env.arena_size = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-steps") && i+1 < argc)        env.max_steps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dt") && i+1 < argc)               env.dt = atof(argv[++i]);
        else if (!strcmp(argv[i], "--min-obstacles") && i+1 < argc)    env.num_obstacles_min = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-obstacles") && i+1 < argc)    env.num_obstacles_max = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--min-order") && i+1 < argc)        env.order_min = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-order") && i+1 < argc)        env.order_max = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--min-speed") && i+1 < argc)        env.speed_min = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-speed") && i+1 < argc)        env.speed_max = atof(argv[++i]);
        else if (!strcmp(argv[i], "--min-std") && i+1 < argc)          env.std_min = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-std") && i+1 < argc)          env.std_max = atof(argv[++i]);
        else if (!strcmp(argv[i], "--goal-radius") && i+1 < argc)      env.goal_radius = atof(argv[++i]);
        else if (!strcmp(argv[i], "--mute-sigma-d") && i+1 < argc)     env.mute_sigma_d = atof(argv[++i]);
        else if (!strcmp(argv[i], "--mute-cone-kappa") && i+1 < argc)  env.mute_cone_kappa = atof(argv[++i]);
        else if (!strcmp(argv[i], "--mute-lambda") && i+1 < argc)      env.mute_lambda = atof(argv[++i]);
        else if (!strcmp(argv[i], "--alpha-ttc") && i+1 < argc)        env.alpha_ttc = atof(argv[++i]);
        else if (!strcmp(argv[i], "--tau-ttc") && i+1 < argc)          env.tau_ttc = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gamma-fwd") && i+1 < argc)        env.gamma_fwd = atof(argv[++i]);
        else if (!strcmp(argv[i], "--alpha-wait") && i+1 < argc)       env.alpha_wait = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sigma-v") && i+1 < argc)          env.sigma_v = atof(argv[++i]);
        else if (!strcmp(argv[i], "--open-side-mode") && i+1 < argc)   env.open_side_mode = atoi(argv[++i]);
    }

    if (traj) {
        return main_traj_mode(traj, mode, load, episodes, &env);
    }

    // Interactive raylib mode — random-action sanity check of the env.
    srand(seed);
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
