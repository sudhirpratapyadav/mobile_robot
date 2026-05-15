# Improvement options — pick what to try next

We're locked into the **paper-eval kinematic envelope** (v ∈ [-0.5, 0.5] m/s,
w ∈ [-π, π] rad/s, a_max=10, α_max=20) and **generalization protocol** (train
on procedural worlds, eval on the published 60). Goal: push past LfH-CP's
30.83% overall under that constraint.

Current honest baseline (option-C reward, 100M, run `ukwnlxj3`):
**20.8% overall** (40.0 / 16.0 / 6.5 % easy/medium/hard).

Iteration budget: **100M steps per variant**; scale only what wins.

---

## What I think are the actual bottlenecks

Before listing options: my read on what's hurting us most right now.

1. **Policy can't predict obstacle motion.** Single-frame LiDAR + slow robot
   = obstacle moves a meter while we react. Policy literally cannot
   distinguish "obstacle approaching" from "obstacle receding" in 1 frame.
2. **No global plan.** Goal vector is absolute. Policy has to discover
   the route around every obstacle from raw LiDAR each step. Classical
   baselines and LfH-CP both use a global Dijkstra path.
3. **Reactive, single-step decisions.** Policy outputs one action; can't
   plan a maneuver across several steps even when the situation is clear.

Most options below directly address one of these three.

---

## A. Observation

Cheap to try; high leverage.

### A1. History-stacked LiDAR (L=5 frames)
- **Why**: lets the policy infer obstacle velocity. Matches LfH-CP's input.
- **Cost**: medium — env ring buffer, obs size 720 → 3604 floats, ~3-4× SPS hit.
- **Expected gain**: large (biggest single physical improvement).

### A2. Add ego velocity (v, w) to obs
- **Why**: under accel limits, current v changes slowly — knowing it lets
  the policy reason "I can or cannot brake before the wall."
- **Cost**: trivial (+2 dims).
- **Expected gain**: small but free.

### A3. Finite-difference LiDAR (Δscan)
- **Why**: cheap proxy for A1 — tells the policy *which* beams changed.
- **Cost**: small (obs size 720 → 1440).
- **Expected gain**: small to medium. Worse than A1 but ~2× cheaper.

### A4. Polar goal representation (distance, bearing)
- **Why**: bounded values are easier for the network than `(dx, dy)` in
  metres.
- **Cost**: trivial.
- **Expected gain**: minor.

### A5. Down-sample LiDAR (720 → 60)
- **Why**: SPS budget for richer policies. LfH-CP's 60-beam baseline.
- **Cost**: trivial.
- **Expected gain**: neutral on quality, big on SPS.

---

## B. Policy network

### B1. 1D CNN over LiDAR scan
- **Why**: scan has spatial structure (neighboring beams correlate).
  Currently a plain MLP ignores that.
- **Cost**: low — torch-side custom model in PufferLib.
- **Expected gain**: small to medium.

### B2. Causal Transformer over scan history
- **Why**: LfH-CP's setup. Best for "predict obstacle motion."
- **Cost**: high — needs A1 + custom torch policy.
- **Expected gain**: medium to large (matches SOTA architecture).

### B3. Recurrent policy (GRU / LSTM)
- **Why**: cheaper than B2 for adding memory; PufferLib has `use_rnn=1`.
- **Cost**: low — flip a config flag.
- **Expected gain**: medium. Easy first step toward "policy with memory."

### B4. Larger MLP (256×3 or 512×3)
- **Why**: Currently 128×2. May be undersized for 720-d obs.
- **Cost**: trivial.
- **Expected gain**: small. Rarely the bottleneck for nav.

