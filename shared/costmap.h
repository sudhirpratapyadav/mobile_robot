// costmap.h — Quick-and-dirty egocentric 2D occupancy grid from a LiDAR scan.
// Header-only.
//
// Given a 720-beam scan over LIDAR_FOV centred on the robot heading, build
// a body-frame square grid of size GRID×GRID covering ±COSTMAP_HALF metres.
// Robot is at the centre cell looking +x (forward → up if you image-print
// row 0 = top).
//
// Cell convention:
//   1.0  = obstacle hit (a beam endpoint landed in this cell)
//   0.0  = unknown / not observed
//
// The "dirty" version only marks the cell containing the beam endpoint.
// Cells in between origin and endpoint are not marked free — saves a per-
// beam DDA loop. Add raytracing later if needed.
#pragma once

#include <math.h>
#include <string.h>
#include "lidar.h"

#define COSTMAP_GRID         64
#define COSTMAP_HALF         5.0f      // metres; grid covers [-HALF, +HALF] in body frame
#define COSTMAP_SIZE         (COSTMAP_GRID * COSTMAP_GRID)

// Convert a body-frame point (xb, yb) to a grid cell index, or -1 if outside.
// xb = forward (+x), yb = left (+y) — consistent with to_body_frame().
// Cell layout: row 0 = +x_b max (forward), col 0 = +y_b max (left).
// (i.e. printed grid would put the robot's forward direction at the top.)
static inline int costmap_idx(float xb, float yb) {
    if (xb < -COSTMAP_HALF || xb >= COSTMAP_HALF) return -1;
    if (yb < -COSTMAP_HALF || yb >= COSTMAP_HALF) return -1;
    float cell = 2.0f * COSTMAP_HALF / (float)COSTMAP_GRID;
    int row = (int)((COSTMAP_HALF - xb) / cell);
    int col = (int)((COSTMAP_HALF - yb) / cell);
    if (row < 0) row = 0;
    if (row >= COSTMAP_GRID) row = COSTMAP_GRID - 1;
    if (col < 0) col = 0;
    if (col >= COSTMAP_GRID) col = COSTMAP_GRID - 1;
    return row * COSTMAP_GRID + col;
}

// Rasterize a single LiDAR scan into a body-frame grid. The scan was
// computed by lidar_scan() in world frame, but each beam's *body-frame*
// direction is well-defined: beam b has angle (b - (N-1)/2) * angle_step
// relative to robot heading (since lidar_scan emits beams over [-FOV/2,
// +FOV/2] of the heading). So endpoint in body frame is
//   (d * cos(beam_angle_body), d * sin(beam_angle_body)).
// d == LIDAR_RANGE means "no hit" — we skip those cells.
//
// `ranges` is the array returned by lidar_scan(). Output `out` is
// COSTMAP_SIZE floats (cleared first).
static inline void costmap_rasterize(const float* ranges, float* out) {
    memset(out, 0, COSTMAP_SIZE * sizeof(float));
    float angle_step = LIDAR_FOV / (float)(LIDAR_BEAMS - 1);
    float angle0 = -0.5f * LIDAR_FOV;          // body-frame: beam 0 to the right
    for (int b = 0; b < LIDAR_BEAMS; b++) {
        float r = ranges[b];
        // Skip "max range" returns — they mean the beam hit nothing within
        // sensor range. Use a tiny margin so floating-point exact-equality
        // doesn't accidentally let them through.
        if (r >= LIDAR_RANGE - 1e-3f) continue;
        float ang = angle0 + b * angle_step;
        float xb = r * cosf(ang);
        float yb = r * sinf(ang);
        int idx = costmap_idx(xb, yb);
        if (idx >= 0) out[idx] = 1.0f;
    }
}
