#!/usr/bin/env python3
"""Render one gif per world from a published-worlds eval directory.

For each `world_NNN.parquet` under `--per-world-dir`, render an animation of
the FIRST episode showing:
  - arena border
  - goal (green outline circle + center dot)
  - full obstacle trajectories as faint background lines
  - current obstacle positions per frame
  - robot trail (cyan line) + current pose (cyan circle with white heading)
  - title with world idx, difficulty, outcome

Each gif is ~600 frames at fps=30 → ~20 s gif per world.

Usage:
  python tools/render_eval_gifs.py \\
      --per-world-dir runs/.../per_world \\
      --classified external/baked_worlds_classified.json \\
      --out-dir runs/.../per_world/gifs \\
      [--fps 30] [--max-worlds 60]
"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path

import cv2
import imageio.v2 as imageio
import numpy as np
import polars as pl


ARENA_HALF = 10.0
SIZE = 480
OBS_R_M = 0.5
ROBOT_R_M = 0.30
GOAL_R_DRAW = 0.5

OUTCOME_LABEL = {0: "ongoing", 1: "success", 2: "collision", 3: "timeout"}
OUTCOME_COLOR_BGR = {
    0: (180, 180, 180),
    1: ( 80, 220,  80),    # green
    2: ( 80,  80, 220),    # red
    3: ( 80, 180, 220),    # amber
}


def world_to_px(x: float, y: float, half: float = ARENA_HALF, size: int = SIZE) -> tuple[int, int]:
    s = size / (2 * half)
    return int(size / 2 + x * s), int(size / 2 - y * s)


def episode_obstacle_cols(df: pl.DataFrame) -> list[tuple[str, str]]:
    pairs = []
    for c in df.columns:
        if c.startswith("o") and c.endswith("_x") and c[1:-2].isdigit():
            ycol = c[:-2] + "_y"
            if ycol in df.columns and df[c].is_not_null().any() and not df[c].is_nan().all():
                pairs.append((c, ycol))
    return pairs


def precompute_obstacle_paths(ep: pl.DataFrame) -> list[np.ndarray]:
    """List of (T, 2) int arrays of pixel coordinates per obstacle."""
    paths = []
    for cx, cy in episode_obstacle_cols(ep):
        xs = ep[cx].to_numpy()
        ys = ep[cy].to_numpy()
        # Drop NaN slots (unused obstacle indices)
        mask = ~(np.isnan(xs) | np.isnan(ys))
        if not mask.any():
            continue
        xs = xs[mask]; ys = ys[mask]
        pts = np.array([world_to_px(float(x), float(y)) for x, y in zip(xs, ys)], dtype=np.int32)
        paths.append(pts)
    return paths


def draw_frame(t: int,
               ep: pl.DataFrame,
               obs_paths: list[np.ndarray],
               obs_cols: list[tuple[str, str]],
               robot_xy_px: np.ndarray,
               thetas: np.ndarray,
               goal_px: tuple[int, int],
               outcome: int,
               world_idx: int,
               difficulty: str,
               size: int = SIZE) -> np.ndarray:
    img = np.full((size, size, 3), 15, dtype=np.uint8)
    cv2.rectangle(img, (1, 1), (size - 2, size - 2), (180, 180, 80), 1)

    # Faint full obstacle trajectories
    for path in obs_paths:
        if len(path) >= 2:
            cv2.polylines(img, [path], False, (55, 55, 90), 1, cv2.LINE_AA)

    # Current obstacle positions
    obs_r_px = int(OBS_R_M * size / (2 * ARENA_HALF))
    for cx, cy in obs_cols:
        x = ep[cx].item(t); y = ep[cy].item(t)
        if x is None or y is None or (isinstance(x, float) and math.isnan(x)):
            continue
        px, py = world_to_px(float(x), float(y), size=size)
        cv2.circle(img, (px, py), obs_r_px, (60, 60, 200), -1, cv2.LINE_AA)

    # Goal
    cv2.circle(img, goal_px, int(GOAL_R_DRAW * size / (2 * ARENA_HALF) * 3),
               (80, 220, 80), 2, cv2.LINE_AA)
    cv2.circle(img, goal_px, 3, (80, 220, 80), -1)

    # Robot trail so far
    if t >= 1:
        cv2.polylines(img, [robot_xy_px[:t + 1]], False, (220, 220, 80), 2, cv2.LINE_AA)

    # Robot pose
    rpx, rpy = int(robot_xy_px[t, 0]), int(robot_xy_px[t, 1])
    robot_r_px = int(ROBOT_R_M * size / (2 * ARENA_HALF))
    cv2.circle(img, (rpx, rpy), robot_r_px, (220, 220, 80), -1, cv2.LINE_AA)
    th = float(thetas[t])
    hx = int(rpx + 18 * math.cos(th))
    hy = int(rpy - 18 * math.sin(th))   # screen y is inverted
    cv2.line(img, (rpx, rpy), (hx, hy), (240, 240, 240), 2, cv2.LINE_AA)

    # Outcome-coloured border
    cv2.rectangle(img, (0, 0), (size - 1, size - 1),
                  OUTCOME_COLOR_BGR.get(outcome, (200, 200, 200)), 3)

    # HUD
    cv2.putText(img, f"world {world_idx:03d}  {difficulty}  step {t}  {OUTCOME_LABEL.get(outcome, '?')}",
                (8, 18), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (240, 240, 240), 1, cv2.LINE_AA)
    return img


def render_world_gif(parquet_path: Path,
                     out_path: Path,
                     world_idx: int,
                     difficulty: str,
                     fps: int = 30,
                     size: int = SIZE,
                     crf: int = 28) -> dict:
    df = pl.read_parquet(parquet_path)
    if df.is_empty():
        return {"world": world_idx, "skipped": "empty"}
    # Pick first episode
    first_ep_id = df["episode"].min()
    ep = df.filter(pl.col("episode") == first_ep_id).sort("tick")
    T = ep.height
    if T < 2:
        return {"world": world_idx, "skipped": f"only {T} steps"}

    obs_cols = episode_obstacle_cols(ep)
    # NOTE: precompute paths/goal at the current `size`
    def w2p(x, y):
        return world_to_px(x, y, size=size)
    obs_paths = []
    for cx, cy in obs_cols:
        xs = ep[cx].to_numpy()
        ys = ep[cy].to_numpy()
        mask = ~(np.isnan(xs) | np.isnan(ys))
        if not mask.any():
            continue
        xs = xs[mask]; ys = ys[mask]
        pts = np.array([w2p(float(x), float(y)) for x, y in zip(xs, ys)], dtype=np.int32)
        obs_paths.append(pts)

    rxs = ep["robot_x"].to_numpy()
    rys = ep["robot_y"].to_numpy()
    thetas = ep["theta"].to_numpy()
    robot_xy_px = np.array([w2p(float(x), float(y)) for x, y in zip(rxs, rys)], dtype=np.int32)
    goal_px = w2p(float(ep["goal_x"].item(0)), float(ep["goal_y"].item(0)))

    outcome = int(ep["outcome"].tail(1).item())

    ext = out_path.suffix.lower()
    if ext == ".gif":
        writer = imageio.get_writer(str(out_path), fps=fps, codec=None)
    elif ext == ".webm":
        # VP9 — broad browser support, small at our quality target.
        writer = imageio.get_writer(
            str(out_path),
            fps=fps,
            codec="libvpx-vp9",
            pixelformat="yuv420p",
            macro_block_size=8,
            ffmpeg_params=["-crf", str(crf), "-b:v", "0", "-row-mt", "1", "-cpu-used", "4"],
        )
    else:
        # h264; yuv420p for broad player compat; faststart for web.
        writer = imageio.get_writer(
            str(out_path),
            fps=fps,
            codec="libx264",
            quality=None,
            pixelformat="yuv420p",
            macro_block_size=8,
            ffmpeg_params=["-crf", str(crf), "-preset", "veryfast", "-movflags", "+faststart"],
        )
    try:
        for t in range(T):
            frame = draw_frame(t, ep, obs_paths, obs_cols, robot_xy_px, thetas,
                               goal_px, outcome, world_idx, difficulty, size=size)
            writer.append_data(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))
    finally:
        writer.close()
    return {"world": world_idx, "frames": T, "outcome": OUTCOME_LABEL.get(outcome, "?")}


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--per-world-dir", required=True)
    p.add_argument("--classified", required=True)
    p.add_argument("--out-dir", required=True)
    p.add_argument("--fps", type=int, default=30)
    p.add_argument("--max-worlds", type=int, default=60)
    p.add_argument("--ext", default="webm", choices=["gif", "mp4", "webm"])
    p.add_argument("--size", type=int, default=SIZE, help="output pixel size (square)")
    p.add_argument("--crf", type=int, default=33,
                   help="quality: lower = better. h264 default 28, vp9 default 33")
    args = p.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    classified = json.loads(Path(args.classified).read_text())

    paths = sorted(Path(args.per_world_dir).glob("world_*.parquet"))[:args.max_worlds]
    for path in paths:
        m = re.match(r"world_(\d+)", path.stem)
        if not m:
            continue
        idx = int(m.group(1))
        diff = classified.get(str(idx), {}).get("difficulty", "?")
        out_path = out_dir / f"world_{idx:03d}.{args.ext}"
        info = render_world_gif(path, out_path, idx, diff,
                                fps=args.fps, size=args.size, crf=args.crf)
        print(f"world_{idx:03d} ({diff}): {info}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
