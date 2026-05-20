#!/bin/bash
# Run the DynaBARN-paper eval protocol on a checkpoint:
#   3 difficulties × N_TRIALS trials each, fixed start/goal, scripted seeding.
# Default: 200 trials per difficulty (matches paper's 20 envs × 10 trials).
#
# Usage (from inside the container):
#   bash /puffertank/host/dyna_barn/run_eval.sh <ckpt.bin> [N_TRIALS] [MAX_STEPS]

set -e

CKPT="$1"
N_TRIALS="${2:-200}"
MAX_STEPS="${3:-600}"
SEED_BASE_E="${SEED_BASE_E:-1000}"
SEED_BASE_M="${SEED_BASE_M:-2000}"
SEED_BASE_H="${SEED_BASE_H:-3000}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/cluster/env.sh"
HOST_ROOT="$DYNA_BARN_DIR"

if [ -z "$CKPT" ]; then
    echo "Usage: $0 <ckpt.bin> [N_TRIALS] [MAX_STEPS]" >&2
    exit 1
fi
if [ ! -f "$CKPT" ]; then
    echo "Checkpoint not found: $CKPT" >&2
    exit 1
fi

cd "$PUFFER_ROOT"

CKPT_NAME=$(basename "$CKPT" .bin)
RUN_DIR=$(dirname "$CKPT")
OUT_DIR="$RUN_DIR/eval/$CKPT_NAME"
mkdir -p "$OUT_DIR"

echo "Eval checkpoint:  $CKPT"
echo "Trials per diff:  $N_TRIALS"
echo "Max steps/ep:     $MAX_STEPS"
echo "Output:           $OUT_DIR"
echo

bash build.sh dyna_eval --fast >/dev/null

for diff_id in 0 1 2; do
    case $diff_id in
        0) diff_name=easy; base=$SEED_BASE_E ;;
        1) diff_name=medium; base=$SEED_BASE_M ;;
        2) diff_name=hard; base=$SEED_BASE_H ;;
    esac
    bin="$OUT_DIR/${diff_name}.bin"
    parq="$OUT_DIR/${diff_name}.parquet"
    echo "=== $diff_name (difficulty=$diff_id, seed_base=$base) ==="
    ./dyna_eval --csv "$bin" --load "$CKPT" --episodes "$N_TRIALS" \
                --max-steps "$MAX_STEPS" --difficulty "$diff_id" \
                --world-seed-base "$base"
    python "$HOST_ROOT/tools/bake_traj_parquet.py" "$bin" "$parq"
    rm -f "$bin"
done

echo
echo "=== summary ==="
python "$HOST_ROOT/tools/eval_summary.py" \
    --easy "$OUT_DIR/easy.parquet" \
    --medium "$OUT_DIR/medium.parquet" \
    --hard "$OUT_DIR/hard.parquet" \
    --out "$OUT_DIR/summary.json"
