#!/usr/bin/env python3
"""Render one WebM per episode from a train-distribution trajectory parquet.

The train-distribution dump (./dyna_train --traj ...) packs N episodes into
a single parquet keyed by `episode`. This script splits by episode and reuses
the per-episode renderer from render_eval_gifs.py to produce N WebMs.

Usage:
  python tools/render_train_dist_webms.py \
      --parquet runs/.../train_dist_render/100ep.parquet \
      --out-dir runs/.../train_dist_render/webms \
      [--fps 30] [--max-episodes 100] [--ext webm] [--size 480] [--crf 33]
      [--arena-half 12.0]
"""
from __future__ import annotations

import argparse
import json
import multiprocessing as mp
import os
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import polars as pl

# Reuse the per-episode renderer. We override its ARENA_HALF since the train
# distribution uses arena_size=24 (half=12), not the 20m default (half=10)
# that render_eval_gifs.py assumes for the published worlds.
sys.path.insert(0, str(Path(__file__).parent))
import render_eval_gifs as reg


OUTCOME_LABEL = {0: "ongoing", 1: "success", 2: "collision", 3: "timeout"}


def _render_one(args_tup):
    """Worker entry-point. Picklable: takes a tuple, returns a dict.

    The temp parquet for this episode is pre-written by the parent (so the
    parent owns the polars DataFrame and the worker never imports polars).
    """
    (tmp_parquet, out_path, ep_id, fps, size, crf, arena_half) = args_tup
    reg.ARENA_HALF = arena_half  # per-process override
    try:
        info = reg.render_world_gif(
            Path(tmp_parquet), Path(out_path),
            world_idx=int(ep_id), difficulty="train",
            fps=fps, size=size, crf=crf,
        )
    finally:
        try:
            Path(tmp_parquet).unlink(missing_ok=True)
        except Exception:
            pass
    return info


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--parquet", required=True)
    p.add_argument("--out-dir", required=True)
    p.add_argument("--fps", type=int, default=30)
    p.add_argument("--max-episodes", type=int, default=10_000)
    p.add_argument("--ext", default="webm", choices=["gif", "mp4", "webm"])
    p.add_argument("--size", type=int, default=reg.SIZE,
                   help="output pixel size (square)")
    p.add_argument("--crf", type=int, default=33,
                   help="quality: lower = better. vp9 default 33")
    p.add_argument("--arena-half", type=float, default=12.0,
                   help="half-arena (m) for world→pixel mapping; "
                        "default matches dyna_train.ini arena_size=24")
    p.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 2) - 1),
                   help="parallel processes (default = cpu_count-1). "
                        "Each episode is one ffmpeg subprocess; "
                        "pool gives near-linear speedup up to physical cores.")
    args = p.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    df = pl.read_parquet(args.parquet)
    if df.is_empty():
        print(f"empty parquet: {args.parquet}", file=sys.stderr)
        return 1

    ep_ids = df["episode"].unique().sort().to_list()[: args.max_episodes]

    # Pre-split: write one temp parquet per episode in the parent. Cheap
    # vs the render itself, and avoids passing polars DataFrames over the
    # process boundary. Each worker reads its own temp file.
    tasks = []
    for ep_id in ep_ids:
        ep = df.filter(pl.col("episode") == ep_id).sort("tick")
        with tempfile.NamedTemporaryFile(suffix=".parquet", delete=False) as tf:
            tmp_path = tf.name
        ep.write_parquet(tmp_path)
        out_path = out_dir / f"ep_{ep_id:03d}.{args.ext}"
        tasks.append((tmp_path, str(out_path), ep_id,
                      args.fps, args.size, args.crf, args.arena_half))

    # Collect per-episode info as workers complete.
    episode_infos: dict[int, dict] = {}
    workers = max(1, args.workers)
    if workers == 1:
        for t in tasks:
            info = _render_one(t)
            ep_id = t[2]
            info["episode"] = int(ep_id)
            info["webm"] = Path(t[1]).name
            episode_infos[ep_id] = info
            print(f"ep_{ep_id:03d}: verdict={info.get('verdict')}  "
                  f"n_obs={info.get('n_obs')}")
    else:
        # Use 'spawn' (not 'fork'): cv2/imageio/polars don't survive a fork-
        # after-import — workers deadlock at first call. Spawn pays a small
        # cold-start tax (~1s per worker) but always works.
        ctx = mp.get_context("spawn")
        with ProcessPoolExecutor(max_workers=workers, mp_context=ctx) as ex:
            futures = {ex.submit(_render_one, t): t for t in tasks}
            for fut in as_completed(futures):
                t = futures[fut]
                ep_id = t[2]
                info = fut.result()
                info["episode"] = int(ep_id)
                info["webm"] = Path(t[1]).name
                episode_infos[ep_id] = info
                print(f"ep_{ep_id:03d}: verdict={info.get('verdict')}  "
                      f"n_obs={info.get('n_obs')}")

    # Aggregate verdicts → headline summary.
    ordered = [episode_infos[k] for k in sorted(episode_infos.keys())]
    verdict_counts: dict[str, int] = {}
    for info in ordered:
        v = info.get("verdict", "ongoing")
        verdict_counts[v] = verdict_counts.get(v, 0) + 1

    total = len(ordered)
    top_summary = {
        "source_parquet": args.parquet,
        "n_episodes":     total,
        "verdict_counts": verdict_counts,
        "verdict_pct":    {k: round(100.0 * v / total, 2)
                           for k, v in verdict_counts.items()} if total else {},
        "fps":            args.fps,
        "size":           args.size,
        "arena_half":     args.arena_half,
    }
    info_path = out_dir / "info.json"
    info_path.write_text(json.dumps(
        {"summary": top_summary, "episodes": ordered},
        indent=2, sort_keys=False))

    print()
    print(f"=== {total} episodes rendered ===")
    for k in sorted(verdict_counts):
        v = verdict_counts[k]
        print(f"  {k:<14} {v:4d}  ({100*v/total:.1f}%)")
    print(f"info: {info_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
