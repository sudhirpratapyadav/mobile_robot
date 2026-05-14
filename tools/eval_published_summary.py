#!/usr/bin/env python3
"""Aggregate per-world parquets into a DynaBARN-published-worlds summary.

Input:
  --per-world-dir   directory with world_NNN.parquet files
  --classified      path to baked_worlds_classified.json
  --out             output summary.json path
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import polars as pl


def summarise_parquet(path: Path) -> dict:
    df = pl.read_parquet(path)
    last = df.group_by("episode").last().sort("episode")
    n = last.height
    succ = int((last["outcome"] == 1).sum())
    coll = int((last["outcome"] == 2).sum())
    tout = int((last["outcome"] == 3).sum())
    return {
        "n": n,
        "success": succ,
        "collision": coll,
        "timeout": tout,
        "success_rate": succ / n if n else 0.0,
    }


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--per-world-dir", required=True)
    p.add_argument("--classified", required=True)
    p.add_argument("--out", required=True)
    args = p.parse_args()

    classified = json.loads(Path(args.classified).read_text())
    by_world: dict[int, dict] = {}
    by_diff: dict[str, dict] = {
        "easy": {"n": 0, "success": 0, "collision": 0, "timeout": 0},
        "medium": {"n": 0, "success": 0, "collision": 0, "timeout": 0},
        "hard": {"n": 0, "success": 0, "collision": 0, "timeout": 0},
    }
    paths = sorted(Path(args.per_world_dir).glob("world_*.parquet"))
    for path in paths:
        m = re.match(r"world_(\d+)", path.stem)
        if not m:
            continue
        idx = int(m.group(1))
        s = summarise_parquet(path)
        by_world[idx] = s
        diff = classified.get(str(idx), {}).get("difficulty", "unknown")
        if diff in by_diff:
            for k in ("n", "success", "collision", "timeout"):
                by_diff[diff][k] += s[k]
    for diff in by_diff:
        n = by_diff[diff]["n"]
        by_diff[diff]["success_rate"] = by_diff[diff]["success"] / n if n else 0.0

    total_n = sum(d["n"] for d in by_diff.values())
    total_s = sum(d["success"] for d in by_diff.values())
    overall = {
        "n": total_n,
        "success": total_s,
        "collision": sum(d["collision"] for d in by_diff.values()),
        "timeout": sum(d["timeout"] for d in by_diff.values()),
        "success_rate": total_s / total_n if total_n else 0.0,
    }

    out = {
        "n_worlds": len(by_world),
        "by_world": {str(k): v for k, v in by_world.items()},
        "by_difficulty": by_diff,
        "overall": overall,
    }
    Path(args.out).write_text(json.dumps(out, indent=2))

    print(f"{'bin':<8} {'N':>5} {'success':>10} {'collision':>10} {'timeout':>10}")
    print("-" * 47)
    for name in ("easy", "medium", "hard"):
        d = by_diff[name]
        print(f"{name:<8} {d['n']:>5} {d['success_rate']:>9.1%}  "
              f"{d['collision']/max(d['n'],1):>9.1%}  "
              f"{d['timeout']/max(d['n'],1):>9.1%}")
    print("-" * 47)
    print(f"overall  {overall['n']:>5} {overall['success_rate']:>9.1%}")
    print()
    print(f"SOTA targets: Dyna-LfLH 22.5%   LfH-CP 30.83%   ours: {overall['success_rate']:.1%}")
    print(f"\nWrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
