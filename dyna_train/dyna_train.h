// dyna_train.h — Procedural DynaBARN-style env with curriculum knobs.
//
// What it is
// ----------
//  - 20×20 m walled arena (matches DynaBARN).
//  - Differential-drive Jackal (shared/jackal.h).
//  - N dynamic cylinder obstacles, each on a polynomial-fit waypoint trajectory.
//  - Random start + random goal per episode (forces generalisation; opposite of
//    DynaBARN's fixed-route eval).
//  - 720-beam LiDAR observation (shared/lidar.h), 270° FOV, 30 m range.
//  - Obs = [lidar(720) | goal_dx_body | goal_dy_body] = 722 floats.
//  - Action = (v, w), v ∈ [-0.5, 2.0] m/s, w ∈ [-π, π] rad/s.
//  - Reward = Δ-distance shaping + Gaussian repulsion (closest LiDAR return) +
//             success bonus − collision penalty (collision terminates).
//
// Curriculum knobs (set from .ini via binding):
//   num_obstacles_min/max     per-episode obstacle count range
//   order_min/max             polynomial-fit degree range
//   speed_min/max             per-segment mean speed (m/s)
//   std_min/max               per-segment speed stddev
//
// Reward knobs:
//   gamma_d                   Δ-distance weight (default 1.0)
//   beta                      Gaussian repulsion weight (default 1.0)
//   sigma_o                   repulsion lengthscale (default 2.0 m)
//   success_bonus             terminal +reward on goal (default 1.0)
//   collision_penalty         terminal -reward on collision (default 10.0)
//
// Termination: goal reached, collision (robot footprint within collision_radius
// of any obstacle or wall), or timeout at max_steps.
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
#include "../shared/motion.h"

#define MAX_OBSTACLES 30
#define OBS_DIM (LIDAR_BEAMS + 2)            // 720 + (goal_dx_body, goal_dy_body)
#define COLLISION_DIST (JACKAL_RADIUS + OBSTACLE_RADIUS)   // 0.30 + 0.5 = 0.80 m
#define DEFAULT_ARENA_SIZE 20.0f
#define DEFAULT_MAX_STEPS 600

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

typedef struct {
    Log log;
    float* observations;
    float* actions;
    float* rewards;
    float* terminals;
    int num_agents;

    // Robot state
    JackalState robot;

    // Goal
    float goal_x, goal_y;

    // Obstacles: trajectories + currently-evaluated positions
    int num_obstacles;
    ObstacleTrajectory traj[MAX_OBSTACLES];
    float obs_x[MAX_OBSTACLES];
    float obs_y[MAX_OBSTACLES];

    // Episode state
    int tick;
    float sim_time;       // seconds since reset
    float prev_dist;
    float episode_return;

    // Config (set via binding/kwargs)
    float arena_size;
    int   max_steps;
    float dt;
    int   num_obstacles_min, num_obstacles_max;
    int   order_min, order_max;
    float speed_min, speed_max;
    float std_min,   std_max;
    float min_init_goal_dist;
    float gamma_d;
    float beta;
    float sigma_o;
    float success_bonus;
    float collision_penalty;
    float goal_radius;

    // Motion-family mixture (each weight is sampled uniformly from a Categorical)
    int   mw_poly, mw_linear, mw_reciprocating, mw_sinusoidal, mw_random_walk, mw_stationary;
    // Sinusoidal / random-walk knobs
    float amp_max;
    float freq_min, freq_max;
    float walk_step_std;
    float reciprocate_min_dist;
    // Robot-velocity caps applied to the policy output BEFORE jackal_step.
    // Defaults: 0.5 m/s, matching the paper-eval move_base local-planner
    // max_vel_x (so the policy trains under the same kinematic envelope it
    // will be evaluated in). Set to 0 (or any value ≥ JACKAL_V_MAX) to
    // effectively disable the cap.
    float train_v_max;        // m/s, forward cap
    float train_v_min;        // m/s, reverse cap (negative number)
    float train_w_max;        // rad/s, angular cap

    unsigned int rng;
    bool window_ready;
} DynaTrain;

// ============================================================================
// Internal helpers
// ============================================================================
static inline float robot_to_goal_dist(const DynaTrain* env) {
    float dx = env->goal_x - env->robot.x;
    float dy = env->goal_y - env->robot.y;
    return sqrtf(dx*dx + dy*dy);
}

static inline bool obstacle_collision(const DynaTrain* env) {
    float cd2 = COLLISION_DIST * COLLISION_DIST;
    for (int i = 0; i < env->num_obstacles; i++) {
        float dx = env->obs_x[i] - env->robot.x;
        float dy = env->obs_y[i] - env->robot.y;
        if (dx*dx + dy*dy < cd2) return true;
    }
    return false;
}

static inline bool wall_collision(const DynaTrain* env) {
    float half = 0.5f * env->arena_size - JACKAL_RADIUS;
    return fabsf(env->robot.x) > half || fabsf(env->robot.y) > half;
}

