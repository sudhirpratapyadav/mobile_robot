# Experiment tracker (dyna_barn)

ML-experiment-style record of each training run + every DynaBARN-protocol eval
against it: configuration, results, conclusions, observations. Unlike `logger.md`
(which tracks *code changes*), this file tracks *experiments*.

Same per-run template as the parent project:

```
## <run_id> — <one-line summary>
**Trained:** YYYY-MM-DD HH:MM
**Branch / commit:** <sha>
**Hypothesis:** what we were testing
**Config delta vs. previous run:** what was changed and why
**Training config:**
  env: train_env vs eval_env, arena size, obstacle bins, motion profile knobs
  reward: γ/β/σ/success-bonus/collision-penalty
  PPO knobs: lr, ent_coef, minibatch, horizon, total_timesteps
**Result (train):** in-distribution success rate, episode_return
**Result (DynaBARN eval, 600 trials = 20 envs × 10 trials × 3 difficulties):**
  Easy: __%   Medium: __%   Hard: __%   Overall: __%
**Observations:** failure modes, trajectory qualities, training-curve quirks
**Conclusion:** what we believe is true after this run
**Followup:** what to try next
```

---

## SOTA targets (to beat)

From `docs/dyna_barn.md` "Current SOTA":

| Method | Protocol | Overall success | Per-bin (E/M/H) |
|---|---|---|---|
| DWA (paper baseline) | 20×10 per bin, in-dist | — | 78 / 43 / 17 % |
| TD3 (paper baseline) | 20×10 per bin, **train=test** | — | 74 / 60 / 28 % |
| Dyna-LfLH | 60×3, generalisation | **22.5%** | — |
| LfH-CP (current SOTA) | 60×2, generalisation | **30.83%** | — |

Note: TD3's 74/60/28 is **train-on-test**, not directly comparable to our
held-out target. The fair baseline to beat is LfH-CP at 30.83%.

Our target: **beat 30.83% overall** with a single policy trained on procedurally-
generated worlds and evaluated on the DynaBARN eval distribution.

---

## Runs

### `1778772096469` — first end-to-end smoke train (50M steps)

**Trained:** 2026-05-14 ~21:00 local
**Branch / commit:** uncommitted (dyna_barn repo fresh, no commits yet)
**Hypothesis:** Wire up dyna_train end-to-end and confirm the env trains at all
with default PPO settings. Establish a floor success rate to compare against
the later 500M-step run.
**Config delta vs. previous run:** N/A — first run.
**Training config:**
  - env: `dyna_train` (random start/goal, easy bin: 5–10 obstacles, polynomial
    order 1–2, speed 0.5–1.0 m/s, std 0.01–0.1). Arena 20×20 m, max_steps 600,
    dt 0.1.
  - obs: single-frame `[720-d LiDAR / 30 m | gx_body | gy_body]` = 722 floats.
  - action: `(v, w)`, v ∈ [-0.5, 2.0] m/s, w ∈ [-π, π] rad/s.
  - reward: γ_d=1, β=1, σ_o=2 m, success_bonus=1, collision_penalty=10.
  - PPO: lr=5e-4 (annealed → 0.1× over training), ent_coef=0.01, minibatch=8192,
    horizon=64, gamma=0.99, gae_lambda=0.95, total_timesteps=50M, MLP policy
    hidden_size=128 × 2 layers, no RNN.
  - vec: 4096 parallel agents, 4 buffers, 16 threads.
**Result (train):** ~275k SPS on single GPU. Final dashboard stats around
collision-rate ≈ 0.6 in the early-training snapshot, dropping rapidly. Three
checkpoints saved (262k, 26M, 50M-step).

**Result (DynaBARN eval, 600 trials = 200 per difficulty bin):**

| Bin    | Success | Collision | Timeout |
|--------|---------|-----------|---------|
| Easy   | **84.0%** | 16.0% | 0.0% |
| Medium | **71.0%** | 29.0% | 0.0% |
| Hard   | **47.5%** | 52.5% | 0.0% |
| **Overall** | **67.5%** | — | — |

Random-action baseline (same protocol): **1.0% overall** (0.5–1.5% per bin).

