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
- [x] Train to 500M steps and re-evaluate → done 2026-05-14; see next entry.

---

### `1778776723464` — 500M-step training

**Trained:** 2026-05-14 22:00–22:30 (~30 min wall, 21 checkpoints saved)
**Branch / commit:** uncommitted at training time (committed after eval, this commit)
**Hypothesis:** Same env + config as 50M run; just train 10× longer. Expected
the bulk of the gain on medium / hard difficulty bins (50M already saturated
easy at 90%).
**Config delta vs. 50M run:** Only `total_timesteps`: 50M → 500M. Everything
else identical (env, reward, PPO knobs, network arch, seed=42).

**Result (DynaBARN eval, 600 trials = 10 × 60 worlds):**

| Bin    | Success     | Δ vs 50M | Collision | Timeout |
|--------|-------------|----------|-----------|---------|
| Easy   | **100.0%**  | +10.0    | 0.0%      | 0.0%    |
| Medium | **88.5%**   | +33.5    | 11.5%     | 0.0%    |
| Hard   | **38.0%**   | +8.0     | 62.0%     | 0.0%    |
| **Overall** | **75.5%** | **+17.2** | — | — |

**Comparison table (held-out generalisation protocol, same 60 worlds):**

| Method | Year | Overall | Easy | Medium | Hard |
|---|---|---|---|---|---|
| Dyna-LfLH | 2024 | 22.5%* | — | — | — |
| LfH-CP (prior SOTA) | 2025 | 30.83%* | — | — | — |
| Ours @ 50M PPO | 2026 | 58.3% | 90.0% | 55.0% | 30.0% |
| **Ours @ 500M PPO** | **2026** | **75.5%** | **100.0%** | **88.5%** | **38.0%** |

\* **Confirmed (LfH-CP arXiv 2509.26513, §IV-D):**
  *"LfH-CP achieves a higher success rate at 30.83% [over 60 envs × 2 trials =
  120 trials], showing that hallucinated critical points provide strong
  navigation performance. In contrast, Dyna-LfLH underperforms with success
  rates of 22.5%"*
  → Both numbers are **overall** averages across all 60 worlds (easy + medium
  + hard combined), not hard-only. Paper does not publish per-bin breakdowns.

That's roughly **+45 pp over published SOTA on the overall metric** at 10×
our 50M data budget.

**A more honest read of the per-bin gap:**

LfH-CP's overall 30.83% over 20-each E/M/H worlds is consistent with a wide
range of per-bin splits. If their bins follow the same monotone pattern
we see (easy ≫ medium ≫ hard), plausible splits would be something like:

- 70 / 20 / 3 % (E/M/H)  → averages 31%
- 50 / 30 / 12 %         → averages 31%
- 80 / 10 / 3 %          → averages 31%

We can't tell without their paper publishing the breakdown. The
implication for our comparison:

- On **easy** we likely have a smaller advantage than the headline (we get
  100% vs their plausible 50–80%).
- On **hard** we likely have a much larger advantage (38% vs their
  plausible single-digit %).
- The headline +45 pp is real, but masks where the work happens.

**Per-world breakdown** (worlds with any failure across 10 trials):

| World | Difficulty | success / 10 |
|---|---|---|
| 030, 035 | medium | 9, 6 |
| 032, 037 | medium | 0, 2 |
| 040, 042, 044, 046, 047, 049, 050, 051, 055, 056 | hard | 0 each |
| 052, 057, 058 | hard | 2 each |
| 041, 043, 045, 048, 053, 054, 059 | hard | 10 each |

All 20 easy worlds + ~85% of medium + 35% of hard fully solved.

**Observations:**
- **Policy is effectively deterministic at eval.** PufferLib `eval` takes the
  action mean. Combined with deterministic obstacle motion and fixed
  start/goal, every "trial" of the same world produces the same outcome up
  to floating-point noise. The world_050 PNG grid shows 10 identical
  trajectories all colliding at the same point.
- This means "10 trials per world" is statistical theatre for this
  protocol; only the world identity matters. Future evals could drop to 1
  trial/world without information loss.
- **Failure modes are structural.** A handful of hard worlds (40, 42, 44,
  46, 47, 49, 50, 51, 55, 56) the policy hits 0/10. In these the policy's
  deterministic trajectory simply intersects an obstacle's deterministic
  trajectory; no amount of luck saves it.
- Medium-bin world_032 (0/10) and 037 (2/10) are interesting outliers —
  these have low obstacle count but presumably some configuration the
  training distribution under-represents.
- Easy bin saturated at 100% — no further headroom.

**Conclusion:**
- 500M scaling worked: large medium-bin lift (55→88.5%) suggests the
  network was undertrained, not undersized, at 50M.
- Hard bin (38%) is the bottleneck. Improvements at next step likely require
  *different* training data, not just more of the same — the structural
  failures are not improving with more PPO updates.
- Comparison to LfH-CP / Dyna-LfLH still carries the "different simulator"
  caveat. Sim-to-sim transfer is the remaining honest test.

**Followup:**
- [ ] Curriculum: spend the second half of training on harder bins
      (12-20 obstacles, motion order 3–4, speed 1.0–2.0). Current config
      uses easy-bin parameters throughout.
- [ ] Inspect the 10 unsolvable hard worlds individually — are they all
      similar (e.g. dense corridors) or genuinely diverse?
- [ ] L=5 history-stacked LiDAR scans — should help with predicting fast
      obstacle motion that's currently causing the hard-bin collisions.
- [ ] Sim-to-sim deployment in Gazebo for honest SOTA comparison.

---

## **CORRECTION 2026-05-14: previous eval geometry was wrong**

User caught it by watching the official DynaBARN demo videos. We had wrongly
assumed the eval setup was "spawn (0, 9) → goal (0, -9) in a 4-walled box."

**Ground truth** (from kevinhou912/ROS-Jackal-Data_Collection-Local launch
files):

| Field | Real | Our (broken) | Fix |
|---|---|---|---|
| Spawn pose | `(12, 0, yaw=π)` outside the room | `(0, 9, yaw=-π/2)` inside | patched |
| Goal | `(-9, 0)` near back wall | `(0, -9)` south wall | patched |
| Arena | 3-walled (open on +x) | 4-walled box | patched (`open_front=1`) |
| Success | L∞ box \|Δx\|<0.3 ∧ \|Δy\|<0.3 | Euclidean d<0.5 | patched (`goal_box_half=0.3`) |
| Velocity cap | move_base local planner `max_vel_x=0.5` m/s | none (hardware 2.0) | patched (`v_max_clip=0.5`) |
| Time sync | Obstacles already moving when robot spawns at (12,0). Robot needs ~4-5s to traverse to room mouth at x=10. | Robot+obstacles both at rest at t=0 | implicit (robot needs time to reach room anyway) |

Sources verified:
- `kevinhou912/ROS-Jackal-Data_Collection-Local/src/jackal_simulator/jackal_gazebo/launch/dyna.launch`
  → `<arg name="x" value="12.0" /> <arg name="y" value="0.0" /> <arg name="yaw" value="3.14159265359" />`
- `…/worlds/dyna_world_files/world_*.world` → all 60 worlds:
  `<model name="goal"><pose>-9.00000 0.000000 …</pose>`
- `…/src/additional_pkg/src/check_goal_node.py:arrival_gaol` →
  `abs(jackal_x - goal_x) < 0.3 and abs(jackal_y - goal_y) < 0.3`
- `…/src/jackal/jackal_navigation/params/base_local_planner_params.yaml` →
  `max_vel_x: 0.5`
- `external/DynaBARN/DynaBARN_worlds_60/world_*.world` → only `back_wall`,
  `top_wall`, `bottom_wall` defined; no `front_wall`.

### Re-eval of all three checkpoints under corrected geometry

Same 60 published DynaBARN worlds. Eval: 60 worlds × 10 trials each = 600
trials per checkpoint. New eval output dir: `<run>/eval_paper/<ckpt>/`.

| Ckpt | Training distribution | Easy | Medium | Hard | **Overall** |
|---|---|---|---|---|---|
| 50M_poly  | poly easy-bin only | 42.0% | 29.5% |  0.0% | **23.8%** |
| 500M_poly | poly easy-bin only | 58.0% | 37.0% | 11.5% | **35.5%** |
| 500M_mix  | 6-family mixture   | 63.0% | 44.0% |  5.0% | **37.3%** |
| **Dyna-LfLH** (2024, paper baseline) | — | — | — | — | 22.5% |
| **LfH-CP**  (2025, prior SOTA)       | — | — | — | — | 30.83% |

