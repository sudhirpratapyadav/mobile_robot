#!/usr/bin/env python3
"""OpenCV-based trajectory viz for dyna_barn parquet files.

Modes:
    viz_cv.py traj.parquet out.png                  # single auto-grid PNG (≤25 eps)
    viz_cv.py traj.parquet out_dir/ --per-episode   # one PNG per episode
    viz_cv.py traj.parquet out_dir/ --batch 25      # 5×5 grid pages
    viz_cv.py traj.parquet out.mp4                  # animation (mp4)
    viz_cv.py traj.parquet out.gif                  # animation (gif)

Uses cv2 primitives for ~100× speed over matplotlib. Per-robot colour, obstacle
paths for dynamic envs, outcome-tagged border per episode.
"""
from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import cv2
import imageio.v2 as imageio
import numpy as np
import polars as pl


# ---------- constants
TILE = 320                # pixels per episode tile
PAD = 6
ARENA_HALF = 10.0
LIDAR_RANGE = 30.0
GOAL_RADIUS_DRAW = 0.5
OBSTACLE_RADIUS = 0.5
ROBOT_RADIUS = 0.30

OUTCOME_LABEL = {0: "ongoing", 1: "success", 2: "collision", 3: "timeout"}
OUTCOME_COLOR = {
    0: (180, 180, 180),
    1: ( 80, 220,  80),   # green
    2: ( 80,  80, 220),   # red  (BGR — red here)
    3: ( 80, 180, 220),   # amber
}


def world_to_px(x: float, y: float, half: float, size: int) -> tuple[int, int]:
    s = size / (2 * half)
    px = int(size / 2 + x * s)
    py = int(size / 2 - y * s)   # flip y so +y is up
    return px, py


def episode_obstacle_cols(df: pl.DataFrame) -> list[tuple[str, str]]:
    pairs = []
    for c in df.columns:
        if c.startswith("o") and c.endswith("_x") and c[1:-2].isdigit():
            ycol = c[:-2] + "_y"
            if ycol in df.columns:
                # at least one non-null in either column
                if df[c].is_not_null().any() and not df[c].is_nan().all():
                    pairs.append((c, ycol))
    return pairs


def is_dynamic(ep_df: pl.DataFrame, pairs: list[tuple[str, str]]) -> bool:
    if ep_df.height < 2 or not pairs:
        return False
    for cx, cy in pairs:
        s0 = ep_df[cx].head(1).to_list()[0]
        sN = ep_df[cx].tail(1).to_list()[0]
        if s0 is None or sN is None:
            continue
        if abs(s0 - sN) > 1e-3:
            return True
        s0 = ep_df[cy].head(1).to_list()[0]
        sN = ep_df[cy].tail(1).to_list()[0]
        if s0 is not None and sN is not None and abs(s0 - sN) > 1e-3:
            return True
    return False


def draw_episode(ep_df: pl.DataFrame, size: int = TILE, half: float = ARENA_HALF) -> np.ndarray:
    img = np.full((size, size, 3), 12, dtype=np.uint8)
    # arena border
    cv2.rectangle(img, (1, 1), (size - 2, size - 2), (180, 180, 80), 1)

    pairs = episode_obstacle_cols(ep_df)
    dyn = is_dynamic(ep_df, pairs)

    # Obstacle paths (faint) + final cylinder
    for cx, cy in pairs:
        xs = ep_df[cx].drop_nulls().to_numpy()
        ys = ep_df[cy].drop_nulls().to_numpy()
        if len(xs) == 0:
            continue
        if dyn and len(xs) > 1:
            pts = np.array([world_to_px(x, y, half, size) for x, y in zip(xs, ys)], dtype=np.int32)
            cv2.polylines(img, [pts], False, (50, 50, 110), 1, cv2.LINE_AA)
        px, py = world_to_px(float(xs[-1]), float(ys[-1]), half, size)
        cv2.circle(img, (px, py), int(OBSTACLE_RADIUS * size / (2 * half)),
                   (60, 60, 200), -1, cv2.LINE_AA)

    # Goal
    gx = ep_df["goal_x"].head(1).to_list()[0]
    gy = ep_df["goal_y"].head(1).to_list()[0]
    gpx, gpy = world_to_px(float(gx), float(gy), half, size)
    cv2.circle(img, (gpx, gpy), int(GOAL_RADIUS_DRAW * size / (2 * half)),
               (80, 220, 80), 2, cv2.LINE_AA)

    # Robot trajectory
    rxs = ep_df["robot_x"].to_numpy()
    rys = ep_df["robot_y"].to_numpy()
    if len(rxs) >= 2:
        pts = np.array([world_to_px(x, y, half, size) for x, y in zip(rxs, rys)], dtype=np.int32)
        cv2.polylines(img, [pts], False, (220, 220, 80), 2, cv2.LINE_AA)

    # Final robot pos
    if len(rxs) > 0:
        rpx, rpy = world_to_px(float(rxs[-1]), float(rys[-1]), half, size)
        cv2.circle(img, (rpx, rpy), int(ROBOT_RADIUS * size / (2 * half)),
                   (220, 220, 80), -1, cv2.LINE_AA)

    # Outcome-coloured border
    last = ep_df.tail(1)
    outcome = int(last["outcome"].to_list()[0]) if "outcome" in ep_df.columns else 0
    col = OUTCOME_COLOR.get(outcome, (200, 200, 200))
    cv2.rectangle(img, (0, 0), (size - 1, size - 1), col, 3)

    # Label
    ep_id = int(last["episode"].to_list()[0])
    text = f"ep {ep_id}  {OUTCOME_LABEL.get(outcome, '?')}"
    cv2.putText(img, text, (6, 16), cv2.FONT_HERSHEY_SIMPLEX, 0.45,
                (240, 240, 240), 1, cv2.LINE_AA)
    return img