**Caveats (important — don't read this as SOTA-beating without context):**
1. Our eval worlds come from the **same trajectory generator** used in
   training, sampled at the published Table I parameter bins. SOTA papers
   (Dyna-LfLH, LfH-CP) evaluate on the **exact 60 published `.world` files**
   from `aninair1905/DynaBARN`. The two distributions are close but not
   identical. We have **not** validated against the published worlds yet.
2. We train on **random start/goal** but eval with **fixed start (0, 9) →
   goal (0, -9)** (paper says (0, 11) → (0, -9), we clamp to in-arena). The
   policy generalises to the fixed route but it's not the same protocol.
3. Eval uses **the same env code** as training (just different config) —
   there is no sim-to-sim transfer step. Real-world deployment via a ROS
   bridge would introduce a gap.

**Observations:**
- Zero timeouts across all 600 trials → episode horizon is comfortable.
- Collision-rate scales monotonically with difficulty (16 / 29 / 53 %),
  consistent with harder motion profiles being genuinely harder.
- The "easy" bin's 84% means the policy isn't yet matching what classical DWA
  achieves in-distribution (78% per paper Fig. 5) — that's the right
  comparison point for our protocol (we're harder than train-on-test TD3 but
  comparable to DWA-style classical planners). Encouraging for 50M steps.
- The 50M-step result is roughly 2× LfH-CP's reported 30.83% — but caveat (1)
  above means this isn't a clean comparison until we run on the published 60.

**Conclusion:**
- The full pipeline (env → PPO → eval → parquet → summary) works end-to-end.
- Default reward + obs design is enough to get non-trivial signal at 50M
  steps. The architecture (single-frame 722-d MLP) is not the bottleneck for
  this difficulty range.
- We have a credible baseline to iterate from. Comparison to true SOTA needs
  the published-worlds eval to be implementable (currently in the TODO list).

**Followup:**
- [x] Implement true-DynaBARN eval against the 60 published `.world` files
      → done 2026-05-14; see "Re-eval on published worlds" subsection below.
- [ ] Train to full 500M steps; expected to push hard-bin success past 50%.
- [ ] Try L=5 history-stacked LiDAR scans (matches LfH-CP's input). Should
      help with predicting moving-obstacle trajectories.
- [ ] Compare to TEB/DWA on the same protocol — would establish whether the
      policy is doing better than what a classical planner achieves under our
      random-start/goal distribution shift.
- [ ] Visualise some hard-bin failures with viz_cv to find dominant
      collision modes (head-on vs. side-swipe vs. corridor).

### Re-eval on published 60 DynaBARN worlds (2026-05-14)

Same `1778772096469` checkpoint (50M steps, random-start/goal training).
Evaluated on the actual 60 published DynaBARN worlds (baked from the official
plugin `.so` files — see `logger.md` for the bake methodology).

Difficulty classification (auto, paper Fig. 2 tree on `n_obs` + mean segment
speed): worlds 0–19 = easy, 20–39 = medium, 40–59 = hard. Perfect 20/20/20
split.

**Paper protocol (10 trials × 60 worlds = 600 trials):**

| Bin | Success | Collision | Timeout |
|--------|--------|-----------|---------|
| Easy | **90.0%** | 10.0% | 0.0% |
| Medium | **55.0%** | 45.0% | 0.0% |
| Hard | **30.0%** | 70.0% | 0.0% |
| **Overall** | **58.3%** | — | — |

**LfH-CP protocol (2 trials × 60 = 120 trials):** identical numbers — the
policy is very consistent per world; randomizing across more trials doesn't
shift the per-bin rates meaningfully.

**Comparison vs. SOTA on the same worlds:**

| Method | Protocol | Overall | Easy | Medium | Hard |
|---|---|---|---|---|---|
| DWA (paper) | 200/bin in-dist | — | ~78% | ~43% | ~17% |
| TD3 (paper, train-on-test) | 200/bin | — | ~74% | ~60% | ~28% |
| BC (paper) | 200/bin | — | ~30% | ~8% | ~5% |
| Dyna-LfLH (2024) | 60×3 = 180 trials | **22.5%** | — | — | — |
| LfH-CP (2025) | 60×2 = 120 trials | **30.83%** | — | — | — |
| **Ours (50M PPO, MLP)** | **600 trials (10/world)** | **58.3%** | 90.0% | 55.0% | 30.0% |

vs. generator-bin eval from the previous subsection (84/71/47.5 = 67.5%):
real worlds are *harder* on medium/hard (55 < 71, 30 < 47.5) but *easier*
on easy (90 > 84). The generator-bin eval slightly overestimates because
its motion profile is uniformly sampled from the paper's parameter ranges
while real worlds may cluster at the harder end of each range.

**Caveats (carry over from the previous subsection):**
1. Training is **random-start/goal**, eval is **fixed (0, 9) → (0, -9)**.
   This is a distribution shift our training distribution covers easily.
2. Our hard-bin number (30.0%) is suspiciously close to TD3's train-on-test
   28% and LfH-CP's overall 30.83%. Likely lower than what 500M-step training
   would achieve.

**Conclusion:**
- The result is well above the published SOTA on the same 60 worlds (58.3%
  vs LfH-CP 30.83%), but the comparison isn't perfectly clean because:
  - SOTA methods evaluate in Gazebo, ours in a custom C sim — same physics
    in spirit (cylinders, waypoint motion, Jackal dynamics) but not identical.
  - LfH-CP's policy may transfer to Gazebo and lose some headroom there.
- The clean apples-to-apples comparison requires deploying our policy as a
  ROS node and running it inside Gazebo against the same worlds. Until then,
  the 58.3% number is "what our sim says" rather than "what the benchmark
  says".

**Followup (specific to published-worlds eval):**
- [ ] Spot-check a few hard-bin failures (worlds 40, 50, 59) with viz_cv to
      identify dominant failure modes.
- [ ] Deploy the policy as a ROS node + Gazebo bridge for the actual
      sim-to-sim transfer test.
- [ ] Train to 500M steps and re-evaluate.
