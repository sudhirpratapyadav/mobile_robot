# Logger — change log book (dyna_barn)

Append-only timestamped history of every change to env code, scripts, config, or
eval infrastructure in this project. **Do not edit or delete previous entries** —
corrections go in a new entry that references the older one. Same convention as
the previous project's `logger.md`.

**Entry format** (newest at the bottom):

```
## YYYY-MM-DD [HH:MM TZ] — short title
**Author:** name
**Files touched:** path1, path2
**Commit (if any):** <sha or "uncommitted">

- bullet of what changed
- bullet of why
- bullet of side-effects / things to verify
```

---

## History before this repo

This project is a hard fork from the `mobile_robot_env/` work in the parent
directory (`../mobile_robot_env/`). All decisions and exploratory work prior to
2026-05-14 are logged in `../mobile_robot_env/logger.md`. The key decisions that
carry over to here:

- Robot model: differential-drive Jackal, `v ∈ [−0.5, 2.0]` m/s, `w ∈ [−π, π]`
  rad/s (per the DynaBARN paper).
- Sensor: 720-beam 2D LiDAR, 270° FOV (per the BARN Challenge 2024 paper).
- Obstacle model: cylinders r=0.5 m with deterministic polynomial-fit waypoint
  trajectories (per DynaBARN paper §III-A).
- Storage format: parquet for trajectory dumps (not CSV).
- Viz: OpenCV (cv2 primitives) for per-episode static PNGs; matplotlib only for
  summary charts.
- Compile-time constants: `LIDAR_BEAMS = 720`, fixed obs shape per PufferLib
  contract.
- Two-env split (train / eval) as decided 2026-05-14 (this turn).
- Observation: single-frame baseline (722 floats = 720 lidar + 2 goal),
  no history stacking yet.
- Eval protocol: DynaBARN-paper-style 20 envs × 10 trials per difficulty bin =
  200 trials/bin, 600 total.
- Eval worlds: regenerated from the same `[order, speed, std]` distributions
  as DynaBARN's published bins (no `.world` parsing).

The full reference doc for the benchmark is `docs/dyna_barn.md` (moved from the
old project unchanged).

---

## 2026-05-14 — Repo seeded
**Author:** Sudhir (with Claude)
**Files touched:** `.gitignore`, `README.md`, `logger.md`, `exp_tracker.md`, `docs/` (moved from `../mobile_robot_env/docs/`)
**Commit:** uncommitted

- Created the folder skeleton: `train_env/`, `eval_env/`, `shared/`, `docs/`,
  `runs/{train,eval}/`.
- `git init` — separate repo from the parent `mobile_robot_env/` tree.
- Moved `docs/dyna_barn.md` and the DynaBARN paper PDF over from
  `../mobile_robot_env/docs/`. The old `docs/` folder was empty and was removed.
- Wrote `README.md` summarising the two-env split and the shared spec.
- Seeded this `logger.md` with a back-pointer to the parent project's history
  and the carry-over decisions.
- No env code yet. Next concrete step: scaffold `shared/` (Jackal dynamics,
  cylinder/waypoint obstacle struct, LiDAR raycast) and the train env on top of
  it.

---

## 2026-05-14 — Full env + pipeline scaffold (autonomous session)
**Author:** Sudhir (with Claude, autonomous mode)
**Files touched:** see below
**Commit:** uncommitted

End-to-end build out from empty skeleton → working train + eval + viz +
pipeline. Tasks #1–#10 from the in-session task list all completed.

Added:
- `setup_container.sh` — symlinks `dyna_train`, `dyna_eval` into PufferLib's
  `ocean/` and their `.ini` files into `config/`.
- `shared/jackal.h` — differential-drive Jackal integrator with action ranges
  v∈[-0.5,2] m/s, w∈[-π,π] rad/s (paper §IV-b). Includes body-frame rotation
  + uniform/normal RNG helpers.
- `shared/obstacle.h` — `ObstacleTrajectory` struct + time-interpolation
  function. Radius 0.5 m fixed (paper §III-A).
- `shared/lidar.h` — 720-beam LiDAR raycast against arena walls (4 segments)
  and cylinders. Header-only, naive O(beams × obstacles). LiDAR_FOV = 270°,
  LiDAR_RANGE = 30 m.
