// world_loader.h — Load a baked DynaBARN world (output of bake_worlds.py)
// into an array of ObstacleTrajectory structs.
//
// Binary layout (per-world file, little-endian):
//   uint32 magic   = 0x44425257 ('DBRW')
//   uint32 version = 1
//   uint32 n_obstacles
//   for each obstacle:
//       uint32 n_waypoints
//       for each waypoint:
//           float32 t
//           float32 x
//           float32 y
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "obstacle.h"

#define BAKED_WORLD_MAGIC   0x44425257u
#define BAKED_WORLD_VERSION 1u

// Load a baked world into the caller-provided arrays. Returns the obstacle
// count on success, or -1 on error. Caller must ensure `traj_out` has space
// for at least `max_obs` entries; extra obstacles in the file are dropped
// with a warning to stderr.
static inline int world_loader_load(const char* path,
                                    ObstacleTrajectory* traj_out,
                                    int max_obs) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "world_loader: fopen %s failed\n", path);
        return -1;
    }
    uint32_t header[3];
    if (fread(header, sizeof(uint32_t), 3, f) != 3) {
        fprintf(stderr, "world_loader: short header in %s\n", path);
        fclose(f);
        return -1;
    }
    if (header[0] != BAKED_WORLD_MAGIC) {
        fprintf(stderr, "world_loader: bad magic 0x%x in %s\n", header[0], path);
        fclose(f);
        return -1;
    }
    if (header[1] != BAKED_WORLD_VERSION) {
        fprintf(stderr, "world_loader: unsupported version %u in %s\n",
                header[1], path);
        fclose(f);
        return -1;
    }
    int n_obs = (int)header[2];
    int n_keep = n_obs < max_obs ? n_obs : max_obs;
    if (n_obs > max_obs) {
        fprintf(stderr, "world_loader: %s has %d obstacles; capping to %d\n",
                path, n_obs, max_obs);
    }

    for (int i = 0; i < n_obs; i++) {
        uint32_t n_wp;
        if (fread(&n_wp, sizeof(uint32_t), 1, f) != 1) {
            fprintf(stderr, "world_loader: short obstacle %d in %s\n", i, path);
            fclose(f);
            return -1;
        }
        int n_keep_wp = (int)n_wp;
        if (n_keep_wp > MAX_WAYPOINTS_PER_OBS) {
            fprintf(stderr, "world_loader: obstacle %d has %u waypoints; capping to %d\n",
                    i, n_wp, MAX_WAYPOINTS_PER_OBS);
            n_keep_wp = MAX_WAYPOINTS_PER_OBS;
        }
        float buf[3];
        if (i < n_keep) {
            ObstacleTrajectory* tr = &traj_out[i];
            tr->num_waypoints = n_keep_wp;
            for (uint32_t w = 0; w < n_wp; w++) {
                if (fread(buf, sizeof(float), 3, f) != 3) {
                    fprintf(stderr, "world_loader: short waypoint %u/%u in obstacle %d of %s\n",
                            w, n_wp, i, path);
                    fclose(f);
                    return -1;
                }
                if ((int)w < n_keep_wp) {
                    tr->t[w] = buf[0];
                    tr->x[w] = buf[1];
                    tr->y[w] = buf[2];
                }
            }
        } else {
            // Skip this obstacle's waypoints
            if (fseek(f, (long)(n_wp * 3 * sizeof(float)), SEEK_CUR) != 0) {
                fprintf(stderr, "world_loader: seek past obstacle %d failed in %s\n",
                        i, path);
                fclose(f);
                return -1;
            }
        }
    }
    fclose(f);
    return n_keep;
}
