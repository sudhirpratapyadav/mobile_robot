# Cluster (iHub SLURM) workflow

## Initial setup (one-time)

```bash
# On dgx2 (compute node, inside a holder allocation):
cd ~/sudhir/mobile_robot
git clone git@github.com:sudhirpratapyadav/mobile_robot.git dyna_barn
srun --jobid=$HOLDER --overlap -n1 \
     bash ~/sudhir/mobile_robot/dyna_barn/cluster/install.sh
```

`install.sh` clones PufferLib at the pinned commit, applies our patch,
builds raylib under `~/.local`, creates a uv venv (Python 3.12), and
compiles `dyna_train`/`dyna_eval`.

## Holder pattern (use existing or create)

Either reuse your existing holder (`squeue -u $USER`) or grab a new one:

```bash
sbatch ~/sudhir/manipulation/slurm/holder_8gpu.sh   # 20-day, 8 GPU
HOLDER=$(squeue -u $USER -h -o "%i" -n holder_8gpu)
```

## Every shell

```bash
export DYNA_BARN_DIR=$HOME/sudhir/mobile_robot/dyna_barn
export PUFFER_ROOT=$HOME/sudhir/mobile_robot/pufferlib
export VENV_DIR=$PUFFER_ROOT/.venv
export PATH=/usr/local/cuda/bin:$PATH
source $DYNA_BARN_DIR/cluster/env.sh   # activates the venv
```

## Launch a training step inside the holder

```bash
CUDA_VISIBLE_DEVICES=0 srun --jobid=$HOLDER --gres=gpu:1 -n1 --exclusive \
    bash -c '
      cd $PUFFER_ROOT
      python -m pufferlib.pufferl train dyna_train \
        --wandb --wandb-project dyna_barn --wandb-group king_reproduce \
        --train.total-timesteps 500000000 \
        --env.beta 10.0 --env.arena-size 40.0 \
        --env.num-obstacles-min 8 --env.num-obstacles-max 30 \
        --env.speed-min 0.5 --env.speed-max 5.0
    ' &
```

Use `CUDA_VISIBLE_DEVICES=N` to pick GPU; `--gres=gpu:1` reserves 1 GPU
from the holder's pool. Run multiple in parallel by re-issuing with
different GPU indices.

## Paper-eval

```bash
ROTATION=90 bash $DYNA_BARN_DIR/tools/eval_run.sh \
    <wandb_run_id> 0000000499908608.bin \
    --arena 40 --min-obstacles 8 --max-obstacles 30 \
    --min-speed 0.5 --max-speed 5.0
```

Output lands in `$DYNA_BARN_DIR/runs/train/dyna_train/<rid>/eval_paper_rot90/`.

## W&B

Same project (`dyna_barn`) and entity
(`sudhirpratapyadav-indian-institute-of-technology-jodhpur`) as the
docker setup. The wandb API key is in `cluster/env.sh.local` (not
committed); export it before launching training:

```bash
export WANDB_API_KEY=...
```
