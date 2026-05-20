#!/bin/bash
# End-to-end: build → baselines → train (TIMESTEPS env-var) → eval each checkpoint.
# Run inside the container:
#   bash /puffertank/host/dyna_barn/run_pipeline.sh
# Optional env-vars:
#   TIMESTEPS=10000000   # override .ini total_timesteps for smoke runs
#   SKIP_TRAIN=1         # skip training (just baselines + eval the latest run)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/cluster/env.sh"
HOST_ROOT="$DYNA_BARN_DIR"
ENV_TRAIN=dyna_train
ENV_EVAL=dyna_eval

cd "$PUFFER_ROOT"

mkdir -p "$HOST_ROOT/runs/train" "$HOST_ROOT/runs/eval"

echo "=== build (python, standalones) ==="
bash build.sh "$ENV_TRAIN" >/dev/null
bash build.sh "$ENV_TRAIN" --fast >/dev/null
bash build.sh "$ENV_EVAL" --fast >/dev/null

echo "=== baseline rollouts on dyna_eval (easy/medium/hard, random actions) ==="
mkdir -p "$HOST_ROOT/runs/eval/baseline_random"
for diff in 0 1 2; do
    case $diff in 0) tag=easy;; 1) tag=medium;; 2) tag=hard;; esac
    bin="$HOST_ROOT/runs/eval/baseline_random/${tag}.bin"
    parq="$HOST_ROOT/runs/eval/baseline_random/${tag}.parquet"
    ./dyna_eval --csv "$bin" --mode random --difficulty "$diff" \
                --episodes 200 --max-steps 600 --world-seed-base $((100 + diff)) >/dev/null
    python "$HOST_ROOT/tools/bake_traj_parquet.py" "$bin" "$parq" >/dev/null
    rm -f "$bin"
done
python "$HOST_ROOT/tools/eval_summary.py" \
    --easy "$HOST_ROOT/runs/eval/baseline_random/easy.parquet" \
    --medium "$HOST_ROOT/runs/eval/baseline_random/medium.parquet" \
    --hard "$HOST_ROOT/runs/eval/baseline_random/hard.parquet" \
    --out "$HOST_ROOT/runs/eval/baseline_random/summary.json"

if [ -z "$SKIP_TRAIN" ]; then
    echo
    echo "=== train ==="
    # Need to rebuild python static lib in case it was overwritten by eval build.
    bash build.sh "$ENV_TRAIN" >/dev/null
    if [ -n "$TIMESTEPS" ]; then
        python -m pufferlib.pufferl train "$ENV_TRAIN" --train.total-timesteps "$TIMESTEPS"
    else
        python -m pufferlib.pufferl train "$ENV_TRAIN"
    fi
fi

# Find latest run dir
RUN_DIR=$(ls -1dt "$HOST_ROOT/runs/train/$ENV_TRAIN"/*/ 2>/dev/null | head -1)
if [ -z "$RUN_DIR" ]; then
    echo "No training run dir found under $HOST_ROOT/runs/train/$ENV_TRAIN/"
    exit 1
fi
RUN_DIR=${RUN_DIR%/}
echo
echo "Run dir: $RUN_DIR"
CKPT=$(ls -1 "$RUN_DIR"/*.bin 2>/dev/null | sort | tail -1)
if [ -z "$CKPT" ]; then
    echo "No .bin checkpoints in $RUN_DIR"
    exit 1
fi

echo
echo "=== eval latest checkpoint ==="
bash "$HOST_ROOT/run_eval.sh" "$CKPT" 200 600
