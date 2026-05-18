# Project purpose & strategy

## Goal

Train a **generalist** mobile-robot navigation policy that handles a broad
distribution of dynamic-obstacle scenarios. Generality is the target, not
any single benchmark.

## How we measure progress

Two metrics:

1. **`clean_reach` on the training distribution** — reach goal AND zero
   collisions before first reach. This is our **primary** optimisation
   target.
2. **`success` on the paper eval** (60 published DynaBARN worlds) — a
   useful **byproduct** sanity check: a generalist should eventually
   handle this distribution too. But paper-eval is **not** what we tune
   against. We do NOT train on the eval distribution.

## Strategic principle (decided 2026-05-18)

Make training broader and harder; push training-distribution
`clean_reach` up; trust that generality will eventually translate to the
held-out paper eval. If a variant has higher train-dist `clean_reach`
than the current best, that is a real win — even if its paper-eval is
lower right now.

This inverts the lens we were using earlier today (where every paper
regression killed a variant). From here on, **train-dist `clean_reach`
is the lever**.

## What this rules out

- **No training on the paper-eval distribution.** Procedural worlds only.
- **No eval-distribution-matched env tweaks.** e.g. shrinking the training
  arena to match the 20 m eval arena (run `7m5b0pm6`) was paper-driven —
  the right ask is "does a more open training arena make a better
  generalist?", not "does it match eval?"
- **No paper-eval-driven ckpt selection.** Pick ckpts on train-dist
  metrics.

## What this means in practice

- Sweep things that make training **harder or broader** (more obstacles,
  more motion-family variety, wider speed range, wider arena, harder
  goal box, etc.).
- Keep paper-eval running for visibility but don't let it gate
  decisions.
- When a variant lifts train-dist `clean_reach`, double down on the
  axis.

## Re-reading prior results under this lens

Several variants previously called "regressed" had **TD-clean ≥
e7sadb3v** but lower paper. Under the new lens, those are now
candidates worth a second look:

| Run | TD-clean | Paper | Old verdict | New lens |
|---|---|---|---|---|
| e7sadb3v | 51 % | 44.7 % | baseline | — |
| 40qb3qf0 (arena=40) | **80 %** | 26.0 % | regressed | **promising** — biggest TD-clean ever |
| 9ypq3hyf (slow obs) | 57 % | 18.7 % | regressed | mild TD gain |
| pnt5yya4 (β=20) | 56 % | 15.3 % | regressed | mild TD gain |
| 7m5b0pm6 (arena=20) | 53 % | 36.8 % | regressed | mild TD gain |
| 4dabm514 (succ+coll bonus) | 52 % | 26.7 % | regressed | flat TD |

`40qb3qf0` arena=40 is the strongest candidate by this metric. It
likely needs a broader training distribution that retains the open-arena
clean_reach while also being harder.

## Day 3+ directions (under this lens)

1. **arena=40 baseline + more obstacles / wider variety** — keep the TD-
   clean win but add training-difficulty.
2. **Motion-family broadening**: re-enable `reciprocating` and
   `random_walk` families that were zeroed in `dyna_train.ini`.
3. **Harder goal box** (0.3 → 0.2 → 0.1) — stricter reach criterion.
4. **Wider obstacle radius variation** (currently fixed at 0.5 m).
5. **Architectural** (CNN/transformer, RNN beyond MinGRU). Blocked on
   building a Python paper-eval (task #89) so we can actually score
   them; but TD-clean is computable today via standard rollout, so
   architectural runs can be ranked on TD-clean even before paper-eval
   infra lands.