- `shared/traj_gen.h` — port of `polynomial_fit.py` (paper Algorithm 1).
  Vandermonde + Gauss-elim solve, evaluates polynomial at every integer x in
  [-A, A], includes the original control points, dedup + sort. Stress-tested
  100% success across 30k trajectories on all 3 difficulty bins.
- `dyna_train/`: `dyna_train.{h,c}`, `binding.c`, `dyna_train.ini`. Procedural
  env with curriculum knobs in the ini, random start/goal.
- `dyna_eval/`: `dyna_eval.{h,c}`, `binding.c`, `dyna_eval.ini`. Fixed start
  (0, 9) → goal (0, -9), difficulty selected via config, deterministic
  per-episode RNG via `world_seed_base + episode_idx`.
- `tools/bake_traj_parquet.py` — converts the standalone-driver `.bin` dump
  to a polars-readable parquet.
- `tools/viz_cv.py` — OpenCV-based viz (single-grid PNG, per-episode PNGs,
  paginated 5×5 grids, mp4/gif animations). ~100× matplotlib speed.
- `tools/eval_summary.py` — aggregates outcomes per difficulty bin, prints +
  writes `summary.json`.
- `run_eval.sh` — DynaBARN-paper-style protocol: 3 difficulties × 200 trials
  per checkpoint.
- `run_pipeline.sh` — build → random-action baseline → train → eval. Has
  `TIMESTEPS` and `SKIP_TRAIN` env-vars.

Decisions made along the way:
- **Env name `dyna_eval` vs. `eval_env`:** Pufferlib's `build.sh` expects
  `ocean/<name>/<name>.c`, so the env dir name must equal the main C file
  stem and must be globally unique. Renamed `train_env` → `dyna_train` and
  `eval_env` → `dyna_eval`.
- **Action mapping:** `a₀, a₁ ∈ [-1, 1]` (unit-square policy output), mapped
  to physical `v ∈ [V_MIN, V_MAX], w ∈ [-W_MAX, W_MAX]` inside `c_step`.
  Allows reverse (a₀ = -1 → v = -0.5 m/s) per the paper.
- **Observation:** LiDAR ranges normalised by `LIDAR_RANGE` so obs is in
  `[0, 1]`. Goal in body frame in raw metres (~ ±15 m → easy to learn).
- **Eval start clamped to (0, 9):** Paper says (0, 11) but our arena is
  [-10, 10]². 11 is outside the wall, so the robot would be unable to spawn.
  Clamped to 9 (1 m clearance from the wall, accounting for footprint).
- **Reward identical between train and eval envs:** Eval ignores reward
  (only counts outcomes), but they share the same shaping for code-reuse
  reasons. Slight inconsistency — eval should arguably be pure success/fail.

Smoke results:
- Trajectory-generator stress test: 100% success, mean ~13 waypoints per
  trajectory, durations 1–120 s across all difficulty bins.
- Standalone driver: 100 episodes × random actions → bin → parquet → viz in
  ~1.5 s total.
- PufferLib training: 275k SPS on single GPU.
- Random-action baseline on eval (DynaBARN protocol, 600 trials): 1.0%
  overall success.
- **First trained policy at 50M steps: 84/71/47.5% (easy/medium/hard),
  67.5% overall.** Full details in `exp_tracker.md` entry for run
  `1778772096469`.

Known caveats (in `exp_tracker.md`):
- Eval worlds are drawn from our own generator at DynaBARN's parameter bins,
  not the actual 60 published `.world` files. Numbers are not yet
  apples-to-apples with Dyna-LfLH / LfH-CP.
- Policy is trained random-start/goal, evaluated on the fixed paper route.
- No history-stacked observations — single-frame LiDAR baseline.

Open / next:
- Bake the 60 published DynaBARN worlds into our binary format (needed for
  real SOTA comparison).
- Full 500M-step training run.
- Ablation: L=5 history scans.
- Visual inspection of hard-bin failures with `viz_cv`.

---

