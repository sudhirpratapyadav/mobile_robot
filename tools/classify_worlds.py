#!/usr/bin/env python3
"""Classify each baked DynaBARN world into {easy, medium, hard} per the
paper's Fig. 2 tree:

    obstacles ∈ [5, 10] AND easy motion   → EASY
    obstacles ∈ [5, 10] AND hard motion   → MEDIUM
    obstacles ∈ [10, 20] AND easy motion  → MEDIUM
    obstacles ∈ [10, 20] AND hard motion  → HARD

Motion profile is inferred from mean per-segment speed across all obstacles:
    mean_speed < 1.0 m/s    → easy motion
    mean_speed ≥ 1.0 m/s    → hard motion

(Paper Table I uses easy = [0.5, 1.0], hard = [1.0, 2.0] — the 1.0 m/s cut is
on the boundary so we use ≥ 1.0 → hard.)

Writes a JSON map {world_idx: {"difficulty": "easy"|"medium"|"hard",
                              "n_obstacles": int, "mean_speed": float}}
"""
from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from pathlib import Path


MAGIC = 0x44425257


def load_world(path: Path) -> list[list[tuple[float, float, float]]]:
    blob = path.read_bytes()
    magic, version, n_obs = struct.unpack_from("<III", blob, 0)
    assert magic == MAGIC
    off = 12
    trajs = []
    for _ in range(n_obs):
        (n_wp,) = struct.unpack_from("<I", blob, off); off += 4
        wps = []
        for _ in range(n_wp):
            t, x, y = struct.unpack_from("<fff", blob, off); off += 12
            wps.append((t, x, y))
        trajs.append(wps)
    return trajs


def mean_segment_speed(trajs: list[list[tuple[float, float, float]]]) -> float:
    speeds = []
    for wps in trajs:
        for a, b in zip(wps[:-1], wps[1:]):
            dt = b[0] - a[0]
            if dt <= 0:
                continue
            d = math.hypot(b[1] - a[1], b[2] - a[2])
            speeds.append(d / dt)
    return sum(speeds) / len(speeds) if speeds else 0.0


def classify(n_obs: int, mean_speed: float) -> str:
    low_count = n_obs < 10
    easy_motion = mean_speed < 1.0
    if low_count and easy_motion:
        return "easy"
    if low_count and not easy_motion:
        return "medium"
    if (not low_count) and easy_motion:
        return "medium"
    return "hard"


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--baked-dir", required=True)
    p.add_argument("--out", required=True)
    args = p.parse_args()

    out: dict[str, dict] = {}
    counts = {"easy": 0, "medium": 0, "hard": 0}
    baked = sorted(Path(args.baked_dir).glob("world_*.bin"),
                   key=lambda p: int(p.stem.split("_")[1]))
    for path in baked:
        idx = int(path.stem.split("_")[1])
        trajs = load_world(path)
        n_obs = len(trajs)
        ms = mean_segment_speed(trajs)
        diff = classify(n_obs, ms)
        out[str(idx)] = {
            "path": str(path),
            "n_obstacles": n_obs,
            "mean_speed": round(ms, 4),
            "difficulty": diff,
        }
        counts[diff] += 1

    Path(args.out).write_text(json.dumps(out, indent=2))
    print(f"Wrote {args.out}")
    print(f"Distribution: easy={counts['easy']}  medium={counts['medium']}  hard={counts['hard']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
