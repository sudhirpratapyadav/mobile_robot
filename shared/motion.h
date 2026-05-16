// motion.h — Mixture of obstacle motion families.
// Header-only. Each family fills an ObstacleTrajectory with waypoints
// sampled at a fixed cadence (default DT_SAMPLE = 1.0 s, total 60 s).
// The downstream env (obstacle.h:obstacle_position) handles linear
// interpolation between waypoints.
//
// Why pre-sampled waypoints rather than closed-form motion: lets us reuse
// the existing collision + lidar + viz code unchanged. The trajectory
// generator just becomes per-family "fill in (t, x, y) triples".
#pragma once

#include <math.h>
#include <stdbool.h>
#include "jackal.h"          // rand_uniformf, rand_normalf
#include "obstacle.h"        // ObstacleTrajectory, MAX_WAYPOINTS_PER_OBS
#include "traj_gen.h"        // existing polynomial-fit family

#define MOTION_T_END      80.0f   // matches dyna_train max_steps * dt = 800 * 0.1
#define MOTION_DT_SAMPLE   1.0f
#define MOTION_FAMILY_COUNT 6     // kept at 6 for back-compat (some weights = 0)

typedef enum {
    MOTION_POLY        = 0,   // low-order polynomial trajectory through random control points
    MOTION_LINEAR      = 1,   // p(t) = p0 + v·t, wall bounce
    MOTION_RECIPROCATING = 2, // DEPRECATED — set weight to 0
    MOTION_SINUSOIDAL  = 3,   // p(t) = p0 + v·t + A·sin(ω·t)·n_perp
    MOTION_RANDOM_WALK = 4,   // DEPRECATED — set weight to 0
    MOTION_STATIONARY  = 5    // fixed point
} MotionFamily;

// Hyper-knobs shared by all families (set from the env config).
typedef struct {
    float arena_half;     // m, half-extent of the arena ([−A, +A] in both axes)
    float speed_min;      // m/s, per-obstacle constant speed lower bound
    float speed_max;      // m/s, upper bound
    int   order_min;      // polynomial degree range (POLY only)
    int   order_max;
    float std_min;        // DEPRECATED (was per-segment speed noise)
    float std_max;        // DEPRECATED
    // Sinusoidal-family:
    float amp_max;        // max swing amplitude (m)
    float freq_min;       // ω/(2π) lower bound (Hz)
    float freq_max;
    float walk_step_std;  // DEPRECATED
    float reciprocate_min_dist; // DEPRECATED
    // Rejection-sampling: keep obstacles' t=0 positions at least
    // `init_min_dist_from_robot` away from (robot_x, robot_y). Caller is
    // expected to set these per reset. 0 = no rejection.
    float init_min_dist_from_robot;
    float robot_x;
    float robot_y;
} MotionParams;

static inline MotionParams motion_default_params(void) {
    return (MotionParams){
        .arena_half = 12.0f,         // larger default — env sets per-reset
        .speed_min = 0.3f,
        .speed_max = 2.0f,
        .order_min = 1,
        .order_max = 2,
        .std_min = 0.0f,
        .std_max = 0.0f,
        .amp_max = 2.0f,
        .freq_min = 0.10f,
        .freq_max = 0.50f,
        .walk_step_std = 0.0f,
        .reciprocate_min_dist = 0.0f,
        .init_min_dist_from_robot = 1.0f,
        .robot_x = 0.0f,
        .robot_y = 0.0f,
    };
}

// ---------------------------------------------------------------------------
// Family-specific generators. Return true on success.
// Each writes ≤ MAX_WAYPOINTS_PER_OBS triples (t, x, y) with t starting at 0.
// ---------------------------------------------------------------------------

static inline int n_steps(void) {
    int n = (int)(MOTION_T_END / MOTION_DT_SAMPLE) + 1;
    if (n > MAX_WAYPOINTS_PER_OBS) n = MAX_WAYPOINTS_PER_OBS;
    return n;
}

// Sample a uniform random (x0, y0) inside [-A, +A]² and reject if it's too
// close to the robot at spawn. Up to `tries` attempts; falls back to the
// last sample if all fail.
static inline void sample_init_pos(unsigned int* rng, const MotionParams* mp,
                                   float* x0_out, float* y0_out) {
    float A = mp->arena_half;
    float min_d2 = mp->init_min_dist_from_robot * mp->init_min_dist_from_robot;
    float x = 0.0f, y = 0.0f;
    for (int t = 0; t < 20; t++) {
        x = rand_uniformf(rng, -A, A);
        y = rand_uniformf(rng, -A, A);
        if (min_d2 <= 0.0f) break;
        float dx = x - mp->robot_x;
        float dy = y - mp->robot_y;
        if (dx*dx + dy*dy >= min_d2) break;
    }
    *x0_out = x;
    *y0_out = y;
}

