# cluster/env.sh — single source of truth for paths.
# Source this at the top of any script that needs DYNA_BARN_DIR / PUFFER_ROOT.
# Defaults match the docker layout we developed against; override per-env via
# environment variables (e.g. on the SLURM cluster) before sourcing.
#
# Cluster (SLURM dgx2) usage:
#   export DYNA_BARN_DIR=$HOME/sudhir/mobile_robot/dyna_barn
#   export PUFFER_ROOT=$HOME/sudhir/mobile_robot/pufferlib
#   export VENV_DIR=$PUFFER_ROOT/.venv      # uv-managed
#   source $DYNA_BARN_DIR/cluster/env.sh
#
# Docker (existing) usage: defaults below are correct; no env needed.

: "${DYNA_BARN_DIR:=/puffertank/host/dyna_barn}"
: "${PUFFER_ROOT:=/puffertank/pufferlib}"
: "${VENV_DIR:=/puffertank/venv}"

export DYNA_BARN_DIR PUFFER_ROOT VENV_DIR

# Convenience: derived paths.
export EVAL_BIN="$PUFFER_ROOT/dyna_eval"
export TRAIN_BIN="$PUFFER_ROOT/dyna_train"

# On iHub cluster nodes: load gnu12 (gcc 12.2) since the system gcc 8.5
# is too old for pufferlib's C++17 designated-initializer usage. Also
# expose nvcc on PATH. No-op when /etc/profile.d/lmod.sh isn't present
# (docker).
if [ -f /etc/profile.d/lmod.sh ] && [ -z "$LMOD_DIR_SOURCED" ]; then
    . /etc/profile.d/lmod.sh
    module load gnu12 2>/dev/null || true
    export LMOD_DIR_SOURCED=1
fi
[ -d /usr/local/cuda/bin ] && case ":$PATH:" in
    *:/usr/local/cuda/bin:*) ;;
    *) export PATH="/usr/local/cuda/bin:$PATH" ;;
esac

# Activate the venv if available (some scripts assume it's already active).
if [ -z "$VIRTUAL_ENV" ] && [ -f "$VENV_DIR/bin/activate" ]; then
    # shellcheck disable=SC1090
    . "$VENV_DIR/bin/activate"
fi
