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
#include "../shared/costmap.h"

#define MAX_OBSTACLES 64   /* bumped 30→64 (2026-05-18) to enable proper
                              density-matching at arena=40 and above.
                              Does NOT change OBS_DIM (policy sees costmap+
                              extras only); only env-internal storage and
                              the trajectory dump Row size. */
// Observation mode — pick at compile time. To switch, change this define and
// rebuild (bash build.sh dyna_train + bash build.sh dyna_train --fast).
//   OBS_MODE_COSTMAP = 0 → 720-d LiDAR + (v, w, goal_dx_body, goal_dy_body)
//   OBS_MODE_COSTMAP = 1 → 64×64 body-frame costmap + (v, w, goal_dx_body, goal_dy_body)
#define OBS_MODE_COSTMAP 1
// Observation history. When >1, the env keeps a ring buffer of the last
// (HISTORY_LEN-1) costmaps and emits all HISTORY_LEN frames stacked
// channel-wise: [costmap_t, costmap_{t-1}, ..., costmap_{t-(H-1)}, extras].
// At episode start the buffer is zero-filled so older slots are blank for
// the first H-1 steps. Compile-time so the puffer binding's OBS_SIZE is
// known. Override at build time: -DHISTORY_LEN=2 etc.
#ifndef HISTORY_LEN
#  define HISTORY_LEN 1   /* axis 1.1 (HIST=2) failed @ 891sg5v4; back to 1 */
#endif
#if OBS_MODE_COSTMAP
#  define OBS_EXTRA 4   // (v, w, goal_dx_body, goal_dy_body)
#  define OBS_DIM (HISTORY_LEN * COSTMAP_SIZE + OBS_EXTRA)
#else
#  define OBS_EXTRA 4
#  define OBS_DIM (LIDAR_BEAMS + OBS_EXTRA)
#endif
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
    float success;             // ratio of episodes that ever touched the goal
    float success_strict;      // reached AND zero collisions in the whole episode
    float success_clean_reach; // reached AND no collisions BEFORE first goal touch
    float collision;           // ratio of episodes with at least one collision
    float timeout;             // 1 − success when no early termination
    float n;
    // Per-episode telemetry (sum across episodes; dashboard divides by n).
    float min_dist_to_goal;    // mean over episodes of the closest the robot got
    float final_dist_to_goal;  // mean over episodes of the final distance
    float n_collision_events;  // mean collision-event count per episode
    float closest_obstacle;    // mean of the closest obstacle distance ever seen
    float steps_at_goal;       // mean number of steps within the goal box
    // Collision-timing metrics (per-episode, mean over all episodes)
    //   dist_from_goal_at_first_collision
    //       = distance at first collision, or min-dist-seen if no collision
    //   max_steps_between_collisions
    //       = longest non-collision streak, or full episode length if no collision
    float dist_from_goal_at_first_collision;
    float max_steps_between_collisions;
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
    float goal_radius;        // Euclidean fallback (used iff goal_box_half ≤ 0)
    float goal_box_half;      // L∞ half-extent for "reached" — matches eval's
                              //   check_goal_node.py:arrival_gaol (paper = 0.3 m)
    // Reward extensions (added 2026-05-15 to fix the "do-nothing" collapse
    // observed in the 5B run with v_max=0.5 + accel limits).
    //   time_penalty:  constant -ve reward per step. (DEPRECATED but kept
    //                  for compat — set to 0 to disable.)
    //   alpha_g, sigma_g: single-scale goal-attraction Gaussian.
    //                  (DEPRECATED in favor of the 3-scale version below.)
    float time_penalty;
    float alpha_g;
    float sigma_g;
    // Multi-scale Gaussian goal-attraction (2026-05-16). Replaces the
    // previous Δ-distance + single-Gaussian shaping. Per step:
    //   r += alpha_short · exp(-(d/sigma_short)²)
    //      + alpha_med   · exp(-(d/sigma_med)²)
    //      + alpha_long  · exp(-(d/sigma_long)²)
    // The three scales are intended to give a reward gradient at all
    // distances: long covers the whole arena, medium is the close-approach
    // pull, short is the precise dock-to-goal signal.
    // Set any alpha_* to 0 to disable that scale.
    float alpha_short, sigma_short;
    float alpha_med,   sigma_med;
    float alpha_long,  sigma_long;
    // No-termination mode: when terminate_on_goal=0 the goal touch does NOT
    // end the episode (robot can dwell), and when terminate_on_collision=0
    // collisions do NOT end the episode (robot keeps going through).
    // collision_penalty is then applied PER EVENT (only on the step the
    // robot transitions from non-collision to collision), not every step.
    int   terminate_on_goal;
    int   terminate_on_collision;
    // Latched per-episode flags for logging — robot "ever touched goal"
    // and "ever collided." Reset in c_reset.
    bool  reached_once;
    bool  was_in_collision;     // edge-detect for per-event collision penalty
    bool  collided_once;
    bool  collided_before_reach;  // set if a collision happens before reached_once flips
    // Per-episode trackers for the new telemetry log channels.
    float ep_min_dist;          // min(dist) ever seen this episode
    float ep_min_obs_dist;      // min(closest_obstacle_dist) this episode
    int   ep_n_collision_events;
    int   ep_steps_at_goal;
    float ep_dist_at_first_coll;  // dist to goal at first collision (-1 if none yet)
    int   ep_last_coll_tick;      // tick of the most recent collision (-1 if none)
    int   ep_max_streak;          // longest run of consecutive non-collision steps so far

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
    // Acceleration limits (slew rate on commanded v / w). Matches the paper's
    // base_local_planner_params.yaml (acc_lim_x = 10, acc_lim_theta = 20).
    // Per step: |v_new - v_prev| <= train_a_max * dt;
    //           |w_new - w_prev| <= train_alpha_max * dt.
    // Set to <= 0 to disable.
    float train_a_max;        // m/s², linear-acceleration cap
    float train_alpha_max;    // rad/s², angular-acceleration cap

    unsigned int rng;
    bool window_ready;

    // Costmap-history ring buffer. Holds the last (HISTORY_LEN-1) costmaps;
    // the current one is computed fresh and prepended each step. Allocated
    // in allocate() to keep DynaTrain struct size constant across builds.
    // Index 0 = oldest, index (HISTORY_LEN-2) = previous. NULL when
    // HISTORY_LEN == 1 (no history).
    float* costmap_history;
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
    // Always compute the LiDAR scan first — needed by the costmap path too,
    // and the standalone driver may render it. Cheap.
    float ranges[LIDAR_BEAMS];
    lidar_scan(&env->robot, 0.5f * env->arena_size,
               env->obs_x, env->obs_y, env->num_obstacles, ranges);

