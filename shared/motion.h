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

#define MOTION_T_END      60.0f
#define MOTION_DT_SAMPLE   1.0f
#define MOTION_ARENA_HALF 10.0f
#define MOTION_FAMILY_COUNT 6

typedef enum {
    MOTION_POLY        = 0,   // existing paper Alg. 1 — polynomial waypoints
    MOTION_LINEAR      = 1,   // p(t) = p0 + v·t, wall bounce
    MOTION_RECIPROCATING = 2, // moves back and forth between two points
    MOTION_SINUSOIDAL  = 3,   // p(t) = p0 + v·t + A·sin(ω·t)·n_perp
    MOTION_RANDOM_WALK = 4,   // velocity-noise process
    MOTION_STATIONARY  = 5    // fixed point
} MotionFamily;

// Hyper-knobs shared by all families (set from the env config).
typedef struct {
    float speed_min;      // m/s, lower bound on per-obstacle linear speed
    float speed_max;      // m/s, upper bound
    int   order_min;      // polynomial degree range (POLY only)
    int   order_max;
    float std_min;        // per-segment speed-noise std (POLY only)
    float std_max;
    // Family-specific:
    float amp_max;        // SINUSOIDAL: max swing amplitude
    float freq_min;       // SINUSOIDAL: ω/(2π) lower bound (Hz)
    float freq_max;
    float walk_step_std;  // RANDOM_WALK: velocity-perturb std per step
    float reciprocate_min_dist; // RECIPROCATING: end-to-end straight-line distance min
} MotionParams;

static inline MotionParams motion_default_params(void) {
    return (MotionParams){
        .speed_min = 0.3f,
        .speed_max = 2.5f,
        .order_min = 1,
        .order_max = 5,
        .std_min = 0.0f,
        .std_max = 0.3f,
        .amp_max = 3.0f,
        .freq_min = 0.05f,
        .freq_max = 0.5f,
        .walk_step_std = 0.4f,
        .reciprocate_min_dist = 4.0f,
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
    float speed = rand_uniformf(rng, mp->speed_min, mp->speed_max);
    float ang   = rand_uniformf(rng, 0.0f, 2.0f * (float)M_PI);
    float vx = speed * cosf(ang);
    float vy = speed * sinf(ang);
    float x0 = rand_uniformf(rng, -MOTION_ARENA_HALF, MOTION_ARENA_HALF);
    float y0 = rand_uniformf(rng, -MOTION_ARENA_HALF, MOTION_ARENA_HALF);
    int N = n_steps();
    out->num_waypoints = N;
    for (int i = 0; i < N; i++) {
        float t = i * MOTION_DT_SAMPLE;
        float x = x0 + vx * t;
        float y = y0 + vy * t;
        x = wrap_bounce(x, -MOTION_ARENA_HALF, MOTION_ARENA_HALF);
        y = wrap_bounce(y, -MOTION_ARENA_HALF, MOTION_ARENA_HALF);
        out->t[i] = t; out->x[i] = x; out->y[i] = y;
    }
    return true;
}

static inline bool fam_reciprocating(ObstacleTrajectory* out, unsigned int* rng,
                                     const MotionParams* mp) {
    float A_half = MOTION_ARENA_HALF - 0.5f;
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
    float speed = rand_uniformf(rng, mp->speed_min, mp->speed_max);
    float ang   = rand_uniformf(rng, 0.0f, 2.0f * (float)M_PI);
    float vx = speed * cosf(ang);
    float vy = speed * sinf(ang);
    float perp_x = -sinf(ang);
    float perp_y =  cosf(ang);
    float A = rand_uniformf(rng, 0.2f, mp->amp_max);
    float freq = rand_uniformf(rng, mp->freq_min, mp->freq_max);
    float omega = 2.0f * (float)M_PI * freq;
    float x0 = rand_uniformf(rng, -MOTION_ARENA_HALF, MOTION_ARENA_HALF);
    float y0 = rand_uniformf(rng, -MOTION_ARENA_HALF, MOTION_ARENA_HALF);
    int N = n_steps();
    out->num_waypoints = N;
    for (int i = 0; i < N; i++) {
        float t = i * MOTION_DT_SAMPLE;
        float x = x0 + vx * t + A * sinf(omega * t) * perp_x;
        float y = y0 + vy * t + A * sinf(omega * t) * perp_y;
        x = wrap_bounce(x, -MOTION_ARENA_HALF, MOTION_ARENA_HALF);
        y = wrap_bounce(y, -MOTION_ARENA_HALF, MOTION_ARENA_HALF);
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
    float x = rand_uniformf(rng, -MOTION_ARENA_HALF, MOTION_ARENA_HALF);
    float y = rand_uniformf(rng, -MOTION_ARENA_HALF, MOTION_ARENA_HALF);
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
        if (x >  MOTION_ARENA_HALF) { x =  MOTION_ARENA_HALF; vx = -vx; }
        if (x < -MOTION_ARENA_HALF) { x = -MOTION_ARENA_HALF; vx = -vx; }
        if (y >  MOTION_ARENA_HALF) { y =  MOTION_ARENA_HALF; vy = -vy; }
        if (y < -MOTION_ARENA_HALF) { y = -MOTION_ARENA_HALF; vy = -vy; }
        out->t[i] = t; out->x[i] = x; out->y[i] = y;
    }
    return true;
}

static inline bool fam_stationary(ObstacleTrajectory* out, unsigned int* rng,
                                  const MotionParams* mp) {
    (void)mp;
    float x0 = rand_uniformf(rng, -MOTION_ARENA_HALF, MOTION_ARENA_HALF);
    float y0 = rand_uniformf(rng, -MOTION_ARENA_HALF, MOTION_ARENA_HALF);
    out->num_waypoints = 1;
    out->t[0] = 0.0f;
    out->x[0] = x0;
    out->y[0] = y0;
    return true;
}

// Poly family is a thin wrapper around the existing paper-Alg-1 generator.
static inline bool fam_poly(ObstacleTrajectory* out, unsigned int* rng,
                            const MotionParams* mp) {
    return traj_generate(out, rng,
                         mp->order_min, mp->order_max,
                         mp->speed_min, mp->speed_max,
                         mp->std_min,   mp->std_max,
                         0.05f);
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