static inline float closest_obstacle_dist(const DynaTrain* env) {
    float best = 1e9f;
    for (int i = 0; i < env->num_obstacles; i++) {
        float dx = env->obs_x[i] - env->robot.x;
        float dy = env->obs_y[i] - env->robot.y;
        float d2 = dx*dx + dy*dy;
        if (d2 < best) best = d2;
    }
    return sqrtf(best);
}

static inline void update_obstacles(DynaTrain* env) {
    for (int i = 0; i < env->num_obstacles; i++) {
        obstacle_position(&env->traj[i], env->sim_time,
                          &env->obs_x[i], &env->obs_y[i]);
    }
}

static inline void compute_obs(DynaTrain* env) {
    float ranges[LIDAR_BEAMS];
    lidar_scan(&env->robot, 0.5f * env->arena_size,
               env->obs_x, env->obs_y, env->num_obstacles, ranges);
    // Normalize to [0, 1] by max range — keeps obs scale bounded.
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

// Sample a valid start/goal pair that:
//   (a) both lie inside the arena (minus a footprint margin)
//   (b) are at least min_init_goal_dist apart
// Tries up to 100 times then accepts whatever it has.
static inline void sample_start_goal(DynaTrain* env) {
    float half = 0.5f * env->arena_size - JACKAL_RADIUS - 0.5f;
    float min_d2 = env->min_init_goal_dist * env->min_init_goal_dist;
    for (int tries = 0; tries < 100; tries++) {
        env->robot.x = rand_uniformf(&env->rng, -half, half);
        env->robot.y = rand_uniformf(&env->rng, -half, half);
        env->goal_x  = rand_uniformf(&env->rng, -half, half);
        env->goal_y  = rand_uniformf(&env->rng, -half, half);
        float dx = env->goal_x - env->robot.x;
        float dy = env->goal_y - env->robot.y;
        if (dx*dx + dy*dy >= min_d2) return;
    }
}

// ============================================================================
// PufferLib lifecycle
// ============================================================================
void init(DynaTrain* env) {
    env->tick = 0;
    env->sim_time = 0.0f;
    env->window_ready = false;
    memset(&env->log, 0, sizeof(Log));
}

void allocate(DynaTrain* env) {
    init(env);
    env->observations = (float*)calloc(OBS_DIM, sizeof(float));
    env->actions = (float*)calloc(2, sizeof(float));
    env->rewards = (float*)calloc(1, sizeof(float));
    env->terminals = (float*)calloc(1, sizeof(float));
}

void free_allocated(DynaTrain* env) {
    free(env->observations);
    free(env->actions);
    free(env->rewards);
    free(env->terminals);
}

void c_reset(DynaTrain* env) {
    env->tick = 0;
    env->sim_time = 0.0f;
    env->episode_return = 0.0f;
    env->robot.v = 0.0f;
    env->robot.w = 0.0f;
    env->robot.theta = rand_uniformf(&env->rng, -(float)M_PI, (float)M_PI);

    // Sample obstacle count for the episode.
    int lo = env->num_obstacles_min, hi = env->num_obstacles_max;
    if (lo < 0) lo = 0;
    if (hi > MAX_OBSTACLES) hi = MAX_OBSTACLES;
    if (hi < lo) hi = lo;
    if (lo == hi) {
        env->num_obstacles = lo;
    } else {
        env->num_obstacles = lo + (rand_r(&env->rng) % (hi - lo + 1));
    }

    // Sample start + goal.
    sample_start_goal(env);

    // Build per-episode MotionParams + family weights from env config.
    MotionParams mp = motion_default_params();
    mp.speed_min            = env->speed_min;
    mp.speed_max            = env->speed_max;
    mp.order_min            = env->order_min;
    mp.order_max            = env->order_max;
    mp.std_min              = env->std_min;
    mp.std_max              = env->std_max;
    mp.amp_max              = env->amp_max;
    mp.freq_min             = env->freq_min;
    mp.freq_max             = env->freq_max;
    mp.walk_step_std        = env->walk_step_std;
    mp.reciprocate_min_dist = env->reciprocate_min_dist;

    int weights[MOTION_FAMILY_COUNT] = {
        env->mw_poly,
        env->mw_linear,
        env->mw_reciprocating,
        env->mw_sinusoidal,
        env->mw_random_walk,
        env->mw_stationary,
    };

    // Generate one trajectory per obstacle: pick a family per obstacle, then
    // generate. Retry up to 8 times each (polynomial fits can fail).
    for (int i = 0; i < env->num_obstacles; i++) {
        bool ok = false;
        for (int retry = 0; retry < 8 && !ok; retry++) {
            MotionFamily fam = motion_pick_family(&env->rng, weights);
            ok = motion_generate(&env->traj[i], fam, &env->rng, &mp);
        }
        if (!ok) {
            // Stationary-at-random fallback if everything failed.
            env->traj[i].num_waypoints = 1;
            env->traj[i].t[0] = 0.0f;
            env->traj[i].x[0] = rand_uniformf(&env->rng,
                -0.5f * env->arena_size + 1.0f, 0.5f * env->arena_size - 1.0f);
            env->traj[i].y[0] = rand_uniformf(&env->rng,
                -0.5f * env->arena_size + 1.0f, 0.5f * env->arena_size - 1.0f);
        }
    }

    update_obstacles(env);
    env->prev_dist = robot_to_goal_dist(env);
    compute_obs(env);
}

void c_step(DynaTrain* env) {
    env->tick++;
    env->sim_time += env->dt;

    // Action: [a_v, a_w] expected in normalised ranges. We accept either:
    //   convention A: a_v ∈ [-1, 1] mapped to [V_MIN, V_MAX] — symmetric;
    //   convention B: a_v ∈ [-1, 1] but clipped+rescaled to [V_MIN, V_MAX].
    // We use B: a∈[-1,1] → v = ((a+1)/2)·(V_MAX-V_MIN) + V_MIN. That gives the
    // policy a unit-square action space and the env handles physical mapping.
    float a0 = env->actions[0];
    float a1 = env->actions[1];
    if (!isfinite(a0)) a0 = 0.0f;
    if (!isfinite(a1)) a1 = 0.0f;
    a0 = clampf(a0, -1.0f, 1.0f);
    a1 = clampf(a1, -1.0f, 1.0f);
    // Map normalized action ∈ [-1, 1]² to physical velocity envelope.
    float v_lo = env->train_v_min;
    float v_hi = env->train_v_max;
    if (v_hi <= 0.0f) v_hi = JACKAL_V_MAX;   // 0 → "no cap" (fallback)
    float w_hi = env->train_w_max;
    if (w_hi <= 0.0f) w_hi = JACKAL_W_MAX;
    float v_cmd = v_lo + 0.5f * (a0 + 1.0f) * (v_hi - v_lo);
    float w_cmd = a1 * w_hi;

    jackal_step(&env->robot, v_cmd, w_cmd, env->dt);

    // Clamp robot to the inner-arena box (footprint-aware).
    float half = 0.5f * env->arena_size - JACKAL_RADIUS;
    env->robot.x = clampf(env->robot.x, -half, half);
    env->robot.y = clampf(env->robot.y, -half, half);

    update_obstacles(env);

    float dist = robot_to_goal_dist(env);

    // Reward
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
        c_reset(env);
    } else {
        compute_obs(env);
    }
}

