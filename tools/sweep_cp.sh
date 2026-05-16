#!/bin/bash
# Sweep collision_penalty in {5, 10, 20} at 200M each.
# Same env as run rnc5gfi8 (3-scale Gaussian + no termination + costmap CNN);
# only collision_penalty differs per run.
set -e
. /puffertank/venv/bin/activate
export WANDB_API_KEY=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
export WANDB_ENTITY=sudhirpratapyadav-indian-institute-of-technology-jodhpur
cd /puffertank/pufferlib

bash build.sh dyna_train >/dev/null

for cp in 5 10 20; do
    echo
    echo "=========================================="
    echo "  collision_penalty = $cp  ($(date '+%H:%M:%S'))"
    echo "=========================================="
    python -m pufferlib.pufferl train dyna_train \
      --train.total-timesteps 200000000 \
      --env.collision-penalty $cp \
      --wandb --wandb-project dyna_barn \
      --wandb-group costmap_3gauss_cp_sweep \
      --tag "costmap_3gauss_cp${cp}_200M" 2>&1 | tail -30
done

echo
echo "Sweep done at $(date '+%H:%M:%S')."
