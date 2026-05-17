#!/bin/bash
# Run one architectural experiment end-to-end.
#
# Sets HISTORY_LEN in both dyna_train.h and dyna_eval.h to the requested value,
# rebuilds standalones + .so, launches training on the requested GPU
# (single GPU only — use CUDA_VISIBLE_DEVICES), waits, then evals
# (train-dist render + paper-eval + paper render) into the run dir.
#
# Usage (inside container):
#   bash tools/run_experiment.sh <gpu_id> <history_len> <wandb_group> [TRAIN_STEPS] [extra puffer flags...]
#
# Example:
#   bash tools/run_experiment.sh 0 2 hist2_beta10 500000000 --env.beta 10.0
#
# Requires: WANDB_API_KEY, WANDB_ENTITY env-vars (or wandb_v1 in .netrc).
set -e

GPU="$1"
HISTORY_LEN="$2"
WANDB_GROUP="$3"
TRAIN_STEPS="${4:-500000000}"
shift 4 || true
EXTRA_FLAGS=("$@")

if [ -z "$GPU" ] || [ -z "$HISTORY_LEN" ] || [ -z "$WANDB_GROUP" ]; then
    echo "usage: $0 <gpu_id> <history_len> <wandb_group> [TRAIN_STEPS] [extra puffer flags...]" >&2
    exit 1
fi

HOST=/puffertank/host/dyna_barn
PUFFER=/puffertank/pufferlib

# Sync HISTORY_LEN in both env headers.
sed -i "s|^#  define HISTORY_LEN .*$|#  define HISTORY_LEN ${HISTORY_LEN}|" \
    "$HOST/dyna_train/dyna_train.h" \
    "$HOST/dyna_eval/dyna_eval.h"

cd "$PUFFER"
source /puffertank/venv/bin/activate

echo "=== build (HISTORY_LEN=$HISTORY_LEN) ==="
bash build.sh dyna_train --fast >/dev/null
bash build.sh dyna_eval --fast >/dev/null
bash build.sh dyna_train >/dev/null
echo "  built."

echo
echo "=== train (GPU $GPU, $TRAIN_STEPS steps, group=$WANDB_GROUP) ==="
CUDA_VISIBLE_DEVICES="$GPU" python -m pufferlib.pufferl train dyna_train \
    --wandb --wandb-project dyna_barn --wandb-group "$WANDB_GROUP" \
    --train.total-timesteps "$TRAIN_STEPS" \
    "${EXTRA_FLAGS[@]}" 2>&1

# Find the most recent run dir matching this group (via wandb run id in stdout).
RUN_ID=$(ls -1dt "$HOST/runs/train/dyna_train"/*/ | head -1 | xargs basename)
RUN_DIR="$HOST/runs/train/dyna_train/$RUN_ID"
CKPT=$(ls -1 "$RUN_DIR"/*.bin | sort | tail -1)
CKPT_STEM=$(basename "$CKPT" .bin)

echo
echo "=== eval (run_id=$RUN_ID, ckpt=$CKPT_STEM) ==="
bash "$HOST/tools/eval_run.sh" "$RUN_ID" "$(basename "$CKPT")"

echo
echo "DONE. run_id=$RUN_ID"