## 2026-05-14 — Bake + eval on the 60 published DynaBARN worlds
**Author:** Sudhir (with Claude)
**Files touched:** tools/bake_worlds.py, tools/classify_worlds.py,
                  tools/render_baked_world.py, tools/eval_published_summary.py,
                  shared/world_loader.h, dyna_eval/dyna_eval.{h,c},
                  run_eval_published.sh, external/DynaBARN/, external/baked_worlds/
**Commit:** uncommitted

Sudhir manually downloaded the DynaBARN.zip (113 MB) into `dyna_barn/` from
the official Tufts Box link (which is unreachable from this sandbox via curl
— browser flow was required). Extracted to `external/DynaBARN/` with 600
compiled `.so` plugins + 60 `.world` SDFs + the generator scripts.

Bake methodology — **no .cc source ships with the dataset; only compiled
`.so`** files. Recovered waypoints by disassembly:

- Each plugin's `Load()` function emits a sequence of:
  `CreateKeyFrame(time); key->Translation(Vector3d(x, y, 0));` calls.
- The `time` argument arrives in `xmm0` from one of:
  1. `movsd ADDR(%rip),%xmm0`  (most cases)
  2. `mov ADDR(%rip),%rax; mov %rax,STACK; movsd STACK,%xmm0` (some cases)
  3. `pxor %xmm0,%xmm0`        (t = 0.0 first keyframe)
- The `x` and `y` are loaded via `movsd ADDR(%rip),%xmm0` then stored to
  the `Vector3d` stack slots.
- `tools/bake_worlds.py` walks `objdump -d` output for each plugin,
  tracks rip-relative double-loads (matching `movsd|mov`, comment-resolved
  vaddr), and pairs them with the call-site sequence to recover all
  `(time, x, y)` triples per obstacle.
- Output format: small flat binary (`uint32 magic|version|n_obs`, then per
  obstacle `uint32 n_waypoints` and `float32[3]` per waypoint).

Verification:
- Stress: all 60 worlds baked in 11s, 0 errors. Obstacle counts 5–19, total
  waypoints 100–212, durations 42–86s, all coordinates within [-10, 10].
- Match: `dyna_eval --world-file world_000.bin` t=0 obstacle positions match
  the baked file's first-waypoint positions byte-exact.
- Visual: rendered overlays for worlds 0, 30, 50 (`runs/eval/baked_check/`).
  Trajectories are clearly polynomial curves crossing the arena, start/end
  on or near the boundaries. Matches the paper Fig. 1 visual style.

Difficulty classification (`tools/classify_worlds.py`):
- Auto-classified per Fig. 2 tree: `n_obs < 10 AND mean_speed < 1.0` → easy;
  `n_obs ≥ 10 AND mean_speed ≥ 1.0` → hard; mixed → medium.
- Result: perfect 20/20/20 split (easy = worlds 0–19, medium = 20–39,
  hard = 40–59). The published worlds are **ordered by difficulty**.

`dyna_eval` extended:
- New `world_file_path[512] + world_file_set` fields on the env struct.
- When `world_file_set=1`, `c_reset` calls `world_loader_load()` (new
  `shared/world_loader.h`) instead of generating fresh trajectories.
- Standalone CLI: `--world-file <path>`.

New `run_eval_published.sh`:
- Loops over `external/baked_worlds/world_*.bin`, runs N trials per world.
- Per-world seeds = `10000 + idx*100` (deterministic, reproducible).
- Aggregates into `summary.json` via `tools/eval_published_summary.py`.

Result on the 50M-step checkpoint (`1778772096469`):

| Bin | 10-trial (paper protocol) | 2-trial (LfH-CP protocol) |
|---|---|---|
| Easy | 90.0% | 90.0% |
| Medium | 55.0% | 55.0% |
| Hard | 30.0% | 30.0% |
| **Overall** | **58.3%** | **58.3%** |

vs. published SOTA on the same 60 worlds: LfH-CP 30.83%, Dyna-LfLH 22.5%.

Full caveats + per-bin breakdown in `exp_tracker.md`.

Open follow-ups (same as exp_tracker):
- Train to 500M; revisit hard-bin number.
- L=5 history stacking.
- Visualize hard-bin failures.
- Sim-to-sim deployment in Gazebo for a clean apples-to-apples comparison.

---

