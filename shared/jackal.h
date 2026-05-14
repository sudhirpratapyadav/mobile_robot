// jackal.h — Clearpath Jackal differential-drive model + state.
// Header-only. No PufferLib dependencies — env files use this and add their own
// observation/action/reward layer on top.
#pragma once

#include <math.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Physical constants from the DynaBARN paper (§IV-b, Fig. 3 caption).
// v_max in m/s; v_min < 0 allows reverse.
#define JACKAL_V_MAX  2.0f
#define JACKAL_V_MIN  (-0.5f)
#define JACKAL_W_MAX  ((float)M_PI)        // ±π rad/s
#define JACKAL_FOOTPRINT_X 0.50f           // length, m
#define JACKAL_FOOTPRINT_Y 0.43f           // width,  m
#define JACKAL_RADIUS      0.30f           // circumscribed radius for collision checks (≈ √(0.25²+0.215²))

typedef struct {
    float x, y;                  // world position (m)
    float theta;                 // heading (rad), 0 = +x
    float v;                     // current linear velocity (m/s)
    float w;                     // current angular velocity (rad/s)
} JackalState;

// Apply a clipped (v_cmd, w_cmd) for one tick of length dt. World-position
// clamp to the arena is handled by the caller.
static inline void jackal_step(JackalState* s,
                               float v_cmd, float w_cmd, float dt) {
    if (!isfinite(v_cmd)) v_cmd = 0.0f;
    if (!isfinite(w_cmd)) w_cmd = 0.0f;
    if (v_cmd < JACKAL_V_MIN) v_cmd = JACKAL_V_MIN;
    if (v_cmd > JACKAL_V_MAX) v_cmd = JACKAL_V_MAX;
    if (w_cmd < -JACKAL_W_MAX) w_cmd = -JACKAL_W_MAX;
    if (w_cmd >  JACKAL_W_MAX) w_cmd =  JACKAL_W_MAX;

    s->v = v_cmd;
    s->w = w_cmd;

    s->theta += dt * s->w;
    if (s->theta >  (float)M_PI) s->theta -= 2.0f * (float)M_PI;
    if (s->theta < -(float)M_PI) s->theta += 2.0f * (float)M_PI;

    s->x += dt * s->v * cosf(s->theta);
    s->y += dt * s->v * sinf(s->theta);
}

// Body-frame rotation: world delta (dx, dy) → body delta (forward, left).
static inline void to_body_frame(float dx_world, float dy_world,
                                 float theta,
                                 float* out_forward, float* out_left) {
    float c = cosf(theta);
    float s = sinf(theta);
    *out_forward =  dx_world * c + dy_world * s;
    *out_left   = -dx_world * s + dy_world * c;
}

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

// Uniform [lo, hi] using rand_r-style PRNG state.
static inline float rand_uniformf(unsigned int* rng, float lo, float hi) {
    extern int rand_r(unsigned int*);
    float u = (float)rand_r(rng) / 2147483647.0f;
    return lo + u * (hi - lo);
}

// Standard-normal via Box-Muller (one of the pair; the other is discarded).
static inline float rand_normalf(unsigned int* rng) {
    float u1 = rand_uniformf(rng, 1e-7f, 1.0f);
    float u2 = rand_uniformf(rng, 0.0f, 1.0f);
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
}
