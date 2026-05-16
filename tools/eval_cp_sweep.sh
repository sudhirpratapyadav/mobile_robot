#!/bin/bash
# Eval the final ckpts of the cp-sweep on the 60 published worlds.
set -e
cd /puffertank/host/dyna_barn
. /puffertank/venv/bin/activate

CKPTS=(
  "runs/train/dyna_train/oacjl4k8/0000000199753728.bin   cp=5"
  "runs/train/dyna_train/1r7yon1c/0000000199753728.bin   cp=10"
)
for entry in "${CKPTS[@]}"; do
  CKPT=$(echo $entry | awk '{print $1}')
  TAG=$(echo $entry | awk '{print $2}')
  echo
  echo "=========================================="
  echo "  $TAG : $(basename $CKPT)"
  echo "=========================================="
  bash /puffertank/host/dyna_barn/run_eval_published.sh \
       "/puffertank/host/dyna_barn/$CKPT" 10 600 2>&1 | tail -10
done