#if OBS_MODE_COSTMAP
    // Egocentric occupancy grid (LingBot-style explicit-geometry obs).
    // HISTORY_LEN frames stacked. Layout:
    //   [costmap_t (4096) | costmap_{t-1} (4096) | ... | costmap_{t-(H-1)} | extras]
    // The current frame is written into observations[0:4096]. Older frames
    // (when HISTORY_LEN > 1) come from env->costmap_history (a ring of
    // HISTORY_LEN-1 buffers, oldest first). At episode start they're zero.
    costmap_rasterize(ranges, env->observations);
#if HISTORY_LEN > 1
    // Copy history slots into obs after the current frame.
    for (int h = 0; h < HISTORY_LEN - 1; h++) {
        // history[h] is the frame from h+1 steps ago (h=0 = previous,
        // h=H-2 = oldest). We emit them in age order: prev, prev-prev, ...
        memcpy(env->observations + (h + 1) * COSTMAP_SIZE,
               env->costmap_history + h * COSTMAP_SIZE,
               COSTMAP_SIZE * sizeof(float));
    }
    // Then shift the ring so the now-current costmap becomes the new
    // "previous" for next step. Move slot 0..H-3 → 1..H-2, write current into 0.
    if (HISTORY_LEN > 2) {
        memmove(env->costmap_history + COSTMAP_SIZE,
                env->costmap_history,
                (HISTORY_LEN - 2) * COSTMAP_SIZE * sizeof(float));
    }
    memcpy(env->costmap_history, env->observations,
           COSTMAP_SIZE * sizeof(float));