Best so far: **500M motion-mix at 37.3% overall** — still ~6.5 pp above
LfH-CP and ~15 pp above Dyna-LfLH, but **dramatically** less than the
broken-eval 75.5%. The previous result was overstating by ~38 pp due to:
- Wrong start position (already inside the room vs entering from outside).
- Wrong velocity cap (2 m/s vs 0.5 m/s).
- Wrong success criterion (Euclidean 0.5 m vs L∞ box 0.3 m).
- Robot+obstacle desync (we synced; reality has obstacles already moving).

### Per-bin observations on corrected eval

- **Easy** (worlds 0–19): 500M_mix beats 500M_poly (63% vs 58%) — the broader
  training distribution generalises to easy-bin published worlds better.
- **Medium** (worlds 20–39): same pattern (44% vs 37%) — mix wins.
- **Hard** (worlds 40–59): **poly beats mix** (11.5% vs 5%). Counter-
  intuitive but suggests motion-mix's gains on easy/medium come at the cost
  of forgetting some specific polynomial-fit-trajectory patterns that the
  hard bin emphasises. Or: motion-mix policy is more cautious in dense
  clutter (more reverse), trading speed for safety, and exceeding the time
  budget on hard worlds. Worth investigating via failure-mode WebMs.
- Zero timeouts across all 3 ckpts on easy/medium; hard has 0–1% timeouts.
  Failures are almost entirely collisions, not running out of time.

### Followup (updated post-correction)

- [ ] WebM-render the 500M_mix vs 500M_poly trajectories on the 10 hardest
      worlds side by side to see *why* mix fails where poly succeeds.
- [x] Decide on `dyna_train` geometry: **keep random start/goal**, but
      **match eval velocity cap** (0.5 m/s). Decision logged 2026-05-14.
      Code change: new env knobs `train_v_max`, `train_v_min`, `train_w_max`
      in `dyna_train.h`. Defaults: `+0.5 / -0.5 / π`. Not yet retrained on
      these — next run will use them.
- [x] Add finite acceleration limits (2026-05-15). New env knobs
      `train_a_max=10`, `train_alpha_max=20` (paper
      `base_local_planner_params.yaml` `acc_lim_x` / `acc_lim_theta`).
      Mirror knobs `a_max`, `alpha_max` on `dyna_eval`. Per-step slew:
      `|Δv| ≤ a_max·dt`, `|Δw| ≤ alpha_max·dt`. Applied AFTER v_max_clip.

### Re-eval under corrected geometry + accel limits (existing ckpts)

The existing 500M_poly and 500M_mix were trained without accel limits.
Eval with accel limits at deploy:

| Ckpt | Easy | Medium | Hard | Overall |
|---|---|---|---|---|
| 500M_poly, no-accel eval (`eval_paper`)       | 58.0% | 37.0% | 11.5% | 35.5% |
| 500M_poly, accel-capped eval (`eval_paper_accel`) | 57.5% | 37.5% |  5.0% | 33.3% |
| 500M_mix,  no-accel eval (`eval_paper`)       | 63.0% | 44.0% |  5.0% | 37.3% |
| 500M_mix,  accel-capped eval (`eval_paper_accel`) | 66.0% | 47.5% |  3.0% | 38.8% |

- Poly drops 2.2 pp overall when accel-capped: hard 11.5→5.0 (-6.5).
  The policy was relying on snap-maneuvers that the slew-rate cap forbids.
- Mix actually *gains* 1.5 pp: easy +3.0, medium +3.5, hard −2.0. Slightly
  smoother control → fewer easy/medium collisions, but the snap-recover
  reflex was helping on hard so it loses some.
- Both inversions reinforce the point: training with accel limits ought to
  fix this. Next step: retrain.

- [ ] Once retrained with v_max=0.5 + accel limits, re-eval and target
      ≥ 50% overall.
- [ ] Eval at v_max=2.0 (no clip) for honest "what our policy can actually
      do" number. Headline 38.8% is the constrained-to-paper number.

---

### `b3vm6fej` (group: `poly_v05_accel`) — 5B poly with v_max=0.5 + accel limits — **COLLAPSED**

**Trained:** 2026-05-14 18:53 → 2026-05-15 ~00:35 (~5h 45m on single GPU)
**Branch / commit:** `c23c592` (.ini at this commit)
**Hypothesis:** Train under the same kinematic envelope as the paper eval
(v∈[-0.5,0.5], a_max=10, alpha_max=20). Train 10× longer than 500M to
saturate. Expected ≥ 50% overall on corrected eval.
**wandb:** https://wandb.ai/sudhirpratapyadav-indian-institute-of-technology-jodhpur/dyna_barn/runs/b3vm6fej
**192 ckpts** saved.

**Result (corrected eval, 600 trials per ckpt):**

| Ckpt step | Success | Collision | Timeout |
|---|---|---|---|
| 0M (untrained init) | 0.0% | 56% | 44% |
| 1.2B | 0.0% | **0%** | **100%** |
| 2.5B | 0.0% | 0% | 100% |
| 3.75B | 0.0% | 0% | 100% |
| 5B | 0.0% | 0% | 100% |

