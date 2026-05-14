// dyna_eval.h — DynaBARN-style evaluation env.
//
// Differences from dyna_train:
//   * Fixed start (0, 11), fixed goal (0, -9), matching the paper §III-D.
//   * Difficulty bin selected by `difficulty` config (0=easy, 1=medium, 2=hard).
//     The bin determines (num_obstacles range, polynomial-order range, speed
//     range, std range) per Table I + Fig. 2 of the paper.
//   * No reward shaping during eval; rewards are still computed but the eval
//     pipeline ignores them — only `success`, `collision`, `timeout` matter.
//   * `world_seed`: when ≥ 0, deterministically initialise the per-episode RNG
//     with `seed + episode_index` (allows reproducing specific worlds).
#pragma once

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include "raylib.h"

#include "../shared/jackal.h"
#include "../shared/obstacle.h"
#include "../shared/lidar.h"
#include "../shared/traj_gen.h"
#include "../shared/world_loader.h"

#define MAX_OBSTACLES 30
#define OBS_DIM (LIDAR_BEAMS + 2)
#define COLLISION_DIST (JACKAL_RADIUS + OBSTACLE_RADIUS)
#define DEFAULT_ARENA_SIZE 20.0f
#define DEFAULT_MAX_STEPS 600

#define EVAL_START_X  0.0f
#define EVAL_START_Y  9.0f       // Paper says (0, 11) but arena is [-10, 10]² — clamp to 9 so robot footprint fits inside the wall.
#define EVAL_GOAL_X   0.0f
#define EVAL_GOAL_Y  -9.0f

#define WIDTH 720
#define HEIGHT 720

static const Color PUFF_RED        = (Color){187, 0, 0, 255};
static const Color PUFF_CYAN       = (Color){0, 187, 187, 255};
static const Color PUFF_GREEN      = (Color){0, 187, 0, 255};
static const Color PUFF_WHITE      = (Color){241, 241, 241, 241};
static const Color PUFF_BACKGROUND = (Color){6, 24, 24, 255};
static const Color PUFF_DIM        = (Color){80, 30, 30, 255};

typedef struct {
    float perf;
    float score;
    float episode_return;
    float episode_length;
    float success;
    float collision;
    float timeout;
    float n;
} Log;

// Difficulty bin parameters (paper Table I + Fig. 2).
typedef struct {
    int   n_obs_min, n_obs_max;
    int   order_min, order_max;
    float speed_min, speed_max;
    float std_min,   std_max;
} DifficultyBin;

static inline DifficultyBin difficulty_bin(int difficulty) {
    // The Fig. 2 tree picks one of (count, motion) combos per overall bin.
    // For a single-difficulty eval env we pick one representative combo:
    //   easy   = low-count + easy motion
    //   medium = low-count + hard motion  (alternate: high-count + easy motion;
    //                                      we choose the harder-motion option)
    //   hard   = high-count + hard motion
    switch (difficulty) {
    case 0: return (DifficultyBin){5, 10, 1, 2, 0.5f, 1.0f, 0.01f, 0.1f};
    case 1: return (DifficultyBin){5, 10, 3, 4, 1.0f, 2.0f, 0.10f, 0.2f};
    case 2: default:
            return (DifficultyBin){10, 20, 3, 4, 1.0f, 2.0f, 0.10f, 0.2f};
    }
}

