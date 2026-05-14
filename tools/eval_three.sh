#!/bin/bash
# Re-evaluate the three trained checkpoints against the corrected DynaBARN
# paper geometry (spawn (12,0,π), goal (-9,0), 3-walled, v_max=0.5).
set -e
cd /puffertank/host/dyna_barn
. /puffertank/venv/bin/activate

CKPTS=(
  runs/train/dyna_train/1778772096469/0000000049807360.bin    # 50M poly easy-bin
  runs/train/dyna_train/1778776723464/0000000499908608.bin    # 500M poly easy-bin
  runs/train/dyna_train/1778780353488/0000000499908608.bin    # 500M motion mix
)
NAMES=(50M_poly 500M_poly 500M_mix)
for i in 0 1 2; do
  CKPT="${CKPTS[$i]}"
  echo
  echo "=========================================="
  echo "  ${NAMES[$i]} : $CKPT"
  echo "=========================================="
  bash /puffertank/host/dyna_barn/run_eval_published.sh \
       "/puffertank/host/dyna_barn/$CKPT" 10 600 2>&1 | tail -12
done
