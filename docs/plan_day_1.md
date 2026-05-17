# Plan — Day 1 (2026-05-17)

This is the planning doc for the next batch of experiments. Built on top of
the current best run **`e7sadb3v`** (beta=10, 500M, v_max=2.0): **44.7 %
paper-eval overall** (E 55.5 / M 44.5 / H 34.0), beating reported LfH-CP
SOTA of 30.83 % by +13.9 pp.

## Context that's changed since `docs/options.md` was written

`docs/options.md` was drafted when our best was the 20.8 % `ukwnlxj3` run
and we believed eval `v_max=0.5` was the spec. Several anchors there are
stale; do not lift conclusions from it without re-checking. Specifically:

- The "locked envelope" of v=0.5 is wrong — paper Fig. 3 caption says
  v_max=2.0 and the 0.5 figure was from move_base's local planner cap,
  which is a baseline's planning constraint, not the env spec.
- The "do-nothing collapse" failure mode (`b3vm6fej`, `v6r7zeja`) was
  triggered by the 0.5 m/s envelope; with v_max=2.0 the policy no
  longer collapses (verified in `lptujnh0`, `e7sadb3v`).
- Costmap CNN was thought broken in `v6r7zeja` (0 % at 500M) — but that
  run was also v=0.5 era. With the corrected envelope it has not yet
  been tested.
- Our current "best baseline" is **e7sadb3v** (44.7 %, MLP+MinGRU on
  4100-d flat costmap obs, beta=10, 500M, v_max=2.0). All comparisons
  below are relative to this.

## What we still don't know about `e7sadb3v`

- Whether **beta** between 10 and 50 has a sweet spot (we know 10 wins,
  50 collapses; 3 / 5 / 15 / 20 unexplored).
- Whether **longer training** (1B / 2B) keeps lifting clean_reach.
- Whether **multi-frame observation** would do for paper-eval what it
  was hypothesised to do (predict obstacle motion).
- Whether the **CNN costmap encoder** (which the native PufferLib
  backend silently ignored — see `logger.md` 2026-05-17) actually beats
  the flat MLP at v_max=2.0. We've never trained a real CNN here.

## Day-1 batch — 4 cheap experiments around `e7sadb3v`

Each is 500M steps (~2 h). Total wall ≈ 8 h sequential, but they can also
queue overnight. All keep `e7sadb3v`'s baseline config except the single
delta listed. **One change per run** — the discipline that got us this
far.

| # | Run tag (wandb group) | Delta vs `e7sadb3v` | Why | Expected outcome |
|---|---|---|---|---|
| 1 | `beta5` | `--env.beta 5.0` | Finds the sweet spot between 1 (36 %) and 10 (67 %). Beta sweep was non-monotonic; we need at least one mid-point datum. | If clean_reach ≥ 60 %, midpoint is fine and we don't gain from beta=10. If < 50 %, beta=10 is near-optimal — skip 15/20 too. |
| 2 | `beta15` | `--env.beta 15.0` | Same logic, other side. Will tell us if there's headroom up. | If clean_reach ≥ 70 % and paper > 45 %, push to 20. Else lock 10. |
| 3 | `beta10_long_1b` | `--train.total-timesteps 1000000000` (everything else identical) | Cheapest gain — does the same recipe just want more steps? | If paper > 50 %, do another 2B run. If flat at 45 %, recipe is saturated. |
| 4 | `beta10_speedmatch` | `--env.speed-max 1.0` (cap obstacle speed at robot's reverse cap) | Removes the "obstacle outruns the robot" failure mode in training. Doesn't change eval (baked worlds), so this is purely a training-distribution simplification. | If train clean_reach jumps a lot but paper drops, we're over-simplifying. If both improve, the asymmetry was hurting more than helping. |

### Why these four, not the options.md picks

`options.md` recommends A1 (history stack), B3 (GRU), D1 (Dijkstra),
C1 (action chunking). All require code (env or torch backend). The
day-1 list above is **config-only** — same env binary, same .ini, just
different CLI overrides. That's the right thing when:

- We just got a +14 pp jump from a single bug-fix (v_max), so cheap
  parameter sweeps may still have low-hanging fruit.
- Each experiment is 2 h not 2 days; fast iteration > big swings.

**Code changes go to Day 2** once we know the parameter landscape.

## Day-1 budget

- Sequential: ~8 h wall (4 × 2 h training) + ~30 min eval/render each =
  ~10 h. Fits one day.
- One GPU; can't actually parallelise training.

## Decision gates

After Day 1 results, we'll know:

1. **Beta optimum**: do we have a non-monotonic gain inside [5, 15] or
   has the recipe saturated?
2. **Scaling**: does 1B beat 500M, and by how much?
3. **Distribution alignment**: does matching the robot/obstacle speed
   help or hurt?

If headline jumps past 50 %, push to Day 2 with longer-training of the
best variant. If it plateaus near 45 %, **then** switch to architectural
changes (A1, B3, D1) because parameter tuning is exhausted.

## Day-2 candidates (queue, not commit)

These are the architectural moves we'd consider after Day 1. Listed
in expected-cost order, cheapest first.

1. **B3 — GRU policy** (`use_rnn=1`). Flip the flag, retrain. Tests
   memory directly without changing the env. Probably the biggest
   single bang/buck if it works.
2. **A2 — already done in current obs** (`v, w` extras are present).
   Skip.
3. **A1 — history-stacked LiDAR** (5 frames). Env-side change. Tests
   "policy lacks motion prediction" hypothesis. Slower SPS but unique
   info gain.
4. **B1/B2 — CNN-on-costmap (real this time)**. Requires either
   `--slowly` Python backend (PyTorch CostmapEncoder64 already wired,
   just unused by native) or implementing a C-native custom encoder
   hook (see `task #57`). Start with `--slowly` for the smoke test.
5. **D1 — global Dijkstra waypoint** instead of absolute goal. Big
   structural change but matches LfH-CP's deploy story; expected
   large gain.

## Logging discipline

Each Day-1 run:

1. Append a new entry to `exp_tracker.md` after eval lands, using
   the same template as the existing `e7sadb3v`/`8wxdf0mh`/`1w2zazvt`
   entries.
2. Render 100 train-distribution WebMs into
   `runs/train/dyna_train/<wandb_id>/train_dist_render/webms/`
   for visual sanity.
3. Run full paper eval (`run_eval_published.sh <ckpt> 10 600`) into
   `runs/train/dyna_train/<wandb_id>/eval_paper/<step>/`.
4. Render the 60 paper-eval WebMs into the same eval_paper subtree.
5. Update headline summary table in `exp_tracker.md`.

## Bigger-picture caveats to remember

- `e7sadb3v`'s 44.7 % is **above reported SOTA**, but our procedural
  training distribution is broader and our v_max envelope is slightly
  different from what LfH-CP reports running under. Treat the gap as
  "we're in the right ballpark," not "we've conclusively beaten the
  paper." If Day 1 lands a clean 55–60 %, that's the moment to
  re-read the LfH-CP paper carefully and check what exactly they
  evaluated against.
- We never actually trained a CNN — the native PufferLib backend
  silently substitutes its default MLP+MinGRU when an unknown env
  name appears in `create_custom_encoder()`. Until we add the
  dyna_train hook there (or use `--slowly`), the `[torch]` block in
  the .ini is dead config. This is documented in `logger.md`
  2026-05-17.