typedef struct {
    Log log;
    float* observations;
    float* actions;
    float* rewards;
    float* terminals;
    int num_agents;

    JackalState robot;
    float goal_x, goal_y;

    int num_obstacles;
    ObstacleTrajectory traj[MAX_OBSTACLES];
    float obs_x[MAX_OBSTACLES];
    float obs_y[MAX_OBSTACLES];

    int tick;
    float sim_time;
    float prev_dist;
    float episode_return;
    int episode_idx;       // 0-indexed counter; useful when world_seed_base ≥ 0

    // Config
    float arena_size;
    int   max_steps;
    float dt;
    int   difficulty;            // 0/1/2 (ignored when world_file_set)
    int   world_seed_base;       // ≥0 → deterministic; <0 → use rng directly
    float gamma_d, beta, sigma_o;
    float success_bonus, collision_penalty;
    float goal_radius;

    // Baked-world override. If world_file_set != 0, c_reset loads the
    // trajectories from `world_file_path` and bypasses traj_gen.
    int   world_file_set;
    char  world_file_path[512];

    unsigned int rng;
    bool window_ready;
} DynaEval;

static inline float robot_to_goal_dist(const DynaEval* env) {
    float dx = env->goal_x - env->robot.x;
    float dy = env->goal_y - env->robot.y;
    return sqrtf(dx*dx + dy*dy);
}

static inline bool obstacle_collision(const DynaEval* env) {
    float cd2 = COLLISION_DIST * COLLISION_DIST;
    for (int i = 0; i < env->num_obstacles; i++) {
        float dx = env->obs_x[i] - env->robot.x;
        float dy = env->obs_y[i] - env->robot.y;
        if (dx*dx + dy*dy < cd2) return true;
    }
    return false;
}

static inline bool wall_collision(const DynaEval* env) {
    float half = 0.5f * env->arena_size - JACKAL_RADIUS;
    return fabsf(env->robot.x) > half || fabsf(env->robot.y) > half;
}

static inline float closest_obstacle_dist(const DynaEval* env) {
    float best = 1e9f;
    for (int i = 0; i < env->num_obstacles; i++) {
        float dx = env->obs_x[i] - env->robot.x;
        float dy = env->obs_y[i] - env->robot.y;
        float d2 = dx*dx + dy*dy;
        if (d2 < best) best = d2;
    }
    return sqrtf(best);
}

static inline void update_obstacles(DynaEval* env) {
    for (int i = 0; i < env->num_obstacles; i++) {
        obstacle_position(&env->traj[i], env->sim_time,
                          &env->obs_x[i], &env->obs_y[i]);
    }
}

static inline void compute_obs(DynaEval* env) {
    float ranges[LIDAR_BEAMS];
    lidar_scan(&env->robot, 0.5f * env->arena_size,
               env->obs_x, env->obs_y, env->num_obstacles, ranges);
    for (int i = 0; i < LIDAR_BEAMS; i++) {
        env->observations[i] = ranges[i] / LIDAR_RANGE;
    }
    float gx_b, gy_b;
    to_body_frame(env->goal_x - env->robot.x,
                  env->goal_y - env->robot.y,
                  env->robot.theta, &gx_b, &gy_b);
    env->observations[LIDAR_BEAMS]     = gx_b;
    env->observations[LIDAR_BEAMS + 1] = gy_b;
}

void init(DynaEval* env) {
    env->tick = 0;
    env->sim_time = 0.0f;
    env->episode_idx = 0;
    env->window_ready = false;
    memset(&env->log, 0, sizeof(Log));
}

void allocate(DynaEval* env) {
    init(env);
    env->observations = (float*)calloc(OBS_DIM, sizeof(float));
    env->actions = (float*)calloc(2, sizeof(float));
    env->rewards = (float*)calloc(1, sizeof(float));
    env->terminals = (float*)calloc(1, sizeof(float));
}

void free_allocated(DynaEval* env) {
    free(env->observations);
    free(env->actions);
    free(env->rewards);
    free(env->terminals);
}