### B5. Diffusion / flow policy
- **Why**: better at multi-modal action distributions (e.g. "go left or
  right" both valid).
- **Cost**: high — custom torch + sampling-time inference.
- **Expected gain**: speculative; worth trying *after* simpler stuff lands.

### B6. Distillation / hierarchical (high-level + low-level)
- **Why**: high-level chooses subgoal, low-level executes it.
- **Cost**: very high.
- **Expected gain**: speculative; usually overkill at our scale.

---

## C. Action space

### C1. Action chunking (predict K future actions)
- **Why**: LfH-CP predicts 5 future actions, executes the first. Forces
  multi-step reasoning at the action level.
- **Cost**: medium — policy outputs K×2, env runs the first only.
- **Expected gain**: medium; complements A1/B3.

### C2. Discrete action grid (e.g. 5×5)
- **Why**: lower-variance gradient than continuous Gaussian; easier
  exploration for the RL phase.
- **Cost**: low — change action shape + binding.
- **Expected gain**: small to medium. Quick test.

### C3. Bounded action distribution (Beta or tanh-Gaussian)
- **Why**: the current Gaussian + clip has dead-zones at the boundary.
  Beta naturally lives in [0, 1].
- **Cost**: low.
- **Expected gain**: small.

### C4. Action-smoothness penalty
- **Why**: penalize `|a_t - a_{t-1}|` to encourage smooth control.
- **Cost**: trivial.
- **Expected gain**: small. Already implicit via accel limits.

---

## D. Model-based / planning

### D1. Global Dijkstra + RL local planner
- **Why**: matches paper baselines + LfH-CP. Replace absolute goal vector
  with "next waypoint 2.25 m ahead along a Dijkstra path" computed from
  the **static obstacle layout** (ignoring dynamic motion — same as
  move_base global planner).
- **Cost**: medium — implement Dijkstra over a known-obstacle grid in C.
  Recompute every reset (or every K steps).
- **Caveat**: uses ground-truth obstacle positions. Not strictly
  comparable to a from-scratch RL baseline, but matches what LfH-CP
  actually deploys. Honest framing: "RL-as-local-planner."
- **Expected gain**: large.

### D1-pure. Same as D1 but build map from LiDAR
- **Why**: avoids the ground-truth-cheat in D1. Robot accumulates an
  occupancy grid from the LiDAR scans, runs Dijkstra on its own map.
- **Cost**: high — incremental map-build + Dijkstra each step.
- **Expected gain**: same as D1 once it works; harder to debug.

### D2. Learned world model + MPC (Dreamer / TD-MPC style)
- **Why**: learns dynamics, plans via shooting / CEM in latent space.
- **Cost**: very high.
- **Expected gain**: speculative; probably skip.

### D3. Imitation warm-start from a classical planner
- **Why**: collect rollouts from a DWA/TEB baseline on procedural worlds,
  BC-train, then RL-finetune. Matches LiCS approach.
- **Cost**: medium — needs a runnable classical planner.
- **Expected gain**: medium. Helps escape early-training collapse.

---

## E. Training distribution / curriculum

### E1. Obstacle-count curriculum (3 → 25)
- **Why**: standard easy-to-hard.
- **Cost**: low — schedule the `num_obstacles_max` knob.
- **Expected gain**: small to medium.

### E2. Speed curriculum (slow → fast obstacles)
- **Why**: paper hard-bin obstacles average 1.5 m/s, our easy bin is
  0.5–1.0. Annealing during training matches the eval distribution
  monotonically.
- **Cost**: low.
- **Expected gain**: medium.

### E3. Mix in long-horizon worlds (more obstacles, longer episodes)
- **Why**: gives policy time to learn cautious-but-effective navigation.
- **Cost**: trivial.
- **Expected gain**: small.

### E4. Extra obstacle motion families (already implemented but disabled)
- **Why**: `shared/motion.h` has 6 families. We're poly-only right now to
  match the eval distribution exactly. Re-enabling broader families
  trades some test-distribution match for generalization headroom.
- **Cost**: trivial — flip the `mw_*` weights in .ini.
- **Expected gain**: ambiguous (helped on easy/medium, hurt on hard last
  time we tried it; under the new envelope might be different).

### E5. Domain randomization on dt, sensor noise, goal noise
- **Why**: prep for sim-to-real transfer.
- **Cost**: low.
- **Expected gain**: minor for sim-only number; useful later.

### E6. Reverse curriculum (start near goal, expand outward)
- **Why**: helps reward-sparse settings.
- **Cost**: low.
- **Expected gain**: low — our reward isn't sparse anymore (option C
  added shaping).

---

## F. Reward / training algorithm

### F1. SAC instead of PPO
- **Why**: more sample-efficient with continuous actions.
- **Cost**: medium — needs PufferLib's SAC backend (verify it has one).
- **Expected gain**: medium; might unlock 2-3× sample efficiency.

### F2. Reward magnitude tuning (option-C variants)
- **Why**: cheap. Sweep `time_penalty ∈ {0.001, 0.005, 0.01}`,
  `alpha_g ∈ {0.02, 0.05, 0.1}`, `collision_penalty ∈ {0.5, 1, 2}`.
- **Cost**: trivial — config sweeps.
- **Expected gain**: 5-10% headline swing.

### F3. Curiosity / intrinsic reward
- **Why**: encourages exploration.
- **Cost**: medium.
- **Expected gain**: low — we're not exploration-bottlenecked under
  option C.

### F4. Higher entropy bonus
- **Why**: may help escape local optima.
- **Cost**: trivial — `ent_coef` knob.
- **Expected gain**: low to medium. Cheap to try.

### F5. Lower lr / longer warmup
- **Why**: stability fix if training oscillates.
- **Cost**: trivial.
- **Expected gain**: low — current run is stable.

---

## My ranked picks for the next batch (just my opinion)

If I had to pick 4 × 100M experiments to run next, I'd pick:

| Priority | Variant | Why |
|---|---|---|
| 1 | **A1 + B3** (5-frame history + GRU) | biggest physical fix; tests memory directly |
| 2 | **A2** (add ego velocity to obs) | cheapest test; isolates "is policy missing self-state info?" |
| 3 | **D1** (global Dijkstra + RL local) | matches LfH-CP's deploy story; expected huge bump |
| 4 | **C1** (action chunking K=5) | tests "multi-step planning at action level" separately from "memory at obs level" |

Skip D2, E5, F1, F3 for now — too costly relative to expected gain.

---

## Things to decide before launching anything

1. **Sequential vs parallel** — I can build all four variants up-front then
   queue trains, OR build → train → eval → review one at a time. Sequential
   is what I'd default to (avoids implementing things you won't want).
