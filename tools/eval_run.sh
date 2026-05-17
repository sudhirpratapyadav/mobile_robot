#!/bin/bash
# End-to-end eval of a single trained run: train-distribution render
# (100 ep + WebMs + info.json) and paper-eval (60 worlds × 10 trials +
# WebMs + summary.json).
#
# Usage (inside container):
#   bash tools/eval_run.sh <wandb_run_id> [final_ckpt_basename] [extra-dyna_train flags...]
#
# Reads from /puffertank/host/dyna_barn/runs/train/dyna_train/<run_id>/.
# Writes to {train_dist_render,eval_paper}/ under the same run dir.
#
# Extra flags after the ckpt basename are passed to the train-distribution
# ./dyna_train invocation (e.g. --arena 20 --max-obstacles 15) so the
# train-dist render uses the same env config the policy was trained on.
# The paper-eval portion always uses dyna_eval's compiled defaults
# (eval geometry is fixed).
#
# IMPORTANT: dyna_train and dyna_eval must currently be built at the same
# HISTORY_LEN (and other compile-time obs knobs) as the ckpt was trained
# with. We don't auto-rebuild — caller is responsible.
set -e

RUN="$1"
CKPT_BN="${2:-0000000499908608.bin}"
shift 2 || true
EXTRA_FLAGS=("$@")

if [ -z "$RUN" ]; then
    echo "usage: $0 <wandb_run_id> [final_ckpt_basename] [extra dyna_train flags...]" >&2
    exit 1
fi

HOST=/puffertank/host/dyna_barn
RUN_DIR="$HOST/runs/train/dyna_train/$RUN"
CKPT="$RUN_DIR/$CKPT_BN"

if [ ! -f "$CKPT" ]; then
    echo "ckpt not found: $CKPT" >&2
    exit 1
fi

cd /puffertank/pufferlib
source /puffertank/venv/bin/activate

echo "=== train-dist (100 ep) ==="
TD="$RUN_DIR/train_dist_render"
mkdir -p "$TD"
./dyna_train --traj "$TD/100ep.bin" --load "$CKPT" --episodes 100 --seed 42 "${EXTRA_FLAGS[@]}"
python "$HOST/tools/bake_traj_parquet.py" "$TD/100ep.bin" "$TD/100ep.parquet"
# Derive arena-half from --arena if passed; else fall back to renderer
# default (12 = arena 24). Using python instead of `bc` (not in container).
ARENA_HALF=12.0
for ((i=0; i<${#EXTRA_FLAGS[@]}; i++)); do
    if [ "${EXTRA_FLAGS[$i]}" = "--arena" ]; then
        ARENA_HALF=$(python -c "print(${EXTRA_FLAGS[$((i+1))]} / 2.0)")
    fi
done
python "$HOST/tools/render_train_dist_webms.py" \
    --parquet "$TD/100ep.parquet" \
    --out-dir "$TD/webms" \
    --workers 24 \
    --arena-half "$ARENA_HALF"

echo
echo "=== paper eval (60 × 10) ==="
bash "$HOST/run_eval_published.sh" "$CKPT" 10 600

CKPT_STEM=$(basename "$CKPT" .bin)
PER_WORLD_DIR="$RUN_DIR/eval_paper/$CKPT_STEM/per_world"
PAPER_WEBMS="$RUN_DIR/eval_paper/$CKPT_STEM/webms"
mkdir -p "$PAPER_WEBMS"
python "$HOST/tools/render_eval_gifs.py" \
    --per-world-dir "$PER_WORLD_DIR" \
    --classified    "$HOST/external/baked_worlds_classified.json" \
    --out-dir       "$PAPER_WEBMS" \
    --workers 24

echo
echo "=== summary ==="
python -c "
import json
d = json.loads(open('$RUN_DIR/eval_paper/$CKPT_STEM/summary.json').read())
print(f'PAPER:   {d[\"overall\"]}')
for k, v in d['by_difficulty'].items():
    print(f'  {k:8s} {v[\"success\"]}/{v[\"n\"]} ({100*v[\"success\"]/v[\"n\"]:.1f}%)  coll={v[\"collision\"]}  to={v[\"timeout\"]}')
print()
td = json.loads(open('$TD/webms/info.json').read())['summary']
print(f'TRAIN-DIST: {td[\"n_episodes\"]} ep verdict_counts={td[\"verdict_counts\"]}')
"
