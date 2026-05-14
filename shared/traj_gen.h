// traj_gen.h — Generate obstacle waypoint trajectories per DynaBARN Algorithm 1.
// Header-only.
//
// Port of github.com/aninair1905/DynaBARN/polynomial_fit.py.
// Differences from the Python:
//   * arena half-extent is parameterised (Python hard-codes 10);
//   * the Python's "every integer x" sampling is preserved verbatim;
//   * polynomial fit is exact interpolation through (order+1) points via
//     Vandermonde + Gaussian elimination, not least-squares (matches the
//     Python since they pick exactly degree+1 points);
//   * edge-intersection logic uses bisection on the polynomial since solving
//     general polynomial roots is overkill for order ≤ 5;
//   * "sort by x descending or ascending with prob 0.5" preserved.
#pragma once

#include <math.h>
#include <stdbool.h>
#include "jackal.h"        // rand_uniformf, rand_normalf, clampf
#include "obstacle.h"      // ObstacleTrajectory, MAX_WAYPOINTS_PER_OBS

#define TRAJ_MAX_ORDER 5      // generator caps at degree 5
#define TRAJ_ARENA_HALF 10.0f // arena half-extent for trajectory sampling

// In-place Gaussian elimination to solve A · coeffs = y for an (n × n) system.
// A is row-major, length n*n. Coeffs are placed in y.
static inline bool gauss_solve(int n, double* A, double* y) {
    for (int k = 0; k < n; k++) {
        // Partial pivot
        int piv = k;
        double max_abs = fabs(A[k*n + k]);
        for (int r = k + 1; r < n; r++) {
            double v = fabs(A[r*n + k]);
            if (v > max_abs) { max_abs = v; piv = r; }
        }
        if (max_abs < 1e-12) return false;
        if (piv != k) {
            for (int c = 0; c < n; c++) {
                double t = A[k*n + c]; A[k*n + c] = A[piv*n + c]; A[piv*n + c] = t;
            }
            double t = y[k]; y[k] = y[piv]; y[piv] = t;
        }
        double inv = 1.0 / A[k*n + k];
        for (int r = k + 1; r < n; r++) {
            double f = A[r*n + k] * inv;
            for (int c = k; c < n; c++) A[r*n + c] -= f * A[k*n + c];
            y[r] -= f * y[k];
        }
    }
    // Back substitute
    for (int k = n - 1; k >= 0; k--) {
        double s = y[k];
        for (int c = k + 1; c < n; c++) s -= A[k*n + c] * y[c];
        y[k] = s / A[k*n + k];
    }
    return true;
}

// Evaluate polynomial p(x) given coefficients in ascending order (c0 + c1·x + c2·x² + …).
static inline double poly_eval(const double* coefs, int n, double x) {
    double s = 0.0, xp = 1.0;
    for (int i = 0; i < n; i++) { s += coefs[i] * xp; xp *= x; }
    return s;
}