#endif
    int extras_off = HISTORY_LEN * COSTMAP_SIZE;
#else
    // Raw normalized LiDAR scan.
    for (int i = 0; i < LIDAR_BEAMS; i++) {
        env->observations[i] = ranges[i] / LIDAR_RANGE;
    }
    int extras_off = LIDAR_BEAMS;
#endif

    // Common extras: ego (v, w) and goal vector in body frame.
    float gx_b, gy_b;
    to_body_frame(env->goal_x - env->robot.x,
                  env->goal_y - env->robot.y,
                  env->robot.theta, &gx_b, &gy_b);
    env->observations[extras_off + 0] = env->robot.v;
    env->observations[extras_off + 1] = env->robot.w;
    env->observations[extras_off + 2] = gx_b;
    env->observations[extras_off + 3] = gy_b;
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
    // history buffer must be allocated here too: native PufferLib's
    // vecenv calls my_init (which calls init) but NOT allocate, since the
    // framework owns obs/actions/rewards itself. Anything env-internal
    // (like costmap_history) needs to live in init() to be ready before
    // the first c_step on either code path.
#if OBS_MODE_COSTMAP && HISTORY_LEN > 1
    env->costmap_history = (float*)calloc(
        (HISTORY_LEN - 1) * COSTMAP_SIZE, sizeof(float));
#else
    env->costmap_history = NULL;
#endif
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
    if (env->costmap_history) free(env->costmap_history);
}

