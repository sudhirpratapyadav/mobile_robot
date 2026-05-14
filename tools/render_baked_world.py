#!/usr/bin/env python3
"""Render a baked world (output of bake_worlds.py) for visual verification.

Produces:
  - One PNG per requested time slice with the obstacle positions at that time
  - One PNG showing the full obstacle trajectories overlaid

Usage:
  python tools/render_baked_world.py world_000.bin out_dir/
  python tools/render_baked_world.py world_000.bin out_dir/ --times 0,5,10,20,40
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import cv2
import numpy as np


MAGIC = 0x44425257
ARENA_HALF = 10.0
SIZE = 540
OBS_R_M = 0.5


def read_baked(path: Path):
    blob = path.read_bytes()
    magic, version, n_obs = struct.unpack_from("<III", blob, 0)
    assert magic == MAGIC, f"bad magic {magic:#x}"
    off = 12
    trajectories = []
    for _ in range(n_obs):
        (n_wp,) = struct.unpack_from("<I", blob, off); off += 4
        wps = []
        for _ in range(n_wp):
            t, x, y = struct.unpack_from("<fff", blob, off)
            off += 12
            wps.append((t, x, y))
        trajectories.append(wps)
    return trajectories


def interp_pos(wps, tau):
    if not wps:
        return None, None
    if tau <= wps[0][0]:
        return wps[0][1], wps[0][2]
    if tau >= wps[-1][0]:
        return wps[-1][1], wps[-1][2]
    i = 1
    while i < len(wps) and wps[i][0] < tau:
        i += 1
    t0, x0, y0 = wps[i - 1]
    t1, x1, y1 = wps[i]
    alpha = (tau - t0) / (t1 - t0)
    return x0 + alpha * (x1 - x0), y0 + alpha * (y1 - y0)


def world_to_px(x, y, half=ARENA_HALF, size=SIZE):
    s = size / (2 * half)
    return int(size / 2 + x * s), int(size / 2 - y * s)


def render_overlay(trajectories, out_path: Path):
    img = np.full((SIZE, SIZE, 3), 15, dtype=np.uint8)
    cv2.rectangle(img, (1, 1), (SIZE - 2, SIZE - 2), (180, 180, 80), 1)
    colors = [
        (200, 100, 100), (100, 200, 100), (100, 100, 200),
        (200, 200, 100), (200, 100, 200), (100, 200, 200),
    ]
    for i, wps in enumerate(trajectories):
        if not wps:
            continue
        col = colors[i % len(colors)]
        pts = np.array([world_to_px(x, y) for _, x, y in wps], dtype=np.int32)
        cv2.polylines(img, [pts], False, col, 1, cv2.LINE_AA)
        sx, sy = world_to_px(wps[0][1], wps[0][2])
        ex, ey = world_to_px(wps[-1][1], wps[-1][2])
        cv2.circle(img, (sx, sy), int(OBS_R_M * SIZE / (2 * ARENA_HALF)), col, -1)
        cv2.circle(img, (ex, ey), int(OBS_R_M * SIZE / (2 * ARENA_HALF)), col, 1)
    cv2.putText(img, f"{len(trajectories)} trajectories overlay",
                (8, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (240, 240, 240), 1, cv2.LINE_AA)
    cv2.imwrite(str(out_path), img)


def render_snapshot(trajectories, tau, out_path: Path):
    img = np.full((SIZE, SIZE, 3), 15, dtype=np.uint8)
    cv2.rectangle(img, (1, 1), (SIZE - 2, SIZE - 2), (180, 180, 80), 1)
    for wps in trajectories:
        if len(wps) < 2:
            continue
        pts = np.array([world_to_px(x, y) for _, x, y in wps], dtype=np.int32)
        cv2.polylines(img, [pts], False, (60, 60, 80), 1, cv2.LINE_AA)
    for wps in trajectories:
        x, y = interp_pos(wps, tau)
        if x is None:
            continue
        px, py = world_to_px(x, y)
        cv2.circle(img, (px, py), int(OBS_R_M * SIZE / (2 * ARENA_HALF)),
                   (80, 80, 220), -1, cv2.LINE_AA)
    cv2.putText(img, f"t = {tau:.2f}s",
                (8, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (240, 240, 240), 1, cv2.LINE_AA)
    cv2.imwrite(str(out_path), img)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("baked", help="path to a baked world_NNN.bin")
    p.add_argument("out_dir", help="output directory")
    p.add_argument("--times", default="0,5,10,20,40",
                   help="comma-separated time slices (seconds)")
    args = p.parse_args()

    trajectories = read_baked(Path(args.baked))
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    times = [float(s) for s in args.times.split(",")]
    name = Path(args.baked).stem
    render_overlay(trajectories, out_dir / f"{name}_overlay.png")
    for t in times:
        render_snapshot(trajectories, t, out_dir / f"{name}_t{t:05.1f}.png")
    print(f"{name}: {len(trajectories)} obstacles")
    print(f"  wrote overlay + {len(times)} snapshot PNGs to {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
