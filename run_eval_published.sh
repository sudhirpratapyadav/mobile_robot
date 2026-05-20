#!/bin/bash
# Run a checkpoint against the 60 published DynaBARN worlds (baked under
# external/baked_worlds/). For each world, run N_TRIALS rollouts. Aggregate
# success/collision/timeout per world and per difficulty bin.
#
# Usage (inside the container):
#   bash /puffertank/host/dyna_barn/run_eval_published.sh <ckpt.bin> [N_TRIALS] [MAX_STEPS]
#
# Defaults:
#   N_TRIALS=10   (matches DynaBARN paper: 10 trials per world)
#   MAX_STEPS=600 (60 seconds at dt=0.1)

set -e

CKPT="$1"
N_TRIALS="${2:-10}"
MAX_STEPS="${3:-600}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/cluster/env.sh"
HOST_ROOT="$DYNA_BARN_DIR"
WORLDS_DIR="$HOST_ROOT/external/baked_worlds"
CLASS_FILE="$HOST_ROOT/external/baked_worlds_classified.json"

if [ -z "$CKPT" ]; then
    echo "Usage: $0 <ckpt.bin> [N_TRIALS] [MAX_STEPS]" >&2
    exit 1
fi
if [ ! -f "$CKPT" ]; then
    echo "Checkpoint not found: $CKPT" >&2
    exit 1
fi
if [ ! -f "$CLASS_FILE" ]; then
    echo "Classification file not found: $CLASS_FILE" >&2
    echo "Run tools/classify_worlds.py first." >&2
    exit 1
fi

cd "$PUFFER_ROOT"

CKPT_NAME=$(basename "$CKPT" .bin)
RUN_DIR=$(dirname "$CKPT")
# OUT_DIR_TAG lets the caller separate eval variants (corrected vs old). Default = corrected.
OUT_DIR_TAG="${OUT_DIR_TAG:-eval_paper}"
OUT_DIR="$RUN_DIR/$OUT_DIR_TAG/$CKPT_NAME"
mkdir -p "$OUT_DIR/per_world"

echo "Checkpoint:     $CKPT"
echo "Trials/world:   $N_TRIALS"
echo "Max steps/ep:   $MAX_STEPS"
echo "Output:         $OUT_DIR"
echo

bash build.sh dyna_eval --fast >/dev/null

# Iterate worlds 000–059
SEED_BASE=10000
for idx in $(seq 0 59); do
    pad=$(printf "%03d" $idx)
    world="$WORLDS_DIR/world_${pad}.bin"
    bin="$OUT_DIR/per_world/world_${pad}.bin"
    parq="$OUT_DIR/per_world/world_${pad}.parquet"
    seed=$((SEED_BASE + idx * 100))
    ./dyna_eval --traj "$bin" --load "$CKPT" \
                --world-file "$world" \
                --episodes "$N_TRIALS" \
                --max-steps "$MAX_STEPS" \
                --world-seed-base "$seed" \
                --seed "$seed" \
                ${EXTRA_EVAL_ARGS:-} >/dev/null
    python "$HOST_ROOT/tools/bake_traj_parquet.py" "$bin" "$parq" >/dev/null
    rm -f "$bin"
    printf "  world_%s done\n" "$pad"
done

# Aggregate
python "$HOST_ROOT/tools/eval_published_summary.py" \
    --per-world-dir "$OUT_DIR/per_world" \
    --classified "$CLASS_FILE" \
    --out "$OUT_DIR/summary.json"