static inline float wrap_bounce(float x, float lo, float hi) {
    // Bounce x into [lo, hi] like a billiard.
    float span = hi - lo;
    if (span <= 0.0f) return lo;
    float u = (x - lo) / span;
    u = u - floorf(u * 0.5f) * 2.0f;
    if (u > 1.0f) u = 2.0f - u;
    if (u < 0.0f) u = -u;
    return lo + u * span;
}

static inline bool fam_linear(ObstacleTrajectory* out, unsigned int* rng,
                              const MotionParams* mp) {
    float A = mp->arena_half;
    float speed = rand_uniformf(rng, mp->speed_min, mp->speed_max);
    float ang   = rand_uniformf(rng, 0.0f, 2.0f * (float)M_PI);
    float vx = speed * cosf(ang);
    float vy = speed * sinf(ang);
    float x0, y0;
    sample_init_pos(rng, mp, &x0, &y0);
    int N = n_steps();
    out->num_waypoints = N;
    for (int i = 0; i < N; i++) {
        float t = i * MOTION_DT_SAMPLE;
        float x = x0 + vx * t;
        float y = y0 + vy * t;
        x = wrap_bounce(x, -A, A);
        y = wrap_bounce(y, -A, A);
        out->t[i] = t; out->x[i] = x; out->y[i] = y;
    }
    return true;
}

static inline bool fam_reciprocating(ObstacleTrajectory* out, unsigned int* rng,
                                     const MotionParams* mp) {
    float A_half = mp->arena_half - 0.5f;
    float speed = rand_uniformf(rng, mp->speed_min, mp->speed_max);
    float xA, yA, xB, yB;
    for (int tries = 0; tries < 20; tries++) {
        xA = rand_uniformf(rng, -A_half, A_half);
        yA = rand_uniformf(rng, -A_half, A_half);
        xB = rand_uniformf(rng, -A_half, A_half);
        yB = rand_uniformf(rng, -A_half, A_half);
        float dx = xB - xA, dy = yB - yA;
        if (sqrtf(dx*dx + dy*dy) >= mp->reciprocate_min_dist) break;
    }
    float dx = xB - xA, dy = yB - yA;
    float L = sqrtf(dx*dx + dy*dy);
    if (L < 1e-3f) return false;
    float period = 2.0f * L / speed;            // one back-and-forth
    int N = n_steps();
    out->num_waypoints = N;
    for (int i = 0; i < N; i++) {
        float t = i * MOTION_DT_SAMPLE;
        float phase = fmodf(t, period) / period;     // [0, 1)
        float u = (phase < 0.5f) ? (phase * 2.0f) : (2.0f - phase * 2.0f);
        out->t[i] = t;
        out->x[i] = xA + u * dx;
        out->y[i] = yA + u * dy;
    }
    return true;
}

static inline bool fam_sinusoidal(ObstacleTrajectory* out, unsigned int* rng,
                                  const MotionParams* mp) {
    float A_arena = mp->arena_half;
    float speed = rand_uniformf(rng, mp->speed_min, mp->speed_max);
    float ang   = rand_uniformf(rng, 0.0f, 2.0f * (float)M_PI);
    float vx = speed * cosf(ang);
    float vy = speed * sinf(ang);
    float perp_x = -sinf(ang);
    float perp_y =  cosf(ang);
    float A_amp = rand_uniformf(rng, 0.2f, mp->amp_max);
    float freq = rand_uniformf(rng, mp->freq_min, mp->freq_max);
    float omega = 2.0f * (float)M_PI * freq;
    float x0, y0;
    sample_init_pos(rng, mp, &x0, &y0);
    int N = n_steps();
    out->num_waypoints = N;
    for (int i = 0; i < N; i++) {
        float t = i * MOTION_DT_SAMPLE;
        float x = x0 + vx * t + A_amp * sinf(omega * t) * perp_x;
        float y = y0 + vy * t + A_amp * sinf(omega * t) * perp_y;
        x = wrap_bounce(x, -A_arena, A_arena);
        y = wrap_bounce(y, -A_arena, A_arena);
        out->t[i] = t; out->x[i] = x; out->y[i] = y;
    }
    return true;
}

static inline bool fam_random_walk(ObstacleTrajectory* out, unsigned int* rng,
                                   const MotionParams* mp) {
    float speed0 = rand_uniformf(rng, mp->speed_min, mp->speed_max);
    float ang   = rand_uniformf(rng, 0.0f, 2.0f * (float)M_PI);
    float vx = speed0 * cosf(ang);
    float vy = speed0 * sinf(ang);
    float A = mp->arena_half;
    float x = rand_uniformf(rng, -A, A);
    float y = rand_uniformf(rng, -A, A);
    int N = n_steps();
    out->num_waypoints = N;
    out->t[0] = 0.0f; out->x[0] = x; out->y[0] = y;
    for (int i = 1; i < N; i++) {
        // Perturb velocity, clamp speed to [speed_min, speed_max].
        vx += mp->walk_step_std * rand_normalf(rng);
        vy += mp->walk_step_std * rand_normalf(rng);
        float spd = sqrtf(vx*vx + vy*vy);
        float lo = mp->speed_min, hi = mp->speed_max;
        if (spd < lo && spd > 1e-3f) { vx *= lo/spd; vy *= lo/spd; }
        if (spd > hi)                 { vx *= hi/spd; vy *= hi/spd; }
        float t = i * MOTION_DT_SAMPLE;
        x += vx * MOTION_DT_SAMPLE;
        y += vy * MOTION_DT_SAMPLE;
        if (x >  A) { x =  A; vx = -vx; }
        if (x < -A) { x = -A; vx = -vx; }
        if (y >  A) { y =  A; vy = -vy; }
        if (y < -A) { y = -A; vy = -vy; }
        out->t[i] = t; out->x[i] = x; out->y[i] = y;
    }
    return true;
}