// ============================================================================
// Render
// ============================================================================
void c_render(DynaTrain* env) {
    if (!env->window_ready) {
        InitWindow(WIDTH, HEIGHT, "PufferLib DynaTrain");
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

    // Arena
    DrawRectangleLines(
        (int)(cx - arena_half * scale),
        (int)(cy - arena_half * scale),
        (int)(env->arena_size * scale),
        (int)(env->arena_size * scale),
        PUFF_CYAN);

    // Future obstacle paths (last 50 waypoints for debug)
    for (int i = 0; i < env->num_obstacles; i++) {
        const ObstacleTrajectory* tr = &env->traj[i];
        for (int j = 1; j < tr->num_waypoints; j++) {
            DrawLineEx(
                (Vector2){cx + tr->x[j-1] * scale, cy + tr->y[j-1] * scale},
                (Vector2){cx + tr->x[j]   * scale, cy + tr->y[j]   * scale},
                1.0f, PUFF_DIM);
        }
    }
    // Current obstacle positions
    for (int i = 0; i < env->num_obstacles; i++) {
        DrawCircle(
            (int)(cx + env->obs_x[i] * scale),
            (int)(cy + env->obs_y[i] * scale),
            OBSTACLE_RADIUS * scale, PUFF_RED);
    }

    // Goal
    DrawCircleLines(
        (int)(cx + env->goal_x * scale),
        (int)(cy + env->goal_y * scale),
        env->goal_radius * scale * 4.0f, PUFF_GREEN);
    DrawCircle(
        (int)(cx + env->goal_x * scale),
        (int)(cy + env->goal_y * scale),
        4.0f, PUFF_GREEN);

    // Robot + heading
    int rx = (int)(cx + env->robot.x * scale);
    int ry = (int)(cy + env->robot.y * scale);
    DrawCircle(rx, ry, JACKAL_RADIUS * scale, PUFF_CYAN);
    int hx = (int)(rx + 14.0f * cosf(env->robot.theta));
    int hy = (int)(ry + 14.0f * sinf(env->robot.theta));
    DrawLineEx((Vector2){rx, ry}, (Vector2){hx, hy}, 3.0f, PUFF_WHITE);

    // HUD
    DrawText(TextFormat("Step: %i  T: %.1fs  Return: %.2f",
             env->tick, env->sim_time, env->episode_return),
             10, 10, 18, PUFF_WHITE);
    DrawText(TextFormat("Robot: (%.2f, %.2f) th=%.2f  v=%.2f  w=%.2f",
             env->robot.x, env->robot.y, env->robot.theta,
             env->robot.v, env->robot.w),
             10, 32, 18, PUFF_WHITE);
    DrawText(TextFormat("Goal d: %.2f  Obs N: %d",
             robot_to_goal_dist(env), env->num_obstacles),
             10, 54, 18, PUFF_WHITE);
    EndDrawing();
}

void c_close(DynaTrain* env) {
    if (env->window_ready) {
        CloseWindow();
        env->window_ready = false;
    }
}