2. **Does the option-C reward stay fixed across all variants** so we're
   isolating the architectural change, or do we co-tune?
3. **Eval protocol**: stick with `eval_paper` (corrected geometry, accel
   on, v=0.5)? Or also a "loose envelope" eval for our own headline?

---

## G. Egocentric occupancy grid — LingBot-inspired path (PICKED 2026-05-15)

User direction: keep things as simple as possible, build a quick-and-dirty
single-step costmap first, then extend to a temporal stack.

### Philosophy (carried over from LingBot-Map)

LingBot-Map (Ant Robbyant, 2026; arXiv 2604.14141) is a 3D RGB-streaming
SLAM model. Its central design choice is what we want to borrow:

> Don't choose between explicit geometry and learned features. Have both.
> Use the network to learn the *operations* a classical SLAM stack does
> by hand, but keep the world-state in an explicit, interpretable,
> geometric form.

Concretely it bakes three classical-SLAM concepts into the architecture as
inductive biases:

| Classical SLAM concept | LingBot-Map's neural analogue | Our adaptation |
|---|---|---|
| Anchor frame / coord system | "Anchor context" tokens | Robot body frame |
| Local sliding window of recent frames | "Pose-reference window" | Last N LiDAR scans, ego-motion-corrected |
| Loop closure / drift correction | "Trajectory memory" tokens | (Skip — we have perfect odometry) |
| Bundle adjustment | 4 grad-descent steps on pose per frame | (Skip — same reason) |
| Output | **Explicit 3D point cloud, not latent** | **Explicit 2D occupancy grid, not latent** |

For our 2D LiDAR nav at v=0.5 m/s in a 20×20 m world, most of the
SLAM-flavored complexity drops out. What's left is the **explicit-2D-grid
representation** + **maybe-temporal-stack**.

### G1. Single-step egocentric costmap (FIRST EXPERIMENT)

Quickest viable thing.

- Each step, rasterize the **current** LiDAR scan into a 64×64 grid in
  robot body frame, covering ±5 m. Cell value = 1 if any beam hit there,
  else 0. (Cheap dirty version: walk the 720 beam endpoints, mark the cell
  containing each endpoint. Optional cheap improvement: also mark cells
  along the beam ray between origin and endpoint as 0 / -1 = "free".)
- New env knob `obs_mode` ∈ {`lidar`, `costmap`}. Same env, same physics,
  same reward. Just the observation changes.
- Obs payload (costmap mode): `[grid_64x64 | v | w | goal_dx_body | goal_dy_body]`
  = 4096 + 4 = 4100 floats.
- Custom torch policy: 3-layer 2D CNN
  (1→16, k=5, stride 2 → 16→32, k=3, stride 2 → 32→64, k=3, stride 2)
  → flatten → concat with `(v, w, goal)` → MLP head.
- Same option-C reward, same procedural training, same `eval_paper`.
- Budget: 100M (same as other variants).

**Hypothesis**: explicit 2D grid + small CNN beats raw 720-d LiDAR + MLP
under the same training budget, even *without* temporal stacking.

### G2. Multi-step temporal grid stack (SECOND EXPERIMENT, only if G1 wins)

LingBot-Map's "pose-reference window" analogue.

- Keep last 3 grids in body frame, **rotated/translated into the current
  robot frame** using known ego-pose deltas (free in sim).
- 3 channels stacked → CNN input is 64×64×3.
- Same architecture, just channel count changes.
- Hypothesis: temporal channels let the policy infer obstacle motion
  (as channel-difference at the pixel).

### Why this is simpler than my original "B" sketch

Same idea but explicit: don't try to be clever with decay or with
ray-tracing. Just rasterize the endpoints into a binary grid in body
frame. Speed > fidelity for the dirty version.

### What's deliberately skipped (for now)

- **Persistent map across the episode** — saves memory, makes tracking
  obstacle motion easier (cells appear/disappear as the obstacle moves).
  Add later if temporal stack helps but plateaus.
- **Free-space marking via raytracing** — only mark cell containing the
  endpoint, not cells along the ray. Less informative but simpler.
- **Decay** — "cell remembers a hit for K steps." Adds another knob;
  start without it.
- **Multi-channel encoding** — e.g. one channel per "velocity bin" of
  detected obstacle. Definitely later.

### Decision path

1. G1 lands at, say, 25%. Same ballpark as option-C lidar (20.8%) → obs
   structure isn't the bottleneck → look at planning (D1) or training.
2. G1 lands at 35%+. Beats LfH-CP's 30.83% with a 100M run → obs
   structure was huge → run G2 to push further.
3. G1 worse than option-C → grid quality too poor; add raytracing or
   decay; or revisit the rasterization resolution.

---

## Tracking

| Run | Variant | Steps | Overall (eval_paper) |
|---|---|---|---|
| `b3vm6fej` | poly + v0.5 + accel + old reward | 5B | **0.0% (collapsed)** |
| `ukwnlxj3` | poly + v0.5 + accel + option-C reward | 100M | **20.8%** |
| TBD | G1 single-step costmap | 100M | TBD |
