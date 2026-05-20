#!/bin/bash
# End-to-end eval of a single trained run: train-distribution render
# (100 ep + WebMs + info.json) and paper-eval (60 worlds × 10 trials +
# WebMs + summary.json).
#
# Usage (inside container):
#   bash tools/eval_run.sh <wandb_run_id> [final_ckpt_basename] [extra-dyna_train flags...]
#   # Override paper-eval rotation (default = 90, open wall on +y):
#   ROTATION=0  bash tools/eval_run.sh ...   # original +x→−x corridor
#   ROTATION=90 bash tools/eval_run.sh ...   # rotated, open wall on top (default)
#
# Reads from /puffertank/host/dyna_barn/runs/train/dyna_train/<run_id>/.
# Writes to {train_dist_render, eval_paper_rot${ROTATION}}/ under the
# same run dir (or eval_paper/ when ROTATION=0 for back-compat).
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

# Source central path config (defaults match docker; cluster sets env vars).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/../cluster/env.sh"

HOST="$DYNA_BARN_DIR"
RUN_DIR="$HOST/runs/train/dyna_train/$RUN"
CKPT="$RUN_DIR/$CKPT_BN"

if [ ! -f "$CKPT" ]; then
    echo "ckpt not found: $CKPT" >&2
    exit 1
fi

cd "$PUFFER_ROOT"

echo "=== train-dist (100 ep) ==="
TD="$RUN_DIR/train_dist_render"
mkdir -p "$TD"
./dyna_train --ini "$DYNA_BARN_DIR/dyna_train/dyna_train.ini" \
    --traj "$TD/100ep.bin" --load "$CKPT" --episodes 100 --seed 42 "${EXTRA_FLAGS[@]}"
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
echo "=== paper eval (60 × 10, rotation=90) ==="
# Default to the rotated paper-eval (open wall on +y). Callers that want
# the original rot=0 setup can set ROTATION=0 explicitly.
EVAL_ROTATION="${ROTATION:-90}"
if [ "$EVAL_ROTATION" = "0" ]; then
    EVAL_TAG="eval_paper"
else
    EVAL_TAG="eval_paper_rot${EVAL_ROTATION}"
fi
EXTRA_EVAL_ARGS="--rotation ${EVAL_ROTATION}" OUT_DIR_TAG="$EVAL_TAG" \
    bash "$HOST/run_eval_published.sh" "$CKPT" 10 600

CKPT_STEM=$(basename "$CKPT" .bin)
PER_WORLD_DIR="$RUN_DIR/$EVAL_TAG/$CKPT_STEM/per_world"
PAPER_WEBMS="$RUN_DIR/$EVAL_TAG/$CKPT_STEM/webms"
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
d = json.loads(open('$RUN_DIR/$EVAL_TAG/$CKPT_STEM/summary.json').read())
print(f'PAPER:   {d[\"overall\"]}')
for k, v in d['by_difficulty'].items():
    print(f'  {k:8s} {v[\"success\"]}/{v[\"n\"]} ({100*v[\"success\"]/v[\"n\"]:.1f}%)  coll={v[\"collision\"]}  to={v[\"timeout\"]}')
print()
td = json.loads(open('$TD/webms/info.json').read())['summary']
print(f'TRAIN-DIST: {td[\"n_episodes\"]} ep verdict_counts={td[\"verdict_counts\"]}')
"