// Generate a trajectory for one obstacle. Returns true on success.
//
// Parameters per the paper:
//   order_min, order_max : polynomial degree bin
//   speed_min, speed_max : per-segment mean-speed bin (m/s)
//   std_min,   std_max   : per-segment speed-stddev bin
//   v_floor              : absolute min speed after clipping (avoid stalls)
static inline bool traj_generate(ObstacleTrajectory* out,
                                 unsigned int* rng,
                                 int order_min, int order_max,
                                 float speed_min, float speed_max,
                                 float std_min,  float std_max,
                                 float v_floor) {
    // 1. Sample order ∈ {order_min, …, order_max}
    int order_span = order_max - order_min;
    int n_order;
    if (order_span <= 0) {
        n_order = order_min;
    } else {
        extern int rand_r(unsigned int*);
        n_order = order_min + (rand_r(rng) % (order_span + 1));
    }
    if (n_order < 1) n_order = 1;
    if (n_order > TRAJ_MAX_ORDER) n_order = TRAJ_MAX_ORDER;

    // 2. Sample n_order + 1 random control points in [-A, A]². Python uses
    //    integer values via random.randint; we use continuous since the
    //    polynomial fit cares about position not integerness.
    int npts = n_order + 1;
    double xs[TRAJ_MAX_ORDER + 1], ys[TRAJ_MAX_ORDER + 1];
    for (int i = 0; i < npts; i++) {
        xs[i] = rand_uniformf(rng, -TRAJ_ARENA_HALF, TRAJ_ARENA_HALF);
        ys[i] = rand_uniformf(rng, -TRAJ_ARENA_HALF, TRAJ_ARENA_HALF);
    }

    // 3. Fit polynomial: Vandermonde system [x_i^j] · c = y_i
    double A[(TRAJ_MAX_ORDER + 1) * (TRAJ_MAX_ORDER + 1)];
    double rhs[TRAJ_MAX_ORDER + 1];
    for (int r = 0; r < npts; r++) {
        double xp = 1.0;
        for (int c = 0; c < npts; c++) {
            A[r*npts + c] = xp;
            xp *= xs[r];
        }
        rhs[r] = ys[r];
    }
    if (!gauss_solve(npts, A, rhs)) return false;
    double coefs[TRAJ_MAX_ORDER + 1];
    for (int i = 0; i < npts; i++) coefs[i] = rhs[i];

    // 4. Evaluate the polynomial at every integer x in [-A, A] AND include the
    //    original control points; collect in-bounds samples; deduplicate.
    int wp_count = 0;
    float wp_x[MAX_WAYPOINTS_PER_OBS];
    float wp_y[MAX_WAYPOINTS_PER_OBS];
    int A_int = (int)TRAJ_ARENA_HALF;
    for (int ix = -A_int; ix <= A_int; ix++) {
        double yv = poly_eval(coefs, npts, (double)ix);
        if (yv < -TRAJ_ARENA_HALF || yv > TRAJ_ARENA_HALF) continue;
        if (wp_count >= MAX_WAYPOINTS_PER_OBS) break;
        wp_x[wp_count] = (float)ix;
        wp_y[wp_count] = (float)yv;
        wp_count++;
    }
    // Also include the original control points (in-bounds by construction).
    for (int i = 0; i < npts; i++) {
        if (wp_count >= MAX_WAYPOINTS_PER_OBS) break;
        wp_x[wp_count] = (float)xs[i];
        wp_y[wp_count] = (float)ys[i];
        wp_count++;
    }
    if (wp_count < 2) return false;

    // Sort by x ascending, then deduplicate near-equal x (within 1e-3).
    // Insertion sort — tiny array.
    for (int i = 1; i < wp_count; i++) {
        float kx = wp_x[i], ky = wp_y[i];
        int j = i - 1;
        while (j >= 0 && wp_x[j] > kx) {
            wp_x[j+1] = wp_x[j]; wp_y[j+1] = wp_y[j];
            j--;
        }
        wp_x[j+1] = kx; wp_y[j+1] = ky;
    }
    int dedup = 1;
    for (int i = 1; i < wp_count; i++) {
        if (fabsf(wp_x[i] - wp_x[dedup-1]) > 1e-3f) {
            wp_x[dedup] = wp_x[i];
            wp_y[dedup] = wp_y[i];
            dedup++;
        }
    }
    wp_count = dedup;
    if (wp_count < 2) return false;

    // 5. Random sort direction (ascending/descending by x).
    bool descending = (rand_uniformf(rng, 0.0f, 1.0f) < 0.5f);
    // Currently waypoints are already in ascending x order (we iterated -A..+A).
    if (descending) {
        for (int i = 0, j = wp_count - 1; i < j; i++, j--) {
            float tx = wp_x[i]; wp_x[i] = wp_x[j]; wp_x[j] = tx;
            float ty = wp_y[i]; wp_y[i] = wp_y[j]; wp_y[j] = ty;
        }
    }

    // 6. Compute timestamps using per-segment sampled speeds.
    out->num_waypoints = wp_count;
    out->t[0] = 0.0f;
    out->x[0] = wp_x[0];
    out->y[0] = wp_y[0];
    for (int i = 1; i < wp_count; i++) {
        float avg_speed = rand_uniformf(rng, speed_min, speed_max);
        float avg_std   = rand_uniformf(rng, std_min,   std_max);
        float s = avg_speed + avg_std * rand_normalf(rng);
        s = clampf(s, speed_min, speed_max);
        if (s < v_floor) s = v_floor;
        float dx = wp_x[i] - wp_x[i - 1];
        float dy = wp_y[i] - wp_y[i - 1];
        float dist = sqrtf(dx * dx + dy * dy);
        float dt = dist / s;
        out->t[i] = out->t[i - 1] + dt;
        out->x[i] = wp_x[i];
        out->y[i] = wp_y[i];
    }
    return true;
}