def grid_image(tiles: list[np.ndarray], cols: int) -> np.ndarray:
    if not tiles:
        return np.zeros((TILE, TILE, 3), dtype=np.uint8)
    rows = math.ceil(len(tiles) / cols)
    h, w = tiles[0].shape[:2]
    out = np.full((rows * (h + PAD) + PAD, cols * (w + PAD) + PAD, 3), 0, dtype=np.uint8)
    for i, t in enumerate(tiles):
        r, c = i // cols, i % cols
        y0 = PAD + r * (h + PAD)
        x0 = PAD + c * (w + PAD)
        out[y0:y0 + h, x0:x0 + w] = t
    return out


def render_animation(ep_df: pl.DataFrame, out_path: Path, fps: int = 20) -> None:
    rxs = ep_df["robot_x"].to_numpy()
    rys = ep_df["robot_y"].to_numpy()
    pairs = episode_obstacle_cols(ep_df)
    size = 480
    half = ARENA_HALF
    is_gif = out_path.suffix.lower() == ".gif"
    writer = imageio.get_writer(str(out_path), fps=fps, codec=None if is_gif else "libx264")
    try:
        for t in range(len(rxs)):
            sub = ep_df.head(t + 1)
            img = draw_episode(sub, size=size, half=half)
            writer.append_data(cv2.cvtColor(img, cv2.COLOR_BGR2RGB))
    finally:
        writer.close()


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("input", help="episodes.parquet")
    p.add_argument("output", help="output path (PNG/dir/MP4/GIF)")
    p.add_argument("title", nargs="?", default=None)
    p.add_argument("--per-episode", action="store_true", help="dump one PNG per episode into output/")
    p.add_argument("--batch", type=int, default=0, help="paginated grids: episodes per page")
    args = p.parse_args()

    df = pl.read_parquet(args.input)
    if df.is_empty():
        print("empty parquet", file=sys.stderr)
        return 1
    eps = sorted(df["episode"].unique().to_list())
    out_path = Path(args.output)

    # Animation mode
    if out_path.suffix.lower() in (".mp4", ".gif"):
        # one animation of the first episode (caller can iterate externally)
        ep0 = df.filter(pl.col("episode") == eps[0]).sort("tick")
        render_animation(ep0, out_path)
        print(f"Wrote animation {out_path}")
        return 0

    if args.per_episode:
        out_path.mkdir(parents=True, exist_ok=True)
        for ep in eps:
            sub = df.filter(pl.col("episode") == ep).sort("tick")
            img = draw_episode(sub)
            cv2.imwrite(str(out_path / f"ep{ep:04d}.png"), img)
        print(f"Wrote {len(eps)} per-episode PNGs to {out_path}/")
        return 0

    if args.batch > 0:
        out_path.mkdir(parents=True, exist_ok=True)
        per_page = args.batch
        cols = int(math.sqrt(per_page))
        if cols * cols != per_page:
            cols = math.ceil(math.sqrt(per_page))
        n_pages = math.ceil(len(eps) / per_page)
        for page in range(n_pages):
            tiles = []
            for ep in eps[page * per_page:(page + 1) * per_page]:
                sub = df.filter(pl.col("episode") == ep).sort("tick")
                tiles.append(draw_episode(sub))
            img = grid_image(tiles, cols)
            cv2.imwrite(str(out_path / f"page{page:03d}.png"), img)
        print(f"Wrote {n_pages} grid pages to {out_path}/")
        return 0

    # Single auto-grid PNG
    cols = min(5, max(1, int(math.ceil(math.sqrt(len(eps))))))
    tiles = []
    for ep in eps[:25]:
        sub = df.filter(pl.col("episode") == ep).sort("tick")
        tiles.append(draw_episode(sub))
    img = grid_image(tiles, cols)
    cv2.imwrite(str(out_path), img)
    print(f"Wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
