#!/usr/bin/env python3
"""Convert a binary trajectory dump (from dyna_train/dyna_eval standalone
drivers) into a parquet file.

Binary layout (host endianness):
    header: int32[4] = [magic, num_episodes, max_obstacles, lidar_beams]
        magic = 0x44594E41 ('DYNA') for train, 0x44594E45 ('DYNE') for eval
    body:   one Row struct per step, packed:
        ep, tick, n_obs, outcome           (4 × int32)
        rx, ry, theta, v, w, gx, gy        (7 × float32)
        ox[MAX_OBS], oy[MAX_OBS]           (2*max_obs × float32, NaN-padded)
        reward                             (float32)
        done                               (int32)

Outcomes:
    0 ongoing  1 success  2 collision  3 timeout
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import polars as pl


def bake(in_path: Path, out_path: Path) -> dict:
    raw = in_path.read_bytes()
    if len(raw) < 16:
        sys.exit(f"file too small: {in_path}")
    magic, n_eps, max_obs, n_beams = struct.unpack("<4i", raw[:16])
    if magic not in (0x44594E41, 0x44594E45):
        sys.exit(f"unknown magic {magic:#x} (expected DYNA/DYNE)")

    # Row size:  4*int32 + (7 + 2*max_obs + 1)*float32 + 1*int32
    #          = 16 + (8 + 2*max_obs) * 4 + 4
    #          = 20 + (8 + 2*max_obs) * 4
    row_fmt = f"<4i7f{2*max_obs}ff i"
    row_size = struct.calcsize(row_fmt)
    body_size = len(raw) - 16
    if body_size % row_size != 0:
        sys.exit(f"body size {body_size} not divisible by row size {row_size}")
    n_rows = body_size // row_size

    cols: dict[str, list] = {
        "episode": [], "tick": [], "n_obs": [], "outcome": [],
        "robot_x": [], "robot_y": [], "theta": [],
        "v": [], "w": [],
        "goal_x": [], "goal_y": [],
        **{f"o{i}_x": [] for i in range(max_obs)},
        **{f"o{i}_y": [] for i in range(max_obs)},
        "reward": [], "done": [],
    }

    off = 16
    for _ in range(n_rows):
        vals = struct.unpack_from(row_fmt, raw, off)
        off += row_size
        ep, tick, n_o, outcome = vals[0:4]
        rx, ry, th, v, w, gx, gy = vals[4:11]
        # ox[max_obs] then oy[max_obs]
        ox_block = vals[11:11 + max_obs]
        oy_block = vals[11 + max_obs:11 + 2 * max_obs]
        reward, done = vals[-2], vals[-1]

        cols["episode"].append(ep); cols["tick"].append(tick)
        cols["n_obs"].append(n_o); cols["outcome"].append(outcome)
        cols["robot_x"].append(rx); cols["robot_y"].append(ry); cols["theta"].append(th)
        cols["v"].append(v); cols["w"].append(w)
        cols["goal_x"].append(gx); cols["goal_y"].append(gy)
        for i in range(max_obs):
            cols[f"o{i}_x"].append(ox_block[i])
            cols[f"o{i}_y"].append(oy_block[i])
        cols["reward"].append(reward); cols["done"].append(done)

    df = pl.DataFrame(cols)
    df.write_parquet(out_path)

    # Summary stats
    last_per_ep = df.group_by("episode").last().sort("episode")
    n_episodes = last_per_ep.height
    outcomes = last_per_ep["outcome"].value_counts().sort("outcome")
    summary = {
        "in_path": str(in_path),
        "out_path": str(out_path),
        "n_episodes": n_episodes,
        "n_rows": n_rows,
        "max_obstacles": max_obs,
        "lidar_beams": n_beams,
        "outcome_counts": dict(zip(outcomes["outcome"].to_list(), outcomes["count"].to_list())),
    }
    return summary


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("input", help="input .bin file from a standalone driver")
    p.add_argument("output", help="output .parquet file")
    args = p.parse_args()

    summary = bake(Path(args.input), Path(args.output))
    print(f"Wrote {summary['out_path']}")
    print(f"  rows: {summary['n_rows']:,}")
    print(f"  episodes: {summary['n_episodes']}")
    print(f"  outcomes: {summary['outcome_counts']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
