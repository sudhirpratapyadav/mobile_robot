// lidar.h — 2D LiDAR raycast against an arena (4 walls) + cylinder obstacles.
// Header-only.
//
// 720 beams over a 270° FOV (matches Clearpath Jackal w/ Hokuyo UST-10LX per
// BARN Challenge 2024 paper §I). Beams indexed 0..LIDAR_BEAMS-1 span
// [-LIDAR_FOV/2, +LIDAR_FOV/2] in robot body frame; beam 0 is to the right,
// beam (N-1) is to the left, with the center beam pointing forward (+x).
//
// Distances are clamped to LIDAR_RANGE if no hit found (= "max range" return).
#pragma once

#include <math.h>
#include "jackal.h"
#include "obstacle.h"

#define LIDAR_BEAMS  720
#define LIDAR_FOV    ((float)(3.0 * M_PI / 2.0))   // 270°
#define LIDAR_RANGE  30.0f                          // m

// Ray–cylinder intersection. Ray origin (rx, ry), direction (cx_dir, sx_dir).
// Cylinder center (ox, oy), radius r. Returns smallest non-negative root, or
// `max` if no real / non-negative root.
static inline float ray_vs_cylinder(float rx, float ry,
                                    float cx_dir, float sx_dir,
                                    float ox, float oy, float r,
                                    float max) {
    float dx = rx - ox;
    float dy = ry - oy;
    float b = dx * cx_dir + dy * sx_dir;        // half of the linear coef
    float c = dx * dx + dy * dy - r * r;
    float disc = b * b - c;
    if (disc < 0.0f) return max;
    float sd = sqrtf(disc);
    float t1 = -b - sd;
    if (t1 >= 0.0f && t1 < max) return t1;
    float t2 = -b + sd;
    if (t2 >= 0.0f && t2 < max) return t2;
    return max;
}

// Ray vs. axis-aligned line segment. Returns distance to hit or `max`.
// Segments: vertical (x = vx, y ∈ [y0, y1]) or horizontal (y = hy, x ∈ [x0, x1]).
static inline float ray_vs_vline(float rx, float ry,
                                 float cx_dir, float sx_dir,
                                 float vx, float y0, float y1, float max) {
    if (fabsf(cx_dir) < 1e-7f) return max;
    float t = (vx - rx) / cx_dir;
    if (t < 0.0f || t >= max) return max;
    float yhit = ry + t * sx_dir;
    if (yhit < y0 || yhit > y1) return max;
    return t;
}
static inline float ray_vs_hline(float rx, float ry,
                                 float cx_dir, float sx_dir,
                                 float hy, float x0, float x1, float max) {
    if (fabsf(sx_dir) < 1e-7f) return max;
    float t = (hy - ry) / sx_dir;
    if (t < 0.0f || t >= max) return max;
    float xhit = rx + t * cx_dir;
    if (xhit < x0 || xhit > x1) return max;
    return t;
}

// Compute all 720 beam ranges. Writes to `out_ranges[LIDAR_BEAMS]`.
//
//   robot  — robot pose
//   arena_half — half-extent of the (square) arena, so walls are at ±arena_half
//   obs_x, obs_y — current obstacle (x, y) per obstacle, length num_obstacles
//   num_obstacles — runtime count (≤ static caller-provided cap)
//
// O(beams × obstacles). For 720 × 20 ≈ 14k ops/step, trivially cheap.
static inline void lidar_scan(const JackalState* robot,
                              float arena_half,
                              const float* obs_x, const float* obs_y,
                              int num_obstacles,
                              float* out_ranges) {
    float angle_step = LIDAR_FOV / (float)(LIDAR_BEAMS - 1);
    float angle0 = robot->theta - 0.5f * LIDAR_FOV;
    float xmin = -arena_half, xmax = arena_half;
    float ymin = -arena_half, ymax = arena_half;
    float rx = robot->x, ry = robot->y;

    for (int b = 0; b < LIDAR_BEAMS; b++) {
        float ang = angle0 + b * angle_step;
        float cdir = cosf(ang);
        float sdir = sinf(ang);
        float best = LIDAR_RANGE;

        // Walls (4 segments). Cheap; do unconditionally.
        best = ray_vs_vline(rx, ry, cdir, sdir, xmin, ymin, ymax, best);
        best = ray_vs_vline(rx, ry, cdir, sdir, xmax, ymin, ymax, best);
        best = ray_vs_hline(rx, ry, cdir, sdir, ymin, xmin, xmax, best);
        best = ray_vs_hline(rx, ry, cdir, sdir, ymax, xmin, xmax, best);

        // Obstacles
        for (int i = 0; i < num_obstacles; i++) {
            best = ray_vs_cylinder(rx, ry, cdir, sdir,
                                   obs_x[i], obs_y[i], OBSTACLE_RADIUS, best);
        }
        out_ranges[b] = best;
    }
}
