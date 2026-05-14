#!/usr/bin/env python3
"""Aggregate outcomes from one or more eval parquet files.

Inputs are pairs of (difficulty_label, parquet_path) — typically easy/medium/hard.
Prints a small table + writes summary.json next to the first parquet.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import polars as pl


def summarise(path: Path) -> dict:
    df = pl.read_parquet(path)
    last = df.group_by("episode").last().sort("episode")
    n = last.height
    counts = dict(zip(last["outcome"].to_list(), [1] * n))   # placeholder
    counts = {o: int((last["outcome"] == o).sum()) for o in (1, 2, 3)}
    return {
        "n_episodes": n,
        "success": counts[1],
        "collision": counts[2],
        "timeout": counts[3],
        "success_rate": counts[1] / n if n else 0.0,
        "collision_rate": counts[2] / n if n else 0.0,
        "timeout_rate": counts[3] / n if n else 0.0,
    }


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--easy", required=True, help="path to easy parquet")
    p.add_argument("--medium", required=True, help="path to medium parquet")
    p.add_argument("--hard", required=True, help="path to hard parquet")
    p.add_argument("--out", default=None, help="optional summary.json output")
    args = p.parse_args()

    bins = {
        "easy":   summarise(Path(args.easy)),
        "medium": summarise(Path(args.medium)),
        "hard":   summarise(Path(args.hard)),
    }
    total_n = sum(b["n_episodes"] for b in bins.values())
    total_s = sum(b["success"] for b in bins.values())
    overall = total_s / total_n if total_n else 0.0

    print(f"{'bin':<8} {'N':>5} {'success':>10} {'collision':>10} {'timeout':>10}")
    print("-" * 47)
    for name, b in bins.items():
        print(f"{name:<8} {b['n_episodes']:>5} "
              f"{b['success_rate']:>9.1%}  "
              f"{b['collision_rate']:>9.1%}  "
              f"{b['timeout_rate']:>9.1%}")
    print("-" * 47)
    print(f"overall  {total_n:>5} {overall:>9.1%}")
    print()
    print(f"SOTA targets: Dyna-LfLH 22.5%   LfH-CP 30.83%   ours: {overall:.1%}")

    if args.out:
        out_path = Path(args.out)
        out_path.write_text(json.dumps({
            "bins": bins,
            "overall_success_rate": overall,
            "overall_n": total_n,
        }, indent=2))
        print(f"\nWrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
