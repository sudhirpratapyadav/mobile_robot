# Plan — Day 2+ (architectural changes)

Status entering Day 2: best so far is **`e7sadb3v`** (beta=10, 500M):
- train-dist clean_reach **67 %**
- paper-eval overall **44.7 %** (E 55.5 / M 44.5 / H 34.0)

Day-1 (β sweep) closed: β=10 is the local optimum. Going architectural.

Two metrics we are tracking and optimising for, **honestly, no cheating**:
1. **clean_reach** on train-distribution (reach goal AND zero collisions before reach)
2. **success** on paper-eval (60 published worlds × 10 trials)

Hardware: **2 × A6000 (48 GB each)**. Will run 2 trainings in parallel via
`CUDA_VISIBLE_DEVICES=0/1`.

## Method

For each architectural axis: **one baseline change** vs `e7sadb3v`, then if
it lifts at least one of the two metrics, **2-3 small reward/parameter
ablations** on top. If it doesn't win, kill the axis and move to the next.

Per-experiment budget: **500M steps** (~2 h per A6000) for the first pass.
Promising ones get a 1B follow-up.

## Order (decided in conversation)

1. **OBS**: history-stacked observation (predict obstacle motion).
2. **NN**: recurrent (GRU) and/or CNN on costmap.
3. **ACTION**: action chunking (predict K future actions).
4. **MODEL-BASED**: next-state prediction auxiliary loss / MPC.

Each axis multi-day. Each layer **builds on the winning recipe from the
previous layer**, not on `e7sadb3v` blindly.

## Axis 1 — Observation: history stacking

### A1.1 — 2-frame stacked costmap (current + previous)

**Delta vs e7sadb3v**: obs becomes `[costmap_t, costmap_{t-1}, v, w, gx_b, gy_b]`
= 2 × 4096 + 4 = 8196 floats. Same MLP backbone (the native default).

**Implementation**: env keeps a 1-step ring buffer of the previous costmap,
emits both. New env knob `history_len` ∈ {1, 2, 5}.

**Hypothesis**: 1-frame diff is enough for the policy to perceive "obstacle
moving towards me" vs "obstacle receding." Tests the largest single
hypothesis in `docs/options.md`.

**Pass criterion**: clean_reach ≥ 70 % AND paper ≥ 47 %.

### A1.2 — 5-frame stacked costmap

Only if A1.1 wins. Tests whether more history helps further.

### A1.3 — Velocity-encoded single-frame costmap (no history)

Cheap ablation: instead of stacking, mark cells in the costmap with the
estimated obstacle radial velocity (range-rate from one-frame diff). Tests
"the policy doesn't need raw history, just the derived motion."

## Axis 2 — Policy network

### A2.1 — MinGRU → LSTM (still flat MLP encoder)

Cheap config flip (`network = LSTM` in `[torch]` if going `--slowly`, OR
extend native to support LSTM). Tests whether the existing MinGRU is the
bottleneck.

### A2.2 — CNN encoder (the one we've never actually trained)

Use `--slowly` PyTorch backend so the `CostmapEncoder64` we wrote earlier
actually fires. Native backend silently ignores `[torch]` for non-ocean
envs; documented in `logger.md` 2026-05-17.

Cost: `--slowly` is ~3-5× slower SPS. 500M will take ~6-10 h instead of
2 h. Fits one GPU overnight while the other runs Axis 1 ablations.

### A2.3 — Transformer over short scan history

Only if Axis 1 (history) lands a clear win. Stack of e.g. 5 costmap frames
with a small (4-layer, 64-d) transformer. Most expensive day-2 item.

## Axis 3 — Action

### A3.1 — Action chunking K=5

Policy outputs 5 × 2 actions; env executes the first one, refreshes obs,
discards rest. Forces multi-step planning at the action level (LfH-CP
style).

### A3.2 — Bounded Beta action distribution

Replace Gaussian + clip with Beta-on-[-1, 1]. Removes the dead-zone at
the action-boundary that Gaussian has. Cheap.

### A3.3 — Action-smoothness penalty

`r -= λ · |a_t - a_{t-1}|`. Cheap; complements accel limits.

## Axis 4 — Model-based / auxiliary prediction

### A4.1 — Next-costmap prediction auxiliary loss

Add a head that predicts the costmap at t+1 from the current obs +
action; train it jointly with the policy. The shared encoder is pushed
to learn dynamics-relevant features.

### A4.2 — Value-equivalent latent world model

Sketch: small TD-MPC-style head. Big lift; tabled until 4.1 lands.

## Per-experiment workflow (locked-in process)

Per training run:

1. **Decide one delta**, write into `dyna_train.ini` or CLI overrides.
   Commit the ini change (no per-run commits to .ini if it's a CLI
   override).
2. Launch on free GPU. Two GPUs = two runs concurrent.
3. When done: render 100 train-distribution WebMs with `info.json`.
4. Run paper eval (`run_eval_published.sh <ckpt> 10 600`) + render 60 WebMs.
5. Update `exp_tracker.md` with the new run entry using the existing
   template; include both metrics + diagnosis text.
6. Update headline summary table in `exp_tracker.md`.
7. Commit when a clear axis result is in (winning or not).

## Decision discipline

- **One change per run.** No multi-variable runs.
- **No fishing**: if I'm tempted to retry a variant with a different
  seed because the first didn't work, that's the wrong call — different
  seeds for the *same* config don't tell us anything new at our budget.
- **Promote winners, kill losers.** A change that doesn't improve EITHER
  metric within 500M is dead.

## Logging files to keep current

- `exp_tracker.md` — one entry per run.
- `logger.md` — only for code/infra changes (env knobs, new fields,
  new tools).
- `README.md` — refresh headline result when a new best lands.
- `docs/plan_day_2.md` (this file) — refresh as axes close.

## Cheating list — things we will NOT do

- Train on the eval set (`runs/eval/baseline_random/{easy,medium,hard}.parquet`
  exists only for baseline rollouts; never used as a training source).
- Tune env-config to match a specific eval world's geometry.
- Use ground-truth obstacle positions in obs (only LiDAR-derived costmap).
- Add a hidden reward that the eval env happens to share.
- Initialise weights from a checkpoint that was eval-selected (we may
  warm-start from `e7sadb3v` for a *training-curve* speedup, but the
  warm-start ckpt is selected on training-time metrics only, never on
  paper-eval).
