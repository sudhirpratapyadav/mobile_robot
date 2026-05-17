#!/usr/bin/env python3
"""Compare results from multiple training runs (train-dist + paper-eval).

Looks for these files under each `runs/train/dyna_train/<run_id>/`:
  train_dist_render/webms/info.json        — verdict_counts
  eval_paper/<ckpt_stem>/summary.json      — paper success by difficulty

Usage:
  python tools/compare_runs.py run_id1 run_id2 run_id3 ...
  python tools/compare_runs.py --all      # all run dirs with both eval files

Prints a side-by-side table.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1] / "runs/train/dyna_train"


def load(run_id: str) -> dict | None:
    rd = ROOT / run_id
    if not rd.is_dir():
        return None
    out: dict = {"run_id": run_id}

    td = rd / "train_dist_render/webms/info.json"
    if td.exists():
        s = json.loads(td.read_text()).get("summary", {})
        out["td_n"] = s.get("n_episodes", 0)
        out["td"] = s.get("verdict_counts", {})

    ep_dirs = sorted((rd / "eval_paper").glob("*"))
    if ep_dirs:
        # Use the most recent (last) ckpt for paper-eval
        ep_dir = ep_dirs[-1]
        summary = ep_dir / "summary.json"
        if summary.exists():
            s = json.loads(summary.read_text())
            out["paper_overall"] = s.get("overall", {})
            out["paper_by_diff"] = s.get("by_difficulty", {})
    return out


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("run_ids", nargs="*")
    p.add_argument("--all", action="store_true",
                   help="auto-discover run dirs with eval results")
    args = p.parse_args()

    if args.all:
        run_ids = []
        for d in sorted(ROOT.iterdir()):
            if not d.is_dir():
                continue
            if (d / "eval_paper").exists() or (d / "train_dist_render/webms/info.json").exists():
                run_ids.append(d.name)
    else:
        run_ids = args.run_ids

    if not run_ids:
        print("no run_ids; use --all or pass them as args", file=sys.stderr)
        return 1

    rows = [load(r) for r in run_ids]
    rows = [r for r in rows if r is not None]

    print(f"{'run_id':<14} {'td_clean':>9} {'td_dirty':>9} {'td_coll':>8} {'td_to':>6}  "
          f"{'paper':>7} {'easy':>6} {'med':>6} {'hard':>6}")
    for r in rows:
        td = r.get("td", {})
        td_n = r.get("td_n", 0) or 1
        td_clean = td.get("clean_reach", 0)
        td_dirty = td.get("reached_dirty", 0)
        td_coll  = td.get("collision", 0)
        td_to    = td.get("timeout", 0)
        po = r.get("paper_overall", {})
        pn = po.get("n", 0) or 1
        psucc = po.get("success", 0)
        pbd = r.get("paper_by_diff", {})
        def pct(num, den):
            return f"{100*num/den:.1f}%" if den else "—"
        easy = pbd.get("easy", {})
        med  = pbd.get("medium", {})
        hard = pbd.get("hard", {})
        print(f"{r['run_id']:<14} "
              f"{pct(td_clean, td_n):>9} "
              f"{pct(td_dirty, td_n):>9} "
              f"{pct(td_coll,  td_n):>8} "
              f"{pct(td_to,    td_n):>6}  "
              f"{pct(psucc, pn):>7} "
              f"{pct(easy.get('success',0), easy.get('n',0)):>6} "
              f"{pct(med.get('success',0),  med.get('n',0)):>6} "
              f"{pct(hard.get('success',0), hard.get('n',0)):>6}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