void c_reset(DynaEval* env) {
    // Deterministic per-episode seeding for repeatability.
    if (env->world_seed_base >= 0) {
        env->rng = (unsigned)(env->world_seed_base + env->episode_idx);
    }

    env->tick = 0;
    env->sim_time = 0.0f;
    env->episode_return = 0.0f;
    env->robot.x = EVAL_START_X;
    env->robot.y = EVAL_START_Y;
    env->robot.theta = -0.5f * (float)M_PI;   // facing −y, toward the goal
    env->robot.v = 0.0f;
    env->robot.w = 0.0f;
    env->goal_x = EVAL_GOAL_X;
    env->goal_y = EVAL_GOAL_Y;

    if (env->world_file_set) {
        int n = world_loader_load(env->world_file_path, env->traj, MAX_OBSTACLES);
        if (n < 0) {
            fprintf(stderr, "dyna_eval: failed to load %s; using empty world\n",
                    env->world_file_path);
            n = 0;
        }
        env->num_obstacles = n;
    } else {
        DifficultyBin bin = difficulty_bin(env->difficulty);
        int lo = bin.n_obs_min, hi = bin.n_obs_max;
        if (hi > MAX_OBSTACLES) hi = MAX_OBSTACLES;
        if (hi < lo) hi = lo;
        env->num_obstacles = (lo == hi)
            ? lo
            : (lo + (rand_r(&env->rng) % (hi - lo + 1)));

        for (int i = 0; i < env->num_obstacles; i++) {
            bool ok = false;
            for (int retry = 0; retry < 8 && !ok; retry++) {
                ok = traj_generate(&env->traj[i], &env->rng,
                                   bin.order_min, bin.order_max,
                                   bin.speed_min, bin.speed_max,
                                   bin.std_min,   bin.std_max,
                                   0.05f);
            }
            if (!ok) {
                env->traj[i].num_waypoints = 1;
                env->traj[i].t[0] = 0.0f;
                env->traj[i].x[0] = rand_uniformf(&env->rng,
                    -0.5f * env->arena_size + 1.0f, 0.5f * env->arena_size - 1.0f);
                env->traj[i].y[0] = rand_uniformf(&env->rng,
                    -0.5f * env->arena_size + 1.0f, 0.5f * env->arena_size - 1.0f);
            }
        }
    }

    update_obstacles(env);
    env->prev_dist = robot_to_goal_dist(env);
    compute_obs(env);
}

void c_step(DynaEval* env) {
    env->tick++;
    env->sim_time += env->dt;

    float a0 = env->actions[0];
    float a1 = env->actions[1];
    if (!isfinite(a0)) a0 = 0.0f;
    if (!isfinite(a1)) a1 = 0.0f;
    a0 = clampf(a0, -1.0f, 1.0f);
    a1 = clampf(a1, -1.0f, 1.0f);
    float v_cmd = JACKAL_V_MIN + 0.5f * (a0 + 1.0f) * (JACKAL_V_MAX - JACKAL_V_MIN);
    float w_cmd = a1 * JACKAL_W_MAX;

    jackal_step(&env->robot, v_cmd, w_cmd, env->dt);
    float half = 0.5f * env->arena_size - JACKAL_RADIUS;
    env->robot.x = clampf(env->robot.x, -half, half);
    env->robot.y = clampf(env->robot.y, -half, half);

    update_obstacles(env);

    float dist = robot_to_goal_dist(env);
    float r = env->gamma_d * (env->prev_dist - dist);
    float d_obs = closest_obstacle_dist(env);
    if (d_obs < 1e3f) {
        float sig2 = env->sigma_o * env->sigma_o;
        r -= env->beta * expf(-(d_obs * d_obs) / sig2);
    }

    bool reached = (dist < env->goal_radius);
    bool collided = obstacle_collision(env) || wall_collision(env);
    bool truncated = (env->tick >= env->max_steps);
    if (reached) r += env->success_bonus;
    if (collided) r -= env->collision_penalty;

    env->rewards[0] = r;
    env->episode_return += r;
    env->terminals[0] = (reached || collided) ? 1.0f : 0.0f;
    env->prev_dist = dist;

    if (reached || collided || truncated) {
        env->log.perf            += reached ? 1.0f : 0.0f;
        env->log.success         += reached ? 1.0f : 0.0f;
        env->log.collision       += collided ? 1.0f : 0.0f;
        env->log.timeout         += (truncated && !reached && !collided) ? 1.0f : 0.0f;
        env->log.episode_length  += env->tick;
        env->log.episode_return  += env->episode_return;
        env->log.score           += env->episode_return;
        env->log.n               += 1.0f;
        env->episode_idx++;
        c_reset(env);
    } else {
        compute_obs(env);
    }
}