## 2026-05-14 — 500M-step training run + WebM rendering pipeline
**Author:** Sudhir (with Claude)
**Files touched:** tools/render_eval_gifs.py (WebM/h264 codec branches);
                  runs/train/dyna_train/1778776723464/ (training run);
                  exp_tracker.md (new run entry)
**Commit:** uncommitted

Training:
- Same `dyna_train.ini` config as before; only `total_timesteps` defaulted
  to 500M (no override needed). Wall-clock ~30 min on the single-GPU
  puffertank container. 21 checkpoints saved.
- Final ckpt: `0000000499908608.bin`.

WebM rendering:
- `tools/render_eval_gifs.py` extended with three codec branches:
  GIF (default imageio), MP4 (libx264 + faststart for web embeds), WebM
  (libvpx-vp9 + row-mt for parallel encoding). Default switched to WebM.
- Tested at `--size 320 --crf 33`: ~45 KB/world, total 2.7 MB for 60 worlds.
  vs. previous GIF: ~450 KB/world, 28 MB total → 10× smaller.
- GIFs deleted (rm -rf inside container due to root ownership of the run
  dir's files).

Eval results on published worlds (full details in exp_tracker.md):
- 500M ckpt: **75.5% overall** on 60 published DynaBARN worlds
  (easy 100% / medium 88.5% / hard 38%).
- vs. 50M ckpt: +17.2 pp overall (medium got the biggest lift +33.5pp).
- vs. published SOTA: +44.6 pp over LfH-CP's 30.83%.

Important methodology note added to exp_tracker.md:
- PufferLib `eval` uses deterministic action means. Combined with the
  fully-deterministic DynaBARN obstacle motion (waypoints + linear
  interpolation, no noise) and fixed start/goal (0, 9) → (0, -9), every
  "trial" of the same world produces an identical trajectory.
- So "10 trials/world" is N=1 statistical theatre; per-world breakdown shows
  binary patterns (0/10 or 9/10–10/10) confirming this.
- Future evals can drop to 1 trial/world without information loss. Multi-
  trial protocol is only meaningful when there's policy or env stochasticity.

Open follow-ups (carry over plus new from 500M analysis):
- Curriculum-bin training (current setup uses easy bin throughout).
- Specifically targeted analysis of the 10 unsolvable hard worlds (40, 42,
  44, 46, 47, 49, 50, 51, 55, 56).
- L=5 history-stacked LiDAR (would help with predicting fast moving
  obstacles that cause the hard-bin collisions).
- Sim-to-sim transfer to Gazebo.

---

## 2026-05-14 — Motion-family mixture (6 families) + 500M training
**Author:** Sudhir (with Claude)
**Files touched:** shared/motion.h (new); dyna_train/dyna_train.h
                  (mixture sampler integration); dyna_train/binding.c;
                  dyna_train/dyna_train.c; dyna_train/dyna_train.ini
                  (broadened ranges + 6-family equal weights)
**Commit:** uncommitted

Motivation: the previous 500M run trained on the DynaBARN easy bin only
(poly motion, speed 0.5–1.0, std 0.01–0.1, 5–10 obstacles). Hard-bin failures
in eval were 100% on 10 of 20 hard worlds — structural failures suggesting
the policy never saw enough variety. Decision (with user): try a broader
training distribution to learn a more general "avoid moving things" skill.

New `shared/motion.h`:
- 6 motion families: `MOTION_POLY` (existing paper Alg. 1), `MOTION_LINEAR`
  (constant velocity + wall bounce), `MOTION_RECIPROCATING` (back-and-forth
  between two points), `MOTION_SINUSOIDAL` (forward + sin-perp swing),
  `MOTION_RANDOM_WALK` (velocity-noise process), `MOTION_STATIONARY`.
- Each family fills an `ObstacleTrajectory` with waypoints sampled at 1 Hz
  over 60 s — reuses existing collision / lidar / viz code unchanged.
- Mixture sampler picks a family per obstacle from integer-weight Categorical.
- Stress test: 5000 trajectories, equal weights, 100% generation success.

Env config (`dyna_train.ini`):
- Broadened ranges: `num_obstacles ∈ [3, 25]` (was 5-10), `speed ∈ [0.3,
  2.5]` (was 0.5-1.0), `order ∈ [1, 5]` (was 1-2), `std ∈ [0, 0.3]`.
- Family weights: all 6 equal.

500M training run `1778780353488` completed in ~30 min, 250k SPS, 21 ckpts.

**Important note on the eval results below: they're from the BROKEN eval
geometry (see next entry). Don't take 31.5% on the same broken eval as a
fair comparison to LfH-CP's 30.83%; user caught the eval mismatch right
after this training finished.**

---

## 2026-05-14 — Eval-geometry correction (paper protocol)
**Author:** Sudhir (with Claude)
**Files touched:** dyna_eval/dyna_eval.h, dyna_eval/dyna_eval.c,
                  dyna_eval/dyna_eval.ini, dyna_eval/binding.c,
                  run_eval_published.sh, tools/eval_three.sh (new),
                  exp_tracker.md, logger.md (this entry)
**Commit:** uncommitted

User flagged after watching the official DynaBARN demo videos: the eval
spawn is OUTSIDE the room, the room is open on the +x side, and obstacles
are already moving when the robot reaches the room mouth. Verified by
cloning kevinhou912/ROS-Jackal-Data_Collection-Local and reading
`dyna.launch` + `check_goal_node.py` directly.

Ground-truth eval geometry vs our previous (wrong) assumption:

| Field | Real | Our (broken) |
|---|---|---|
| Spawn | `(12, 0, yaw=π)` outside room, facing −x | `(0, 9, yaw=-π/2)` inside |
| Goal | `(-9, 0)` | `(0, -9)` |
| Arena walls | 3 (back, top, bottom). Open on +x. | 4 (closed box) |
| Success | `\|Δx\|<0.3 AND \|Δy\|<0.3` (L∞ box) | Euclidean `d<0.5` |
| Vel cap | `move_base max_vel_x=0.5` m/s | none (hardware 2.0) |

Patched `dyna_eval`:
- New constants: `EVAL_START_X=12, EVAL_START_Y=0, EVAL_START_TH=π`,
  `EVAL_GOAL_X=-9, EVAL_GOAL_Y=0, EVAL_VMAX_MOVEBASE=0.5`.
- New env knobs: `open_front` (1 = no +x wall), `v_max_clip` (cap policy v),
  `goal_box_half` (L∞ half-extent; 0.3 by default).
- `c_reset`: spawn at the new pose.
- `c_step`: clip v_cmd before integration; clamp y always, clamp −x always,
  clamp +x only if !open_front.
- `wall_collision`: no longer flag +x escape when open_front.
- Success criterion: L∞ box if `goal_box_half > 0`, else legacy Euclidean.
- All new fields plumbed through binding.c and dyna_eval.ini defaults to
  the corrected paper geometry.
- `run_eval_published.sh`: new `OUT_DIR_TAG` env-var, defaults to
  `eval_paper`, so corrected results don't collide with old broken ones.

Re-evaluated all three checkpoints (see exp_tracker.md for full table):

| Ckpt | Easy | Medium | Hard | Overall |
|---|---|---|---|---|
| 50M_poly  | 42.0% | 29.5% |  0.0% | 23.8% |
| 500M_poly | 58.0% | 37.0% | 11.5% | 35.5% |
| 500M_mix  | 63.0% | 44.0% |  5.0% | **37.3%** |

Best is 500M_mix at 37.3% overall — still above LfH-CP 30.83% and
Dyna-LfLH 22.5%, but dramatically lower than the broken-eval 75.5%. The
broken eval was overstating by ~38 pp.

Curious finding: 500M_mix is BETTER on easy/medium than 500M_poly, but
WORSE on hard (5% vs 11.5%). Possible explanations:
- Mix gets distracted by motion patterns the published worlds don't have.
- Mix learned to be more cautious (e.g. more reverse), exceeding time budget
  on hard worlds — but timeout rate is ~0% so this isn't time. More likely
  it's collision-on-cautious-maneuver.

Followups:
- Decide whether to also patch dyna_train to match the paper geometry (fixed
  spawn, fixed goal, 3-walled, fixed motion family if needed). See task #24
  in the task list.
- WebM-render hard-world failures for 500M_mix vs 500M_poly to find the
  collision modes.

---

## 2026-05-15 — Lower train v_max to 0.5 m/s to match paper-eval cap
**Author:** Sudhir (with Claude)
**Files touched:** dyna_train/dyna_train.h, dyna_train/dyna_train.c,
                  dyna_train/dyna_train.ini, dyna_train/binding.c,
                  exp_tracker.md, logger.md (this entry)
**Commit:** uncommitted

Decision (user): keep random start/goal in training but **cap policy linear
velocity at 0.5 m/s** to match the paper's `move_base` local planner
(`max_vel_x = 0.5`). Previous training let the policy go up to JACKAL_V_MAX
= 2.0 m/s, then eval clipped at 0.5 — the policy was learning aggressive
fast maneuvers that don't work at the slower eval speed.

Patches:
- `dyna_train.h`: new env knobs `train_v_max` (forward cap), `train_v_min`
  (reverse cap), `train_w_max` (angular cap). Mapping in `c_step`:
  `v_cmd = v_min + (a0 + 1)/2 · (v_max - v_min)`,
  `w_cmd = a1 · w_max`. Falls back to JACKAL_V_MAX / JACKAL_W_MAX if knob
  ≤ 0.
- `binding.c`: plumbs the three new kwargs through `my_init`.
- `dyna_train.ini`: `train_v_max = 0.5`, `train_v_min = -0.5`,
  `train_w_max = π` (≈ 3.14159).
- `dyna_train.c`: standalone-driver default-inits the three fields so the
  standalone build picks them up.

Sanity:
- Both python and standalone builds clean.
- Travel-time check: `min_init_goal_dist = 12 m` at v=0.5 m/s ⇒ 24 s minimum
  travel; `max_steps = 600` at dt=0.1 ⇒ 60 s episode budget. 36 s
  manoeuvre headroom. Fine.

Not retrained yet (per user: "dont stat traning yet"). Next training run
will use this config. Existing checkpoints (50M_poly, 500M_poly, 500M_mix)
were trained with v_max=2.0 and remain in-tree for comparison.

Open: when retrained, expect a different per-bin profile — the policy
should learn slow-but-precise navigation rather than fast-swerve, which
should help hard-bin numbers and avoid the "mix loses to poly on hard"
inversion we currently see.

---

## 2026-05-15 — Finite acceleration limits in both envs
**Author:** Sudhir (with Claude)
**Files touched:** dyna_train/dyna_train.{h,c,ini}, dyna_train/binding.c,
                  dyna_eval/dyna_eval.{h,c,ini}, dyna_eval/binding.c,
                  exp_tracker.md, logger.md
**Commit:** uncommitted

User pointed out: the policy could change v and w arbitrarily per step,
which is non-physical. Real Jackal under move_base has slew-rate limits
from `base_local_planner_params.yaml`:
  acc_lim_x       = 10 m/s²
  acc_lim_theta   = 20 rad/s²

Added matching slew-rate clamps in both envs. Per step, after the
unit-square→physical mapping (and after v_max_clip in eval), the commanded
v / w are constrained by:
  |v_cmd - v_prev| <= a_max     · dt    (= 1.0 m/s   at dt=0.1)
  |w_cmd - w_prev| <= alpha_max · dt    (= 2.0 rad/s at dt=0.1)

At v ∈ [-0.5, 0.5] (span 1.0), the linear cap is not binding — but the
angular one (w ∈ [-π, π], span 6.28) **is**: the policy can no longer
swing w from -π to +π in a single step.

New knobs:
  dyna_train.h: train_a_max=10, train_alpha_max=20
  dyna_eval.h:  a_max=10, alpha_max=20
Plumbed through binding.c and the .ini files. Set ≤ 0 to disable.

Existing-ckpt sanity numbers (corrected geometry + accel-capped eval):
  500M_poly:   58.0 / 37.0 / 11.5 → 57.5 / 37.5 / 5.0  (35.5 → 33.3 overall)
  500M_mix:    63.0 / 44.0 /  5.0 → 66.0 / 47.5 / 3.0  (37.3 → 38.8 overall)

Poly loses on hard (snap-maneuvers were helping); mix gains on easy/medium
(smoother control under the cap). Retraining with the cap should fix both
sides.