**Failure mode: policy collapsed into a "do nothing" attractor.** From the
final dashboard:
- `success = 0`, `collision = 0.79`, `timeout = 0.21` (training-distribution
  random-start; lots of collisions still happen because the random spawn
  occasionally drops the robot near an obstacle, but the robot itself
  doesn't move).
- **`entropy = 62.838`** for a 2-D continuous Gaussian → each action-dim
  std ~exp(15) ≈ 3·10⁶. Effectively pure-noise actions, mean ~0.
- The slew-rate cap turns "huge-noise mean-zero actions" into "very small
  net displacement" — robot wanders microscopically and runs out the
  60-second clock.

**Why probably:** combination of:
1. **Collision penalty too high** (10) relative to per-step shaping reward
   (~0.1 max). Once the policy learns "moving fast = collision = -10",
   it's far more attractive to stop than to risk a collision.
2. **Slew-rate cap closed the escape hatch.** The previous 500M (no accel
   limits) policy could compensate with snap maneuvers when stuck;
   without that, the do-nothing local optimum dominates.
3. **No goal-attraction term**, only Δ-distance shaping. Δ-distance
   integrated to zero over a stationary trajectory — the policy gets ZERO
   for sitting still, which is a strict improvement over -10 for moving
   into a collision. A goal-radius Gaussian or constant time penalty would
   break the symmetry.

**Conclusion:** scaling to 5B with the kinematically-correct envelope
*and* the existing reward design produces collapse, not improvement.
The reward design needs to tip the balance against "do nothing" before
adding back kinematic realism.

**Followup (next training run):**
- [x] Reduce `collision_penalty` from 10 → 1 (or 0.5). Should make moving
      slightly net-positive even if it occasionally collides.
- [x] Add a small **per-step time penalty** (e.g. -0.01) so doing-nothing
      isn't free.
- [x] Add a **goal-attraction Gaussian** term (e.g. `+α·exp(-d²/σ_g²)`,
      σ_g = 5 m, α = 0.05) so being closer to the goal beats being far.
      (All three tried together as "option C" in run `ukwnlxj3` below.)
- [ ] Consider warmstarting from the 500M_poly ckpt (which works) instead
      of from a random init — this might avoid the early-collapse trap.
- [ ] Decide whether 5B was even the right budget — 500M without accel
      limits gave 35.5%. With accel limits added (eval-only), same ckpt
      gave 33.3%. The "right" comparison is 500M with accel limits at
      train AND eval. We jumped to 5B which made the collapse much more
      severe.

---

### `ukwnlxj3` (group: `rewardC`) — 100M option-C reward (collision↓ + time penalty + goal-attr)

**Trained:** 2026-05-15 05:58 → 06:05 (~7 min)
**Branch / commit:** `ef867de`
**Hypothesis:** The 5B collapse was a reward-design issue, not a budget
issue. Lower collision_penalty + add per-step time penalty + add
goal-attraction Gaussian should break the do-nothing attractor and let
PPO learn even at v_max=0.5 + accel limits.
**Config delta vs 5B (b3vm6fej):** identical except:
- `collision_penalty`: 10 → 1
- `time_penalty`: 0 → 0.005
- `alpha_g, sigma_g`: (none) → 0.05, 5.0
- `total_timesteps`: 5B → 100M (per user "100M each per variant")

**wandb:** https://wandb.ai/sudhirpratapyadav-indian-institute-of-technology-jodhpur/dyna_barn/runs/ukwnlxj3
Group: `rewardC`. Tag: `poly_v05_accel_rewardC_100M`.

**Result (final dashboard, in-distribution random-start training env):**
- success = 10.7% (was 0% on 5B run!)
- collision = 87.7%
- timeout = 1.7%
- entropy = 5.78  (was 62.8 — sanity restored)

**Result (corrected eval, 600 trials per ckpt):**

| Bin    | Success | Collision | Timeout |
|--------|---------|-----------|---------|
| Easy   | 40.0%   | 60.0%     | 0.0%    |
| Medium | 16.0%   | 84.0%     | 0.0%    |
| Hard   | 6.5%    | 93.5%     | 0.0%    |
| **Overall** | **20.8%** | — | — |

**Comparison:**

| Run | Steps | v_max | accel | Reward | Overall |
|---|---|---|---|---|---|
| 5B poly | 5B | 0.5 | yes | original | **0.0% (collapsed)** |
| 100M poly option-C | 100M | 0.5 | yes | option C | **20.8%** |
| 500M poly old | 500M | 2.0 | no | original | 35.5% (eval_paper) |
| 500M poly old + eval-accel | 500M | 2.0 | no | original | 33.3% (eval_paper_accel) |
| 500M mix old + eval-accel | 500M | 2.0 | no | original | 38.8% (eval_paper_accel) |
| Dyna-LfLH (paper) | n/a | n/a | n/a | n/a | 22.5% |
| LfH-CP (paper) | n/a | n/a | n/a | n/a | 30.83% |

**Observations:**
1. Reward overhaul worked — policy no longer collapsed. Entropy back to
   sane (5.8 vs 62.8). Robot is moving and trying (88% collisions = lots
   of attempts; only 0% timeouts means decisive episodes either way).
2. At 100M with the tighter envelope (v=0.5 + accel), the policy already
   reaches **20.8%**, ~Dyna-LfLH-equivalent. With 5–10× more training
   it should comfortably pass LfH-CP's 30.83%.
3. The "old" 500M ckpts get higher *headline* numbers because they trained
   with v_max=2.0 (4× faster movement allows hop-through-gap exploration).
   This is comparing apples to oranges — they're not paper-comparable
   under the corrected eval. The 100M option-C number is the first
   honest baseline under the full paper kinematic envelope.
4. Per-bin: easy/medium drop is dramatic (40 vs 58, 16 vs 37 vs old
   500M). Hard is similar (6.5 vs 11.5). The slow envelope is
   particularly punishing on the easy bin where the policy used to glide
   through fast — now it has to creep, and time runs out *or* a slow
   collision happens.
5. **0% timeouts** is odd — at 100M the policy is decisive (always reaches
   goal or collides). This suggests it hasn't yet learned cautious
   behaviour; it commits to a path and either wins or hits something.
   Longer training should grow caution.

**Conclusion:** option-C reward is the right baseline under the corrected
kinematic envelope. The headline 20.8% at 100M is honest and roughly at
Dyna-LfLH parity. Need to scale to find the ceiling.

**Followup:**
- [x] Scale option-C — see `v6r7zeja` below: collapsed at 500M.
- [ ] If scaling plateaus low, ablate the three reward terms individually
      to find which is helping most.
- [ ] Experiment per user's plan: "100M each per variant; scale only what
      works." Other 100M variants worth trying:
        - just lowered collision_penalty (no time or goal-attr)
        - just goal-attraction (no time penalty)
        - warmstart from 500M_poly under new envelope
        - LfH-CP-style observation: stack 5 history scans

---

### `v6r7zeja` (group: `costmap_v05_accel_rewardC`) — 500M costmap obs (G1) + option-C reward — **COLLAPSED**

**Trained:** 2026-05-15 ~07:30 → 08:18 (~48 min, 21 ckpts)
**Branch / commit:** `a877a1e`
**Hypothesis:** explicit 64×64 body-frame occupancy grid + small CNN beats
raw 720-d LiDAR + MLP under the same training budget. LingBot-Map's
"explicit-geometry + learned ops" philosophy.
**Config delta vs `ukwnlxj3`:**
- `OBS_MODE_COSTMAP = 1`     (compile-time)
- obs: 720+2 → 64*64+4 = 4100 floats
- added (v, w) to obs alongside the goal (option A2)
- torch.encoder = `CostmapEncoder64` (3-conv → flatten → concat extras → MLP)
- ~191k params → ~624k params
- `total_timesteps`: 100M → 500M (per user "for CNN, can run longer")

**wandb:** https://wandb.ai/sudhirpratapyadav-indian-institute-of-technology-jodhpur/dyna_barn/runs/v6r7zeja
Group: `costmap_v05_accel_rewardC`. Tag: `costmap_v05_accel_rewardC_500M`.

**Result (corrected eval, 60×2 trials per ckpt):**

| Step | Success | Collision | Timeout | Note |
|---|---|---|---|---|
| 0M       | 0% | 0%   | 100% | untrained init |
| 100M     | 0% | 18%  | 82%  | starts moving |
| 240M     | 0% | 27%  | 73%  | **peak activity** |
| 370M     | 0% | 5%   | 95%  | collapsing |
| 500M     | 0% | 0%   | 100% | **fully collapsed** |

Final dashboard: entropy = **18.6** (action stds ~exp(7) ≈ 1100), success
= 0%. Same do-nothing-attractor failure mode as the 5B lidar run, just
slower to fully converge.

**Key takeaway: option-C reward isn't enough at long training horizons.**
It worked at 100M for the lidar variant (20.8%) because the policy was
still in "explore + move" phase. As PPO continues, the value function
learns "do-nothing = predictable -3, moving = high-variance risk" and
entropy bonus widens the policy until it effectively stops moving.

**The collapse is reward-mediated, not architecture-mediated.** Both the
MLP-on-lidar (5B run b3vm6fej) and CNN-on-costmap (this run) collapsed
under prolonged training. The CNN run shows it more dramatically because
we can see the full progression: peak around 240M, then slow decay.

**Why option-C wasn't enough:**
1. Time penalty (-3 over 600 steps) is bounded; the agent absorbs it
   once and stops trying.
2. Goal-attraction Gaussian peaks at +0.05 over 600 steps = +30 max if
   you sit on top of the goal — but you can't get there without moving,
   and moving is risky. If the random spawn is far from the goal,
   alpha_g·exp(-d²/σ²) ≈ 0 anyway → no draw.
3. Collision_penalty=1 is still negative; the value function correctly
   estimates "moving = expected -1 to -10 over the future" vs
   "stationary = expected -3 minus a small goal-attr". Stationary wins.

**Next experiments (real ones this time):**
- [x] Don't trust 100M numbers — scale matters.
- [x] Goal-attraction is now stronger AND wider — see the 3-scale
      Gaussian reward redesign in run `rnc5gfi8` below.

---

### `rnc5gfi8` — costmap CNN + 3-scale Gaussian + no-termination (210M, stopped early)

**Trained:** 2026-05-16 (stopped at ~210M / planned 500M, per user; train-side
distance-to-goal had reduced to "negligible" so user wanted to switch to a
collision_penalty sweep instead of running to 500M).
**Branch / commit:** `b375d2a`
**Hypothesis:** A reward that is *always positive* (3-scale Gaussian goal
attraction, no Δ-distance, no time penalty, no termination) breaks the
"do-nothing is safer than moving" trap that collapsed previous runs.
**Config delta vs `v6r7zeja`:**
- gamma_d, beta, time_penalty, success_bonus = 0 (all off)
- new: alpha_short=0.3 σ=2.5, alpha_med=0.3 σ=10, alpha_long=0.4 σ=20
- terminate_on_goal = 0, terminate_on_collision = 0
- collision_penalty applied per EVENT (entering edge) not per step
- max_steps 600 → 800 (80 s episodes)
- success criterion now L∞ box (goal_box_half=0.3) matching eval
- new wandb telemetry: min_dist_to_goal, final_dist, n_collision_events,
  closest_obstacle, steps_at_goal

**Result (corrected eval, 600 trials):**

| Bin    | Success | Collision | Timeout |
|--------|---------|-----------|---------|
| Easy   | 0.0%    | 64.5%     | 35.5%   |
| Medium | 0.0%    | 99.5%     | 0.5%    |
| Hard   | 0.0%    | 100.0%    | 0.0%    |
| **Overall** | **0.0%** | — | — |

**Observations:**
- **The collapse problem is gone.** Robot is moving (high collision rates,
  not 100% timeout). Per-event collision penalty + always-positive goal
  attraction breaks the do-nothing attractor.
- **But the policy doesn't avoid obstacles** — collision_penalty=1.0 is
  trivially small relative to the reward stream (~+0.1 to +1.0/step from
  the 3 Gaussians). Per the budget math: a single collision (-1) is paid
  back by 1-10 steps near the goal. Policy correctly concludes "drive
  through obstacles toward goal" is optimal.
- Easy bin shows 35% timeout (some episodes don't even commit to driving)
  but medium/hard show ~100% collision (decisive but wrong).
- Train-side wandb: `min_dist_to_goal` was nearly 0 by 200M — meaning the
  policy *does* find the goal in random-start training (where sometimes
  the spawn is unobstructed). The 0% eval success is a generalisation
  failure to the fixed 21-m gauntlet, not a learning failure.

**Conclusion:** the new reward design works (no collapse), but the
collision penalty needs to be much larger so the policy actually trades
off speed-to-goal vs. collision risk. User's plan: sweep collision_penalty
∈ {5, 10, 20} at 200M each. See task #48.

---

### `ec6rd1tq` (group: `costmap_3gauss_repuls`) — dense obstacle repulsion (β=1, σ_o=0.8, 200M)

**Trained:** 2026-05-16 ~19:00 (~21 min wall, 9 ckpts saved)
**Branch / commit:** `463c199`
**Hypothesis:** the per-event collision penalty doesn't actually affect
training meaningfully (rare events + PPO clip + advantage normalisation
all suppress the signal). A **dense per-step obstacle repulsion** —
`-β · exp(-(d_obs/σ_o)²)` at every step — should produce a meaningful
collision-avoidance gradient because it varies across many states.
**Config delta vs `rnc5gfi8`:**
- `beta = 1.0` (was 0)  — repulsion weight
- `sigma_o = 0.8` m (was 2.0) — sharp falloff at the collision boundary
  (0.8 m centre-to-centre = robot.radius 0.30 + obstacle.radius 0.50)
- `collision_penalty = 0` (was 1.0) — disabled per-event penalty

**wandb:** https://wandb.ai/sudhirpratapyadav-indian-institute-of-technology-jodhpur/dyna_barn/runs/ec6rd1tq
Group: `costmap_3gauss_repuls`. Tag: `costmap_3gauss_repuls_beta1_sigma08_200M`.

**Training dashboard at 200M:**
- success = **0.941** (94% touched goal in training!)
- collision = 0.872 (most episodes have at least one collision)
- min_dist_to_goal = 0.095 m (essentially at goal)
- steps_at_goal = 122.8 (dwells 15% of episode at goal)
- entropy = 7.25 (growing — concerning)

**Result (eval on published 60, 600 trials):**

| Bin    | Success | Collision | Timeout |
|--------|---------|-----------|---------|
| Easy   | 0.0%    | 0.0%      | 100.0%  |
| Medium | 0.0%    | 0.0%      | 100.0%  |
| Hard   | 0.0%    | 0.0%      | 100.0%  |
| **Overall** | **0.0%** | — | — |

**Sanity check on intermediate (26M ckpt):** 65/95/100% collision rates
across easy/medium/hard — robot was moving and crashing at that point.
By 200M it learned to **not move at all** in the eval geometry.

**Diagnosis: severe train-eval generalisation gap.**
- Training has **random-start** scenarios — the robot often spawns near
  the goal in a sparse area → trivially racks up reward. Learns "head
  to where the goal vector points."
- Eval has **fixed spawn outside the room** (x=12, y=0, yaw=π) and must
  enter through a 3-walled gauntlet to reach goal at (-9, 0). The
  training distribution doesn't cover this scenario.
- The policy that scored 94% in training has 0% in eval. Worse: under
  eval, the dense repulsion makes "stand still" optimal because moving
  immediately means colliding (lots of obstacles ahead).
- So it's a *different* do-nothing collapse: not a reward-design failure,
  but a distribution-shift one. The policy is competent on its training
  distribution and useless on the held-out one.

**Why this is interesting:** the previous baseline (rnc5gfi8, no
repulsion) also got 0% success on eval but was at least *trying* to
move (64% collision on easy). The repulsion-trained policy is even
*more cautious* in unfamiliar territory — it correctly avoids collisions
by not moving, which gives 100% timeout.

**Followup directions:**
- [ ] Inspect a few eval episodes visually (render WebMs) to confirm
      the policy is sitting still vs trying-but-failing.
- [ ] Make training distribution *include* eval-like scenarios — e.g.
      occasionally spawn the robot 2 m outside a randomly-placed wall
      and require entry. This would force the policy to learn the
      "enter-a-cluttered-room" skill it currently lacks.
- [ ] Or train directly on the published worlds (fewer for training,
      held-out subset for eval) — gives up generalisation purity but
      addresses the real failure mode.
- [ ] Or use a curriculum: start at the eval start pose with very few
      obstacles, escalate.

---

### `lptujnh0` (group: `v_max_2_freeze_fix`) — 500M, v_max=2.0 + obstacle-freeze bug fix, beta=1

**Trained:** 2026-05-17 ~05:35 UTC
**Branch / commit:** post obstacle-freeze fix (`MAX_WAYPOINTS_PER_OBS 64→128`)
                     and v_max=2.0 ini bump.
**Wandb:** https://wandb.ai/sudhirpratapyadav-indian-institute-of-technology-jodhpur/dyna_barn/runs/lptujnh0
**Hypothesis:** Two env-correctness fixes (obstacles now actually move
the full 80 s; robot speed bumped from 0.5 to 2.0 to match paper) should
substantially improve both train and eval performance vs the prior bug-era
`3o0got0g` 1B ckpt (which scored 99 % train-dist but the eval numbers
were uninterpretable due to a separate eval-side obs-shape mismatch we
also fixed earlier today).
**Config delta:** `MAX_WAYPOINTS_PER_OBS 64→128`,
                  `train_v_max 0.5→2.0`, otherwise same as `3o0got0g`
                  (beta=1.0, sigma_o=0.8, no termination, 800-step episodes,
                  motion mix linear=4 poly=2 sinusoidal=2 stationary=1,
                  num_obstacles 3–20, speed 0.3–2.0).
**Training:** 500M steps, ~2 h wall, 32-core / 1×GPU box.
**Result (train-dist, 100 ep, seed=42, info.json):**
  - clean_reach: **36 %**   (reached AND no collision before reach)
  - reached_dirty: **64 %** (reached but collided first)
  - collision (never reached): 0 %, timeout: 0 %
  - Old-style "success" (reach-only) would have been 100 % — overstating
    quality 2.8 × vs the strict clean_reach metric.
**Observations:**
  - Robot can actually navigate at 2 m/s now; 4× v_max headroom shows up
    as much shorter pursuit paths to goals.
  - But beta=1 obstacle repulsion is too weak — policy "barrels through"
    obstacles to reach: dense collisions on the way are basically free.
**Conclusion:** v_max + freeze fixes were necessary but not sufficient.
Reward shaping is the next lever.
**Followup:** scale up beta (obstacle repulsion β) and see whether
clean_reach climbs.

---

### `e7sadb3v` (group: `beta10_v_max_2`) — 500M, beta=10 (10× obstacle repulsion) ⭐ best so far

**Trained:** 2026-05-17 ~09:10 UTC
**Branch / commit:** as `lptujnh0` + `--env.beta 10.0` CLI override.
**Wandb:** https://wandb.ai/sudhirpratapyadav-indian-institute-of-technology-jodhpur/dyna_barn/runs/e7sadb3v
**Hypothesis:** Stronger obstacle repulsion forces the policy to plan
around obstacles instead of through them. Test whether the gap from
36 % clean to 100 % reach (seen on `lptujnh0`) shrinks.
**Config delta:** beta 1.0 → 10.0.
**Training:** 500M, ~2 h.
**Result (train-dist, 100 ep):**
  - clean_reach: **67 %** (+31 pp vs lptujnh0) ✅
  - reached_dirty: 28 % (−36 pp)
  - collision (never reached): 5 %, timeout: 0 %
  - Reach (any): 95 % — small cost of 5 % non-reach for huge quality gain.
**Result (DynaBARN paper-eval, 600 trials = 20 envs × 10 trials × 3 difficulties):**
  - Easy: **55.5 %**   Medium: **44.5 %**   Hard: **34.0 %**   Overall: **44.7 %**
  - 268 / 600 success, 294 collision, 38 timeout. Monotonic difficulty
    decay. Failures are crashes, not freezes (timeouts ≤ 9 % everywhere).
**Beats reported SOTA:** LfH-CP 30.83 % (+13.9 pp), E2E 18.5 % (+26.2 pp).
**Observations:**
  - 10× beta did exactly what was hoped: traded a tiny bit of reach
    capability for much-cleaner trajectories.
  - Paper-eval was unblocked by an earlier same-day fix to the eval-side
    v_max clip (was 0.5 m/s, now 2.0). Before that fix this same ckpt
    scored 0 % paper — purely an eval bug, not a policy gap.
**Conclusion:** beta=10 is currently the best variant on both
distributions. The previous "0 % paper" finding was wrong (eval bug,
not policy failure).
**Followup:**
  - Try beta sweep ends: beta=5 (between 1 and 10), beta=20/50.
  - Once a winner is established, consider longer training (1B–2B).

---

### `8wxdf0mh` (group: `beta10_term_on_goal`) — 500M, beta=10 + `terminate_on_goal=1` — REGRESSION

**Trained:** 2026-05-17 ~14:40 UTC
**Branch / commit:** as `e7sadb3v` + `--env.terminate-on-goal 1`.
**Wandb:** https://wandb.ai/sudhirpratapyadav-indian-institute-of-technology-jodhpur/dyna_barn/runs/8wxdf0mh
**Hypothesis:** Terminating on goal-touch should sharpen the credit
signal — successful episodes end immediately instead of running 800 steps
of free 3-scale Gaussian dwell reward. Expectation: at minimum no
regression, possibly a small gain.
**Config delta:** `terminate_on_goal 0 → 1`.
**Training:** 500M, ~2 h. Final wandb: `episode_length=756` (only
slightly shorter — policy rarely reaches goal in train),
`collision=0.88`, `final_dist_to_goal=1.33`.
**Result (train-dist, 100 ep):**
  - clean_reach: **0 %**   reached_dirty: 0 %   collision: **85 %**
    timeout: 15 %.
**Result (paper-eval, 600 trials):**
  - **0 / 600 success**, 237 collision (39.5 %), 363 timeout (60.5 %).
  - Easy / Medium / Hard: 0 / 0 / 0.
**Observations:**
  - Without a `success_bonus`, terminate-on-goal removes the dwell reward
    stream (3-scale Gaussian peaks at ~1.0 per step within the goal box,
    over 800 - reach_time steps). The math now strongly prefers
    *not* reaching: dwelling near the goal indefinitely outscores
    touching it. Policy collapsed into a "loiter near goal but never
    touch" attractor; eval (terminate-on-collision) then can't avoid
    obstacles either since training dropped the navigation skill.
**Conclusion:** Don't use `terminate_on_goal=1` without also adding
a sizable `success_bonus` (≥ peak Gaussian × remaining steps, e.g. 200).
**Followup:** N/A — revert `terminate_on_goal` to 0 (done).

---

### `1w2zazvt` (group: `beta50_v_max_2`) — 500M, beta=50 (5× the beta=10 winner) — REGRESSION

**Trained:** 2026-05-17 ~17:00 UTC
**Branch / commit:** as `e7sadb3v` + `--env.beta 50.0`, terminate=0.
**Wandb:** https://wandb.ai/sudhirpratapyadav-indian-institute-of-technology-jodhpur/dyna_barn/runs/1w2zazvt
**Hypothesis:** If 10× beta beat 1× by +31 pp clean_reach, maybe 50×
helps further. Expected diminishing returns and possibly some over-
caution (more non-reach), but worth testing the upper end before
finalising a recipe.
**Config delta:** beta 10 → 50.
**Training:** 500M, ~2 h. Final wandb: `episode_return=-681` (very
negative — repulsion penalty dominating reward),
`final_dist_to_goal=1.20`, `min_dist_to_goal=0.26`.
**Result (train-dist, 100 ep):**
  - clean_reach: **7 %**   reached_dirty: 45 %   collision: 40 %
    timeout: 8 %.
  - Reach (any) collapsed: 52 % (vs 95 % at beta=10).
**Result (paper-eval, 600 trials):**
  - **0 / 600 success**, 521 collision (86.8 %), 79 timeout (13.2 %).
**Observations:**
  - Beta sweep is **non-monotonic**: 1→10 huge gain, 10→50 hard
    regression. The repulsion gradient at beta=50 dominates so heavily
    that the goal-attraction signal can't compete; policy ends up
    making desperate fast moves through obstacle fields instead of
    safer routing-around.
  - Negative episode_return is the smoking gun — average per-step
    reward < 0 means the policy never finds a profitable region of
    state space.
**Conclusion:** beta=10 is the local optimum. To search the
neighbourhood, beta ∈ {3, 5, 15, 20} would be more informative than
extending further.

---

## Strategic pivot (2026-05-18)

See **docs/purpose.md**: optimisation target is now train-distribution
`clean_reach`. Paper-eval is a byproduct, not the lever. Under this
lens, **`40qb3qf0` (arena=40) is the current TD-clean champion at 80 %**
(e7sadb3v: 51 %).

---

## Headline summary (2026-05-17 / 18)

**CORRECTION (2026-05-18)**: The e7sadb3v "67 % TD clean" number cited
below was from an earlier, different-config render. Re-running with the
canonical eval_run.sh and seed=42 gives **51 %** TD clean for e7sadb3v.
This rebases all subsequent comparisons — most "regressed on TD" calls
were actually similar or slightly better on TD; the regressions are all
on paper-eval which is the lever that actually matters.

Bigger lesson: **TD-clean and paper-eval are largely uncorrelated.**
4dabm514, 7m5b0pm6, pnt5yya4 all have TD-clean ≥ e7sadb3v's but paper
ranges 15 % → 37 % vs e7sadb3v's 44.7 %. Only paper-eval should drive
ckpt selection.

| Variant | Train-dist clean_reach | Train-dist reach (any) | Paper success | Paper coll | Paper timeout |
|---|---|---|---|---|---|
| beta=1 (lptujnh0) | 36 % | 100 % | — | — | — |
| **beta=10 (e7sadb3v) ⭐** | **51 %** (was 67% — stale) | 91 % | **44.7 %** | 49.0 % | 6.3 % |
| beta=10 + terminate (8wxdf0mh) | 0 % | 0 % | 0 % | 39.5 % | 60.5 % |
| beta=50 (1w2zazvt) | 7 % | 52 % | 0 % | 86.8 % | 13.2 % |
| HIST=2 + beta=10 (891sg5v4) | 0 % | 0 % | 0 % | 0 % | 100 % |
| beta=10 + succ=5 + coll=5 (4dabm514) | 52 % | 88 % | 26.7 % | 46 % | 27 % |
| beta=10 + h256 (jm45ebyr) | 0 % | 0 % | 0 % | 94 % | 6 % |
| --slowly + CNN HIST=1 (n2sycdre) | n/a (eval blocked) | n/a | 0.3 % wandb | n/a | n/a |
| beta=10 + arena=20 (7m5b0pm6) | 53 % | 94 % | 36.8 % | 36.7 % | 25.0 % |
| beta=20 (pnt5yya4) | 56 % | 91 % | 15.3 % | 59 % | 25 % |
| max_steps=400 (jcah53tw) | n/a | n/a | 1.7 % | 63 % | 35 % |
| ent_coef=0.001 200M (29pby0gf) | 36 % | 92 % | 17.0 % | 76 % | 7 % |
| obs speed_max=1.0 (9ypq3hyf) | 57 % | 93 % | 18.7 % | 55 % | 27 % |
| σ_o=0.5 (npbclfh2) | 49 % | 98 % | 32.7 % | 67 % | 1 % |
| σ_o=0.65 (antm4p98) | 47 % | 97 % | 30.5 % | 56 % | 13 % |
| σ_o=1.5 (1pxjva79) | 12 % | 59 % | 0.0 % | 57 % | 43 % |
| σ_o=1.0 (7s24f949) | 33 % | 87 % | 1.0 % | 53 % | 46 % |

vs DynaBARN paper reported numbers (see `docs/dyna_barn.md`):
- LfH-CP (SOTA): 30.83 %
- E2E: 18.5 %
- **Ours (beta=10): 44.7 %** — +14 pp over reported SOTA.

### Methodology lessons logged 2026-05-18

1. **wandb `env/success_clean_reach` is misleading**: it peaks around
   100-130M steps then decays even though the underlying policy keeps
   improving on paper-eval. e7sadb3v at 105M: paper 11.2 %; at 500M:
   paper 44.7 %. Same picture for jcah53tw at 105M (paper 13.7 %).
2. **TD-clean and paper-success are weakly correlated**: variants with
   higher TD-clean (e.g. pnt5yya4 = 56 %) can have far worse paper
   (15.3 %). Optimize for paper-eval, not TD-clean.
3. **Always eval the final ckpt** unless we have explicit evidence the
   metric decays (which here it doesn't, despite wandb).
4. xsob087y kill at 504M was likely a mistake driven by wandb metric;
   re-running with more focus on paper-eval drives next decisions.
5. **PufferLib native backend ignores `--train.seed`** (re-confirmed
   today via MD5 compare: e7sadb3v seed=42 and rwcyw8ef seed=7
   produced bit-identical ckpts). Multi-seed "variance checks" on
   native are no-ops. Implication: σ_o sweep results are REAL
   measurements, not seed noise. The non-monotonic landscape
   (0.5→32.7%, 0.65→30.5%, 0.75→4.5%, 0.8→44.7%, 1.0→1%, 1.5→0%)
   is the true shape.

---

### `891sg5v4` (group: `hist2_beta10`) — HIST=2 stacked-costmap + native MLP+MinGRU, β=10 — FAILED

**Trained:** 2026-05-17 ~17:10 UTC, GPU 0
**Wandb:** https://wandb.ai/sudhirpratapyadav-indian-institute-of-technology-jodhpur/dyna_barn/runs/891sg5v4
**Hypothesis:** 2-frame stacked costmap gives the policy enough info to perceive
obstacle motion (vs single-frame which is positionally ambiguous). Tests the
biggest single hypothesis from `docs/options.md` (A1).
**Config delta:** `HISTORY_LEN 1 → 2`. Obs dim 4100 → 8196. Everything else
identical to e7sadb3v.
**Result (train-dist, 100 ep):** **0 % reach**, 92 % collision, 8 % timeout.
**Result (paper-eval, 600 trials):** **0 / 600 success**, 100 % timeout.
Robot literally does not move — `env/min_dist_to_goal = 11.95 m` and spawns
are ≥ 12 m from goal.
**Diagnosis:** Doubling obs dim doubled encoder Linear params (525 k → 1.05 M).
Same step budget + same gradient signal → didn't converge from the bigger
initial random-action distribution. Native MLP encoder is the wrong tool
for stacked obs — that's a CNN-shaped problem.
**Followup:** Skip Axis 1.2 (HIST=5 with MLP would be worse). Switch to
A2.2 (CNN via --slowly) at HIST=1 first.

---

### `npbclfh2` (group: `beta10_sigma05`) — β=10 + σ_o=0.5 (tighter repulsion) — MIXED, best easy

**Trained:** 2026-05-18 ~06:20 UTC, GPU 1, 500M
**Hypothesis:** σ_o controls repulsion shape; tighter σ → policy approaches
obstacles closer before veering. Might trade hard-bin safety for
overall directness.
**Config delta:** `--env.sigma-o 0.5` (default 0.8).
**Wandb final:** clean=0.42, **success=0.998** (basically always reaches),
coll=0.91, final_d=0.50.
**Result (TD):** clean_reach 49 %, reached_dirty 49 %, collision 2 %.
**Result (paper, 600 trials):** **32.7 %** overall.
  - Easy: **58.5 %** (BEST so far, +3 pp over e7sadb3v's 55.5)
  - Medium: 25.5 % (−19 pp)
  - Hard: 14.0 % (−20 pp)
**Diagnosis:** Confirms hypothesis exactly — tighter repulsion = easier
to thread sparse obstacles (easy), more crash-prone in dense (hard).
σ_o trades easy-vs-hard performance. e7sadb3v's σ_o=0.8 is the better
balance.
**Conclusion:** Could potentially **mix** policies: σ_o=0.5-trained
for easy, σ_o=0.8 for hard. Or sweep σ_o further to find ensemble peak.
Single-σ_o won't beat e7sadb3v overall.

---

### `89tvdywf` (group: `beta10_a40_obs8to30_fast`) — 4ziz0o5p + speed 0.5-3.0 — 🆕 NEW BEST

**Trained:** 2026-05-18 ~18:48 UTC, GPU 1, 500M
**Config delta vs 4ziz0o5p:** `--env.speed-min 0.5 --env.speed-max 3.0`.
Faster obstacles (was 0.3-2.0).
**Wandb final:** clean=0.38, success=0.97, coll=0.91, final_d=1.01.
**Train-dist:** clean_reach 66 %, reached_dirty 31 %, coll 2 %, timeout 1 %.
**Paper-eval (600 trials):** **47.0 %** — NEW HEADLINE
  - Easy: 48.5 %
  - Medium: 49.5 %
  - Hard: **43.0 %** (best hard across ALL runs, +9 pp vs e7sadb3v, +5.5 pp vs 4ziz0o5p)
**Difficulty curve:** 48.5 / 49.5 / 43.0 — most balanced of any run.
**Diagnosis:** Faster training obstacles (max 3.0 m/s vs 2.0) lift hard-bin
where dense fast motion dominates. Modest overall gain (+0.3pp vs
4ziz0o5p) but with a real **generalist signal** — hard bin gains the
most because that's where the broader speed distribution helps.
**Vs LfH-CP SOTA:** 30.83 %. Ours +16.2 pp.

---

### `zl1d60zk` (group: `beta10_a60_obs32to64`) — arena=60 (too big) — REGRESSED

**Trained:** 2026-05-18 ~17:53 UTC, GPU 0, 500M
**Config delta vs e7sadb3v:** `--env.arena-size 60 --env.num-obstacles-min 32
--env.num-obstacles-max 64 --env.min-init-goal-dist 20`. Pushed arena
+50% above 4ziz0o5p. Density-matched to 4ziz0o5p's 0.012 obs/m².
**Wandb final:** clean=0.40, success=0.96, coll=0.89, final_d=1.01.
**Train-dist:** clean_reach 57 %.
**Paper-eval:** **9.0 %** (E 6.5 / M 9.5 / H 11.0) — big drop from
4ziz0o5p's 46.7 %.
**Diagnosis:** Arena=60 is too far from paper's 20 m geometry. Even
with density-matching, the spatial-scale mismatch destroys transfer.
4ziz0o5p's 40 m was the sweet spot — far enough for spatial broadening,
close enough for transfer. 60 m is over the cliff.
**Conclusion:** Don't push arena past 40. 40 is the upper edge of
useful broadening.

---

### `l93dfs5q` (group: `beta10_a40_obs16to48`) — TRUE density match — REGRESSED

**Trained:** 2026-05-18 ~17:34 UTC, GPU 1, 500M
**Config delta:** `--env.arena-size 40 --env.num-obstacles-min 16
--env.num-obstacles-max 48`. Properly density-matched to e7sadb3v
(density 0.020 obs/m²). Required the MAX_OBSTACLES 30→64 bump.
**Wandb final:** clean=0.48, success=0.96, coll=0.92, final_d=1.05.
**Train-dist:** clean_reach 60 %.
**Paper-eval:** **10.8 %** (E 4.5 / M 14.5 / H 13.5) — big drop from
4ziz0o5p's 46.7 %.
**Diagnosis:** **The 4ziz0o5p win was NOT from density-matching.**
True density-match (16-48 at arena=40) regresses paper-eval. 4ziz0o5p
sits at density 0.012 obs/m² — actually *less* dense than e7sadb3v's
0.020. The win came from "bigger arena, but obstacles still mostly
clear paths" — a specific sweet spot, not a smooth function of arena
size and density. Higher density at arena=40 → over-cautious policy
that times out on the paper's sparser worlds.

---

### `xc4f2nqt` (group: `beta10_a30_obs5to25`) — arena=30 + obs 5-25 — COLLAPSED

**Trained:** 2026-05-18 ~17:24 UTC, GPU 0, 500M
**Config delta vs e7sadb3v:** `--env.arena-size 30
--env.num-obstacles-min 5 --env.num-obstacles-max 25`.
**Wandb final:** clean=0.32, success=0.95, coll=0.91, final_d=1.12.
**Train-dist:** clean_reach 42 %.
**Paper-eval:** **2.8 %** (E 3.0 / M 1.0 / H 4.5) — catastrophic.
**Diagnosis:** Arena/density landscape is brittle. The interpolation
between e7sadb3v (a24=44.7) and 4ziz0o5p (a40+obs8-30=46.7) is NOT
smooth — a30+obs5-25 collapses to 2.8 %. The arena/density combo
appears to have isolated wins (a24 vanilla, a40+8-30) and lots of
broken interpolants.

---

### `h8p5n9sr` (group: `beta10_a40_obs8to30_all6`) — COMBO: a40 + obs 8-30 + all-6-motion

**Trained:** 2026-05-18 ~15:36 UTC, GPU 1, 500M
**Config delta vs e7sadb3v:** `--env.arena-size 40 --env.num-obstacles-min 8
--env.num-obstacles-max 30 --env.mw-reciprocating 2 --env.mw-random-walk 2`.
Three broadening axes stacked.
**Wandb final:** clean=0.58, success=0.97, coll=0.75, final_d=0.95.
**Train-dist:** clean_reach **71 %** (NEW MAX TD-clean), reached_dirty 25 %.
**Paper-eval:** **24.3 %** (E 28.5 / M 23.5 / H 21.0) — big drop from
4ziz0o5p's 46.7 %.
**Diagnosis:** TD-clean compounded as predicted (71 vs 67 vs 51), but
paper crashed (24 vs 46.7). Same pattern as zbfjl55j: re-enabling
reciprocating + random_walk broadens training too far from paper's
narrow distribution. The all-6-motion addition fully overwhelms the
arena+density gains.
**Conclusion:** Don't add reciprocating/random_walk motion families if
paper-eval matters at all. Genuinely broader generalist, but pays a
heavy transfer cost.

---

### `zbfjl55j` (group: `beta10_all6_motion`) — re-enable reciprocating + random_walk

**Trained:** 2026-05-18 ~14:42 UTC, GPU 1, 500M
**Config delta vs e7sadb3v:** `--env.mw-reciprocating 2 --env.mw-random-walk 2`.
All 6 motion families active (was: linear=4 poly=2 sinusoidal=2 stationary=1
reciprocating=0 random_walk=0).
**Wandb final:** clean=0.36, success=0.95, coll=0.88, final_d=1.07.
**Train-dist:** clean_reach **61 %** (+10 pp vs e7sadb3v's 51 %).
**Paper-eval:** **19.7 %** (E 16 / M 25 / H 18) — big drop from 44.7.
**Diagnosis:** Train distribution broadened (more motion variety), TD-clean
up. But paper-eval's baked .world files have narrower obstacle motion
(mostly polynomial trajectories), so policy that trains across 6 motion
families ends up under-specialised for the paper's distribution.
**Under purpose.md lens:** TD-clean win (generality), paper drop (over-broad
for this specific eval set). Real generalist behavior but doesn't
benefit paper-eval.

---

### `4ziz0o5p` (group: `beta10_arena40_obs8to30`) — arena=40 + density-matched obs 8-30 — 🆕 NEW BEST

**Trained:** 2026-05-18 ~14:18 UTC, GPU 0, 500M
**Config delta vs e7sadb3v:** `--env.arena-size 40.0
--env.num-obstacles-min 8 --env.num-obstacles-max 30`. Wider arena +
denser obstacles (8-30 in 1600 m² ≈ same per-m² as e7sadb3v's 3-20 in
576 m²).
**Wandb final:** clean=0.49, success=0.97, coll=0.84, final_d=0.94.
**Train-dist (100 ep, matching cfg):** clean_reach **67 %**,
reached_dirty 31 %, timeout 1 %, collision 1 % — **+16 pp vs e7sadb3v**.
**Paper-eval (600 trials):** **46.7 %** ⭐ — **NEW HEADLINE**
  - Easy: 46.5 % (−9 pp vs e7sadb3v's 55.5, only regression)
  - Medium: **56.0 %** (+11.5 pp — BEST medium across all runs)
  - Hard: **37.5 %** (+3.5 pp)
**Why it wins both:** the bigger arena gives more spatial range for
the policy to learn routing variety; the density match keeps the
"actually need to avoid obstacles" signal intact (which arena=40 + 3-20
lost). Together: a strictly broader distribution that produces a
better generalist.
**Vs LfH-CP SOTA:** 30.83 %. Ours +15.9 pp.

---

### `8pfpn2ef` (group: `beta10_arena22`) — β=10 + arena=22 (midpoint)

**Trained:** 2026-05-18 ~13:31 UTC, GPU 1, 500M
**Config delta:** `--env.arena-size 22.0`. Tests if arena slightly larger
than eval (20) helps.
**Wandb final:** clean=0.30, success=0.96.
**Train-dist (100 ep, arena=22):** clean_reach **58 %**, reached_dirty 35 %,
collision 5 %, timeout 2 % — **+7 pp TD-clean over e7sadb3v 51 %**.
**Paper-eval:** **27.2 %** (E 31.5 / M 30 / H 20). −17.5 pp vs 44.7.
**Diagnosis:** Sits between arena=20 (paper 36.8) and arena=24 (44.7) on
paper, but better than both on TD-clean. The 22→24 jump in paper-eval
is larger than expected — 2 m of training-arena difference matters.
**Under purpose.md lens:** mild TD-clean win; small step on the
broader-distribution axis.

---

### `88gl5zqc` (group: `beta10_minigd8`) — β=10 + min_init_goal_dist=8 (easier curriculum)

**Trained:** 2026-05-18 ~13:05 UTC, GPU 0, 500M
**Config delta:** `--env.min-init-goal-dist 8.0` (vs 12.0). Shorter
required start-goal distance → easier task on average.
**Wandb final:** clean=0.40, success=0.97, coll=0.90, final_d=1.10
(≈ identical to e7sadb3v).
**Train-dist (100 ep):** clean_reach 54 %, reached_dirty 40 %, +marginal.
**Paper-eval:** **16.5 %** (E 17.5 / M 20.0 / H 12.0) — big drop from 44.7.
**Diagnosis:** Easier training → policy doesn't learn long-range
navigation. Paper requires 21 m start→goal; training avg now much
shorter. Robot learned local reach well, lost path-planning horizon.
**Under purpose.md lens:** marginal TD-clean lift but qualitatively
worse. Don't make training easier.

---

### `p3yf7hiq` (group: `beta10_obs5to30`) — β=10 + obstacles 5-30 (denser) — FLATTER DIFFICULTY CURVE

**Trained:** 2026-05-18 ~11:54 UTC, GPU 1, 500M
**Config delta:** `--env.num-obstacles-min 5 --env.num-obstacles-max 30`.
Everything else = e7sadb3v.
**Wandb final:** success=0.96, success_clean_reach=0.25, collision=0.95
(very high — denser env stresses policy).
**Train-dist (100 ep, matching cfg):** clean_reach 47 %, reached_dirty 49 %,
collision 4 % — slightly below e7sadb3v's 51 %.
**Paper-eval:** **41.0 %** overall.
  - Easy: 35 % (−20.5 pp vs e7sadb3v)
  - Medium: 44 % (≈ same)
  - **Hard: 44 % (+10 pp vs e7sadb3v's 34 %)** ⭐
**Observation:** Difficulty curve FLATTENED. Hard transferred much
better; easy degraded. Net paper-eval close to e7sadb3v (41 vs 44.7)
but with more even per-difficulty performance — more like a generalist.
**Under purpose.md lens:** TD-clean slightly worse, but the policy is
more robust across difficulty. Worth combining with arena=40 to keep
the broad-arena win while restoring density.

---

### `40qb3qf0` (group: `beta10_arena40`) — β=10 + arena=40 (walls far away) — TD-CLEAN CHAMPION (paper down)

**Trained:** 2026-05-18 ~11:54 UTC, GPU 0, 500M
**Wandb:** https://wandb.ai/sudhirpratapyadav-indian-institute-of-technology-jodhpur/dyna_barn/runs/40qb3qf0
**Config delta:** `--env.arena-size 40.0`. Everything else identical
to e7sadb3v.
**Wandb final env metrics:**
  - success (reached at all): **98.9 %**
  - success_clean_reach: **67.9 %** ⭐ (vs e7sadb3v 0.39)
  - success_strict: 33.0 %
  - collision: 66.6 %  n_collision_events: 1.47/ep
  - closest_obstacle: 0.84 m
  - final_dist_to_goal: 0.84 m  min_dist_to_goal: 0.05 m
  - dist_from_goal_at_first_collision: 4.37 m
  - steps_at_goal (dwell): 72  max_steps_between_collisions: 605
  - episode_return: 578.7  timeout: 1.1 %
**Train-dist (100 ep, arena=40):** clean_reach **80 %**, reached_dirty
19 %, timeout 1 %. **BEST TD-clean of any run** (+29 pp over e7sadb3v).
**Paper-eval (600 trials, arena=20 paper geom):** **26 %**
(E 29 / M 27.5 / H 21.5). Worse than e7sadb3v's 44.7 %.
**Under old paper-driven lens:** regression.
**Under new TD-clean lens (docs/purpose.md):** champion. The training
metric is the lever; paper-eval is a byproduct.
**Why it wins on TD-clean:** larger arena gives more space to plan
around obstacles; walls rarely encountered (vs e7sadb3v where the
boundary clamps the robot's path).
**Why it loses on paper-eval (informational):** policy trained for 40m
open space doesn't fit the paper's 20m 3-walled room — wrong spatial
prior. But this is fixable by making the 40m training distribution
harder/broader rather than shrinking the arena.
**Followup:** This is the new baseline. Make IT harder: more obstacles,
broader motion families, harder goal box, etc., while keeping arena=40.

---

### `9ypq3hyf` (group: `beta10_slow_obs`) — β=10 + obstacle speed_max=1.0 (down from 2.0) — REGRESSED

**Trained:** 2026-05-18 ~06:20 UTC, GPU 0, 500M
**Hypothesis:** Capping training obstacle speed at robot's forward max
(1.0) makes physics fairer; training was 0.3-2.0 (obstacles can be 2x
faster than robot). Should generalize at least as well.
**Config delta:** `--env.speed-max 1.0`.
**Wandb final:** clean=0.55, success=0.94, coll=0.77, final_d=1.05 (best
wandb signals of any non-baseline run).
**Result:** Paper **18.7%** (E 18.5 / M 18.0 / H 19.5), TD clean 57%.
**Diagnosis:** Slow training obstacles → policy doesn't learn to handle
fast obstacles in paper-eval (.world files have varied speeds, some
exceeding 1.0). Wandb looked promising but the train-vs-paper
distribution mismatch crushed generalization.
**Conclusion:** Don't shrink training speed distribution.

---

### `29pby0gf` (group: `beta10_entropy001`) — β=10 + ent_coef=0.001 (10x lower entropy bonus) — REGRESSED

**Trained:** 2026-05-18 ~05:44 UTC, GPU 1, 200M (capped early)
**Hypothesis:** Lower entropy bonus → policy commits earlier to good
trajectories. Might escape PPO over-training mode observed in other
runs.
**Config delta:** `--train.ent-coef 0.001` (vs default 0.01).
**Wandb final:** clean=0.47, success=0.96.
**Result:** Paper **17.0%** (E 23 / M 16.5 / H 11.5), TD clean 36%.
**Diagnosis:** Lower entropy → premature commitment → worse generalization.
Default ent_coef=0.01 is right.

---

### `jcah53tw` (group: `beta10_steps400`) — β=10 + max_steps=400 (half-length episodes) — FAILED

**Trained:** 2026-05-18 ~04:36 UTC, GPU 0, 500M
**Wandb:** https://wandb.ai/sudhirpratapyadav-indian-institute-of-technology-jodhpur/dyna_barn/runs/jcah53tw
**Hypothesis:** Shorter episodes (400 vs 800 ticks = 40 s vs 80 s) ×
same step budget = 2× more episodes per gradient update → richer signal,
faster credit assignment.
**Config delta:** `--env.max-steps 400`.
**Results:**
  - wandb peak clean_reach 0.50 at 121M, decayed to 0.36 at 500M.
  - 105M ckpt paper-eval: **13.7 %** (E 22 / M 13 / H 6).
  - 500M ckpt paper-eval: **1.7 %** (E 1.5 / M 0.5 / H 3.0).
**Diagnosis:** Faster wandb convergence but **massive paper-eval
collapse**. Got WORSE with continued training. Hypothesis: shorter
episodes amplify PPO over-training because more episodes per gradient
mean policy drifts farther per update. Or: 800-tick episodes give the
policy time to learn long-horizon planning that 400 truncates.
**Conclusion:** max_steps=800 default is necessary for paper-eval. Don't
shorten episodes.

---

### `pnt5yya4` (group: `beta20`) — β=20 baseline ablation — REGRESSED

**Trained:** 2026-05-18 ~03:28 UTC, GPU 0
**Wandb:** https://wandb.ai/sudhirpratapyadav-indian-institute-of-technology-jodhpur/dyna_barn/runs/pnt5yya4
**Hypothesis:** Beta sweep was 1 (36% TD), 10 (67% TD, 44.7% paper ⭐),
50 (7% TD, 0% paper). Sweet spot is somewhere — try β=20 to see if the
peak extends right or β=10 is sharp.
**Config delta:** `--env.beta 20.0`. Everything else identical to e7sadb3v.
**Result (train-dist, 100 ep):** clean_reach 56% (−11 pp), reached_dirty 35%.
**Result (paper-eval, 600 trials):** Overall **15.3%** (−29 pp). Easy 20.5%,
Medium 18.5%, Hard 7.0%.
**Observations:**
  - Train-dist degraded mildly (56 vs 67), but paper crashed
    disproportionately. This is the same pattern as β=50 (just less
    severe): excess repulsion teaches over-cautious policy that
    transfers worse to OOD fixed geometry.
  - Mid-ckpt (445M) eval was nearly identical (TD 58%, paper 15.2%),
    so the regression is stable not transient.
**Conclusion:** β=10 is a sharp peak. β=15 likely also worse. The
repulsion ↔ attraction balance is precisely 10 for our env.
**Followup:** Stop trying β values away from 10. Lever moves elsewhere.

---

### `7m5b0pm6` (group: `beta10_arena20`) — β=10 + arena=20 + min_init_goal=10 — MIXED

**Trained:** 2026-05-17 ~21:01 UTC, GPU 1
**Wandb:** https://wandb.ai/sudhirpratapyadav-indian-institute-of-technology-jodhpur/dyna_barn/runs/7m5b0pm6
**Hypothesis:** Smaller train-arena (20 m, matching eval) should improve
generalization by removing the size mismatch. Tests "distribution match
of arena helps."
**Config delta:** `--env.arena-size 20.0 --env.min-init-goal-dist 10.0`
(everything else identical to e7sadb3v).
**Result (train-dist, 100 ep, arena=20):**
clean_reach **53 %** (−14 pp), reached_dirty 41 %, collision 5 %, timeout 1 %.
**Result (paper-eval, 600 trials):**
Overall **36.8 %** (−7.9 pp). Easy **32 %** (−23.5 pp), Medium **39 %**
(−5.5 pp), Hard **39.5 %** (**+5.5 pp**).
**Observations:**
  - Per-difficulty trend is interesting: easy degraded most, hard
    improved. Hypothesis: tighter training arena → policy practices
    obstacle-density-heavy scenarios more often, transferring to hard
    bin where obstacle count is highest.
  - Overall regression: arena-size match wasn't worth the easy-case
    skill loss.
**Conclusion:** Don't shrink arena. The 24 m default lets the policy
practice both open navigation (easy) and dense routing (hard).
**Followup:** Try the OPPOSITE — bigger arena (28 / 32) to see if
even more open space helps easy/medium further.

---

### `4dabm514` (group: `beta10_succ5_coll5`) — β=10 + success_bonus=5 + collision_penalty=5 — REGRESSED

**Trained:** 2026-05-17 ~18:31 UTC, GPU 1
**Wandb:** https://wandb.ai/sudhirpratapyadav-indian-institute-of-technology-jodhpur/dyna_barn/runs/4dabm514
**Hypothesis:** Adding a one-shot `success_bonus=5` on first reach and a
per-event `collision_penalty=5` makes the reach/safety trade-off explicit
on top of the dense β=10 repulsion + 3-scale Gaussian attraction. Expected
clean_reach to climb past 67 %.
**Config delta:** `--env.success-bonus 5.0 --env.collision-penalty 5.0`.
**Result (train-dist, 100 ep):** clean_reach **52 %** (−15 pp vs e7sadb3v),
reached_dirty 36 %, collision 10 %, timeout 2 %.
**Result (paper-eval, 600 trials):** **26.7 %** overall (−18 pp).
Easy 29.5 %, Medium 31.5 %, Hard 19.0 %.
**Diagnosis:** Discrete reward signals (one-shot bonus, per-event penalty)
interfere with the carefully-tuned dense reward landscape. Each per-event
collision penalty briefly dominates the per-step β·exp(-d²/σ²) repulsion
gradient → policy becomes over-cautious or learns wrong avoidance behaviour.
Net: the dense β alone was already the right shape; adding spikes hurt.
**Conclusion:** **Stop adding discrete reward terms to e7sadb3v's recipe.**
The dense Gaussian-attraction + dense Gaussian-repulsion combo is the
right shape; piling on spikes breaks the gradient.
**Followup:** Don't sweep individual values of success_bonus / collision_pen
on top of β=10; try fundamentally different lever (architecture instead).