void c_render(DynaEval* env) {
    if (!env->window_ready) {
        InitWindow(WIDTH, HEIGHT, "PufferLib DynaEval");
        SetTargetFPS(60);
        env->window_ready = true;
    }
    if (IsKeyDown(KEY_ESCAPE)) exit(0);

    BeginDrawing();
    ClearBackground(PUFF_BACKGROUND);

    float scale = WIDTH / env->arena_size;
    float arena_half = 0.5f * env->arena_size;
    float cx = WIDTH * 0.5f;
    float cy = HEIGHT * 0.5f;

    DrawRectangleLines(
        (int)(cx - arena_half * scale),
        (int)(cy - arena_half * scale),
        (int)(env->arena_size * scale),
        (int)(env->arena_size * scale),
        PUFF_CYAN);

    for (int i = 0; i < env->num_obstacles; i++) {
        const ObstacleTrajectory* tr = &env->traj[i];
        for (int j = 1; j < tr->num_waypoints; j++) {
            DrawLineEx(
                (Vector2){cx + tr->x[j-1] * scale, cy + tr->y[j-1] * scale},
                (Vector2){cx + tr->x[j]   * scale, cy + tr->y[j]   * scale},
                1.0f, PUFF_DIM);
        }
    }
    for (int i = 0; i < env->num_obstacles; i++) {
        DrawCircle(
            (int)(cx + env->obs_x[i] * scale),
            (int)(cy + env->obs_y[i] * scale),
            OBSTACLE_RADIUS * scale, PUFF_RED);
    }

    DrawCircleLines(
        (int)(cx + env->goal_x * scale),
        (int)(cy + env->goal_y * scale),
        env->goal_radius * scale * 4.0f, PUFF_GREEN);
    DrawCircle(
        (int)(cx + env->goal_x * scale),
        (int)(cy + env->goal_y * scale),
        4.0f, PUFF_GREEN);

    int rx = (int)(cx + env->robot.x * scale);
    int ry = (int)(cy + env->robot.y * scale);
    DrawCircle(rx, ry, JACKAL_RADIUS * scale, PUFF_CYAN);
    int hx = (int)(rx + 14.0f * cosf(env->robot.theta));
    int hy = (int)(ry + 14.0f * sinf(env->robot.theta));
    DrawLineEx((Vector2){rx, ry}, (Vector2){hx, hy}, 3.0f, PUFF_WHITE);

    const char* dn = "?";
    if (env->difficulty == 0) dn = "easy";
    else if (env->difficulty == 1) dn = "medium";
    else if (env->difficulty == 2) dn = "hard";
    DrawText(TextFormat("Difficulty: %s  Ep: %d  Step: %i  T: %.1fs",
             dn, env->episode_idx, env->tick, env->sim_time),
             10, 10, 18, PUFF_WHITE);
    DrawText(TextFormat("Robot: (%.2f, %.2f) th=%.2f  v=%.2f",
             env->robot.x, env->robot.y, env->robot.theta, env->robot.v),
             10, 32, 18, PUFF_WHITE);
    DrawText(TextFormat("Goal d: %.2f  Obs N: %d  Return: %.2f",
             robot_to_goal_dist(env), env->num_obstacles, env->episode_return),
             10, 54, 18, PUFF_WHITE);
    EndDrawing();
}

void c_close(DynaEval* env) {
    if (env->window_ready) {
        CloseWindow();
        env->window_ready = false;
    }
}
