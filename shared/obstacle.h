// obstacle.h — Dynamic cylinder obstacle with a waypoint trajectory.
// Header-only.
//
// Trajectory representation per the DynaBARN paper §III-A:
//   ⟨c_i⟩ for i = 0..N, c_i = ((x_i, y_i), t_i)
// Segment i is the line from waypoint i-1 to waypoint i, traversed at constant
// speed (paper: "we use a straight line to approximate the movement between
// two points"). Position at time t is linear interpolation along the active
// segment. After the last waypoint, the obstacle holds position at the last
// waypoint.
//
// All cylinders have a fixed radius (DynaBARN: 0.5 m).
#pragma once

#include <math.h>
#include <stdbool.h>

#define OBSTACLE_RADIUS    0.5f       // per DynaBARN paper §III-A
#define MAX_WAYPOINTS_PER_OBS  64     // compile-time cap

typedef struct {
    int num_waypoints;                // ≥ 1
    float t[MAX_WAYPOINTS_PER_OBS];   // monotonically increasing, t[0] = 0
    float x[MAX_WAYPOINTS_PER_OBS];
    float y[MAX_WAYPOINTS_PER_OBS];
} ObstacleTrajectory;

// Evaluate position at world time `tau` (seconds since episode start).
// Returns the current (x, y). If tau is past the last waypoint, returns the
// last waypoint. If num_waypoints == 1 or tau ≤ 0, returns the first waypoint.
static inline void obstacle_position(const ObstacleTrajectory* traj,
                                     float tau,
                                     float* out_x, float* out_y) {
    int n = traj->num_waypoints;
    if (n <= 1 || tau <= traj->t[0]) {
        *out_x = traj->x[0];
        *out_y = traj->y[0];
        return;
    }
    if (tau >= traj->t[n - 1]) {
        *out_x = traj->x[n - 1];
        *out_y = traj->y[n - 1];
        return;
    }
    // Linear scan; trajectories are short (≤ 64). Binary search isn't worth it.
    int i = 1;
    while (i < n && traj->t[i] < tau) i++;
    float t0 = traj->t[i - 1], t1 = traj->t[i];
    float alpha = (tau - t0) / (t1 - t0);
    *out_x = traj->x[i - 1] + alpha * (traj->x[i] - traj->x[i - 1]);
    *out_y = traj->y[i - 1] + alpha * (traj->y[i] - traj->y[i - 1]);
}
