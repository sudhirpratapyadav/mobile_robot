#!/bin/bash
set -e
cd /puffertank/host/dyna_barn
. /puffertank/venv/bin/activate
CKPTS=(
  runs/train/dyna_train/v6r7zeja/0000000000262144.bin
  runs/train/dyna_train/v6r7zeja/0000000105119744.bin
  runs/train/dyna_train/v6r7zeja/0000000236191744.bin
  runs/train/dyna_train/v6r7zeja/0000000367263744.bin
  runs/train/dyna_train/v6r7zeja/0000000498335744.bin
)
NAMES=(0M 100M 240M 370M 500M)
for i in 0 1 2 3 4; do
  CKPT="${CKPTS[$i]}"
  echo
  echo "=========================================="
  echo "  ${NAMES[$i]} : $(basename $CKPT)"
  echo "=========================================="
  bash /puffertank/host/dyna_barn/run_eval_published.sh \
       "/puffertank/host/dyna_barn/$CKPT" 2 600 2>&1 | tail -10
done
