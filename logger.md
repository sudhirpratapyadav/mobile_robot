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
