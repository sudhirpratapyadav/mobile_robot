#!/bin/bash
# Eval a sequence of checkpoints from the same training run on the 60
# published DynaBARN worlds (corrected paper geometry + accel limits).
set -e
cd /puffertank/host/dyna_barn
. /puffertank/venv/bin/activate

CKPTS=(
  runs/train/dyna_train/b3vm6fej/0000000000262144.bin
  runs/train/dyna_train/b3vm6fej/0000001232338944.bin
  runs/train/dyna_train/b3vm6fej/0000002490630144.bin
  runs/train/dyna_train/b3vm6fej/0000003748921344.bin
  runs/train/dyna_train/b3vm6fej/0000004999872512.bin
)
NAMES=(0M 1.2B 2.5B 3.75B 5B)
for i in 0 1 2 3 4; do
  CKPT="${CKPTS[$i]}"
  echo
  echo "=========================================="
  echo "  ${NAMES[$i]} : $(basename $CKPT)"
  echo "=========================================="
  bash /puffertank/host/dyna_barn/run_eval_published.sh \
       "/puffertank/host/dyna_barn/$CKPT" 10 600 2>&1 | tail -10
done