void c_reset(DynaTrain* env) {
    env->tick = 0;
    env->sim_time = 0.0f;
    env->episode_return = 0.0f;
    env->robot.v = 0.0f;
    env->robot.w = 0.0f;
    env->robot.theta = rand_uniformf(&env->rng, -(float)M_PI, (float)M_PI);
    env->reached_once = false;
    env->was_in_collision = false;
    env->collided_once = false;
    env->collided_before_reach = false;
#if OBS_MODE_COSTMAP && HISTORY_LEN > 1
    if (env->costmap_history) {
        memset(env->costmap_history, 0,
               (HISTORY_LEN - 1) * COSTMAP_SIZE * sizeof(float));
    }
#endif
    env->ep_min_dist = 1e9f;
    env->ep_min_obs_dist = 1e9f;
    env->ep_n_collision_events = 0;
    env->ep_steps_at_goal = 0;
    env->ep_dist_at_first_coll = -1.0f;   // sentinel: "no collision yet"
    env->ep_last_coll_tick = -1;
    env->ep_max_streak = 0;

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
    mp.arena_half           = 0.5f * env->arena_size;
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
    mp.init_min_dist_from_robot = 1.0f;   // reject obstacles within 1 m of spawn
    mp.robot_x              = env->robot.x;
    mp.robot_y              = env->robot.y;

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
    // Slew-rate (acceleration) limiting: clamp the per-step change so the
    // policy can't ask for physically-impossible jumps in v / w.
    if (env->train_a_max > 0.0f) {
        float dv_max = env->train_a_max * env->dt;
        float dv = v_cmd - env->robot.v;
        if (dv >  dv_max) v_cmd = env->robot.v + dv_max;
        if (dv < -dv_max) v_cmd = env->robot.v - dv_max;
    }
    if (env->train_alpha_max > 0.0f) {
        float dw_max = env->train_alpha_max * env->dt;
        float dw = w_cmd - env->robot.w;
        if (dw >  dw_max) w_cmd = env->robot.w + dw_max;
        if (dw < -dw_max) w_cmd = env->robot.w - dw_max;
    }

    jackal_step(&env->robot, v_cmd, w_cmd, env->dt);

    // Clamp robot to the inner-arena box (footprint-aware).
    float half = 0.5f * env->arena_size - JACKAL_RADIUS;
    env->robot.x = clampf(env->robot.x, -half, half);
    env->robot.y = clampf(env->robot.y, -half, half);

    update_obstacles(env);

    float dist = robot_to_goal_dist(env);

    // ───── Reward (per step) ─────
    float r = 0.0f;

    // Δ-distance shaping (DEPRECATED, set gamma_d=0 to disable)
    if (env->gamma_d != 0.0f) {
        r += env->gamma_d * (env->prev_dist - dist);
    }
    // Obstacle Gaussian repulsion (DEPRECATED, set beta=0 to disable)
    if (env->beta > 0.0f) {
        float d_obs = closest_obstacle_dist(env);
        if (d_obs < 1e3f) {
            float sig2 = env->sigma_o * env->sigma_o;
            r -= env->beta * expf(-(d_obs * d_obs) / sig2);
        }
    }
    // Single-scale goal-attraction Gaussian (DEPRECATED, set alpha_g=0)
    if (env->alpha_g > 0.0f && env->sigma_g > 0.0f) {
        float sg2 = env->sigma_g * env->sigma_g;
        r += env->alpha_g * expf(-(dist * dist) / sg2);
    }
    // Per-step time penalty (DEPRECATED, set time_penalty=0)
    if (env->time_penalty > 0.0f) {
        r -= env->time_penalty;
    }
    // Multi-scale Gaussian goal attraction — current primary signal.
    // Each scale gives a soft "proximity" reward at its own range; sum
    // gives a smooth gradient at all distances.
    if (env->alpha_short > 0.0f && env->sigma_short > 0.0f) {
        float s2 = env->sigma_short * env->sigma_short;
        r += env->alpha_short * expf(-(dist * dist) / s2);
    }
    if (env->alpha_med > 0.0f && env->sigma_med > 0.0f) {
        float s2 = env->sigma_med * env->sigma_med;
        r += env->alpha_med * expf(-(dist * dist) / s2);
    }
    if (env->alpha_long > 0.0f && env->sigma_long > 0.0f) {
        float s2 = env->sigma_long * env->sigma_long;
        r += env->alpha_long * expf(-(dist * dist) / s2);
    }

    // ───── Goal & collision events ─────
    // Success criterion: L∞ box if goal_box_half > 0, else legacy Euclidean.
    // Default goal_box_half = 0.3 m matches the paper's eval pipeline
    // (check_goal_node.py:arrival_gaol).
    bool reached_now;
    if (env->goal_box_half > 0.0f) {
        reached_now = fabsf(env->robot.x - env->goal_x) < env->goal_box_half
                   && fabsf(env->robot.y - env->goal_y) < env->goal_box_half;
    } else {
        reached_now = (dist < env->goal_radius);
    }
    bool collided_now = obstacle_collision(env) || wall_collision(env);

    // First-time goal touch: bookkeeping only (success_bonus is applied
    // only if terminate_on_goal so it doesn't double-pay during dwell).
    if (reached_now && !env->reached_once) {
        env->reached_once = true;
        if (env->success_bonus > 0.0f) {
            // One-shot bonus on first contact even if not terminating.
            r += env->success_bonus;
        }
    }
    // Edge-detected collision: penalty applies only on the step the robot
    // transitions from non-collision to collision.
    if (collided_now && !env->was_in_collision) {
        env->collided_once = true;
        if (!env->reached_once) env->collided_before_reach = true;
        env->ep_n_collision_events++;
        // Record distance-at-first-collision (first collision only).
        if (env->ep_dist_at_first_coll < 0.0f) {
            env->ep_dist_at_first_coll = dist;
        }
        // Streak from last collision to this one (or from episode start if
        // this is the first collision). ep_last_coll_tick = -1 means
        // "no prior collision" — streak is tick - 0 = tick.
        int streak = (env->ep_last_coll_tick < 0)
                     ? env->tick
                     : env->tick - env->ep_last_coll_tick;
        if (streak > env->ep_max_streak) env->ep_max_streak = streak;
        env->ep_last_coll_tick = env->tick;
        if (env->collision_penalty > 0.0f) {
            r -= env->collision_penalty;
        }
    }
    env->was_in_collision = collided_now;

    // ───── Per-episode telemetry ─────
    if (dist < env->ep_min_dist) env->ep_min_dist = dist;
    {
        float d_obs_now = closest_obstacle_dist(env);
        if (d_obs_now < env->ep_min_obs_dist) env->ep_min_obs_dist = d_obs_now;
    }
    if (reached_now) env->ep_steps_at_goal++;

    // ───── Termination ─────
    bool ends_on_goal      = (reached_now  && env->terminate_on_goal);
    bool ends_on_collision = (collided_now && env->terminate_on_collision);
    bool truncated         = (env->tick >= env->max_steps);

    env->rewards[0] = r;
    env->episode_return += r;
    env->terminals[0] = (ends_on_goal || ends_on_collision) ? 1.0f : 0.0f;
    env->prev_dist = dist;

    if (ends_on_goal || ends_on_collision || truncated) {
        // Logging uses the latched per-episode flags so success/collision
        // counts are meaningful even when termination is off (every
        // episode then ends via truncation).
        bool succ_strict   = env->reached_once && !env->collided_once;
        bool succ_clean    = env->reached_once && !env->collided_before_reach;
        env->log.perf               += env->reached_once ? 1.0f : 0.0f;
        env->log.success            += env->reached_once ? 1.0f : 0.0f;
        env->log.success_strict     += succ_strict ? 1.0f : 0.0f;
        env->log.success_clean_reach+= succ_clean  ? 1.0f : 0.0f;
        env->log.collision          += env->collided_once ? 1.0f : 0.0f;
        env->log.timeout            += (truncated && !env->reached_once && !ends_on_collision) ? 1.0f : 0.0f;
        env->log.episode_length  += env->tick;
        env->log.episode_return  += env->episode_return;
        env->log.score           += env->episode_return;
        env->log.n               += 1.0f;
        // New telemetry channels (sums; dashboard divides by n)
        env->log.min_dist_to_goal   += env->ep_min_dist < 1e8f ? env->ep_min_dist : 0.0f;
        env->log.final_dist_to_goal += dist;
        env->log.n_collision_events += (float)env->ep_n_collision_events;
        env->log.closest_obstacle   += env->ep_min_obs_dist < 1e8f ? env->ep_min_obs_dist : 0.0f;
        env->log.steps_at_goal      += (float)env->ep_steps_at_goal;
        // dist-from-goal-at-first-collision: if no collision happened
        // this episode, fall back to the min-distance-seen.
        float dist_first = env->ep_dist_at_first_coll >= 0.0f
                           ? env->ep_dist_at_first_coll
                           : (env->ep_min_dist < 1e8f ? env->ep_min_dist : 0.0f);
        env->log.dist_from_goal_at_first_collision += dist_first;
        // max consecutive-non-collision streak. If episode had no collision,
        // the whole episode is one streak (= env->tick). Otherwise, finalize
        // the tail-streak (from last collision to episode end) and take the max.
        int final_streak;
        if (env->ep_last_coll_tick < 0) {
            final_streak = env->tick;
        } else {
            int tail = env->tick - env->ep_last_coll_tick;
            final_streak = env->ep_max_streak;
            if (tail > final_streak) final_streak = tail;
        }
        env->log.max_steps_between_collisions += (float)final_streak;
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