static inline bool fam_stationary(ObstacleTrajectory* out, unsigned int* rng,
                                  const MotionParams* mp) {
    float x0, y0;
    sample_init_pos(rng, mp, &x0, &y0);
    out->num_waypoints = 1;
    out->t[0] = 0.0f;
    out->x[0] = x0;
    out->y[0] = y0;
    return true;
}

// Poly family — start at a uniform random (x0, y0), pick a random heading
// and a small lateral acceleration, integrate forward at constant tangent
// speed. Produces smooth curved trajectories (order 1 = linear-ish, order
// 2 = parabolic). Bounces off arena walls.
//
// Note: this is a much simpler "natural" generator than the paper's
// Algorithm 1 (which fits an arbitrary-degree polynomial through random
// control points and ends up with high-curvature wiggles). We keep the
// `order_min/max` knob: order 1 → no curvature, order 2 → constant
// curvature, order ≥ 3 → adds a small slow swing.
static inline bool fam_poly(ObstacleTrajectory* out, unsigned int* rng,
                            const MotionParams* mp) {
    float A = mp->arena_half;
    float speed = rand_uniformf(rng, mp->speed_min, mp->speed_max);
    float ang   = rand_uniformf(rng, 0.0f, 2.0f * (float)M_PI);
    float vx = speed * cosf(ang);
    float vy = speed * sinf(ang);
    float perp_x = -sinf(ang);
    float perp_y =  cosf(ang);
    // Curvature: 0 for order=1, in [-0.5, 0.5] for order=2, larger for order ≥ 3.
    int order = (mp->order_min <= mp->order_max)
                ? mp->order_min + (rand_r(rng) % (mp->order_max - mp->order_min + 1))
                : mp->order_min;
    if (order < 1) order = 1;
    float curvature_scale = 0.0f;
    if (order >= 2) curvature_scale = 0.5f * (order - 1);
    float lat_acc = rand_uniformf(rng, -curvature_scale, curvature_scale);
    float x0, y0;
    sample_init_pos(rng, mp, &x0, &y0);
    int N = n_steps();
    out->num_waypoints = N;
    for (int i = 0; i < N; i++) {
        float t = i * MOTION_DT_SAMPLE;
        // Tangential position + quadratic lateral drift along perp.
        float x = x0 + vx * t + 0.5f * lat_acc * t * t * perp_x;
        float y = y0 + vy * t + 0.5f * lat_acc * t * t * perp_y;
        x = wrap_bounce(x, -A, A);
        y = wrap_bounce(y, -A, A);
        out->t[i] = t; out->x[i] = x; out->y[i] = y;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Mixture sampler. Picks one family by integer weight vector then calls it.
// `weights` is length MOTION_FAMILY_COUNT in MotionFamily enum order.
// ---------------------------------------------------------------------------
static inline MotionFamily motion_pick_family(unsigned int* rng,
                                              const int weights[MOTION_FAMILY_COUNT]) {
    int total = 0;
    for (int i = 0; i < MOTION_FAMILY_COUNT; i++) total += weights[i];
    if (total <= 0) return MOTION_POLY;
    extern int rand_r(unsigned int*);
    int r = rand_r(rng) % total;
    int acc = 0;
    for (int i = 0; i < MOTION_FAMILY_COUNT; i++) {
        acc += weights[i];
        if (r < acc) return (MotionFamily)i;
    }
    return MOTION_POLY;
}

static inline bool motion_generate(ObstacleTrajectory* out,
                                   MotionFamily family,
                                   unsigned int* rng,
                                   const MotionParams* mp) {
    switch (family) {
    case MOTION_POLY:          return fam_poly(out, rng, mp);
    case MOTION_LINEAR:        return fam_linear(out, rng, mp);
    case MOTION_RECIPROCATING: return fam_reciprocating(out, rng, mp);
    case MOTION_SINUSOIDAL:    return fam_sinusoidal(out, rng, mp);
    case MOTION_RANDOM_WALK:   return fam_random_walk(out, rng, mp);
    case MOTION_STATIONARY:    return fam_stationary(out, rng, mp);
    default:                   return fam_poly(out, rng, mp);
    }
}
