#!/bin/bash
# cluster/install.sh — one-shot setup on the iHub SLURM cluster (dgx1/dgx2).
#
# Run from a compute node (inside the holder allocation):
#   srun --jobid=$HOLDER --overlap -n1 bash ~/sudhir/mobile_robot/dyna_barn/cluster/install.sh
#
# What it does:
#   1. Clones PufferLib at the pinned commit + applies our 3-file patch.
#   2. Builds raylib 5.0 under ~/.local (used by dyna_eval at link time).
#   3. Creates a uv-managed Python 3.12 venv and installs PufferLib + deps.
#   4. Symlinks PufferLib config/{dyna_train,dyna_eval}.ini → our repo.
#   5. Compiles dyna_train, dyna_eval, and the PufferLib _C.so.
#
# Idempotent: skips steps whose outputs already exist.

set -euo pipefail

# --- Cluster paths ---
MOBILE_ROOT="$HOME/sudhir/mobile_robot"
DYNA_BARN_DIR="$MOBILE_ROOT/dyna_barn"
PUFFER_ROOT="$MOBILE_ROOT/pufferlib"
RAYLIB_PREFIX="$HOME/.local"
PUFFER_COMMIT="d21a161a40094ec7ba7578e9cf8779b73f6c218c"
PUFFER_REMOTE="https://github.com/pufferai/pufferlib"
RAYLIB_VER="5.0"

# --- 0. Toolchain check ---
echo "== toolchain =="
export PATH="/usr/local/cuda/bin:$PATH"
which gcc nvcc uv >/dev/null || { echo "missing gcc/nvcc/uv on PATH"; exit 1; }
gcc --version | head -1
nvcc --version | tail -2
uv --version

# --- 1. PufferLib clone at pinned commit ---
if [ ! -d "$PUFFER_ROOT/.git" ]; then
    echo "== cloning pufferlib =="
    git clone "$PUFFER_REMOTE" "$PUFFER_ROOT"
fi
cd "$PUFFER_ROOT"
echo "== checking out $PUFFER_COMMIT =="
git fetch origin "$PUFFER_COMMIT" 2>/dev/null || true
git checkout "$PUFFER_COMMIT"

# Apply our 3 patches (idempotent — check for our marker).
PATCH="$DYNA_BARN_DIR/cluster/pufferlib.diff"
if [ -f "$PATCH" ]; then
    if grep -q "CostmapEncoder64" "$PUFFER_ROOT/pufferlib/models.py" 2>/dev/null; then
        echo "== pufferlib patches already applied =="
    else
        echo "== applying pufferlib patches =="
        git apply --3way "$PATCH"
    fi
fi

# Symlink configs into pufferlib/config so `pufferl train dyna_train` finds them.
for f in dyna_train.ini dyna_eval.ini; do
    src="$DYNA_BARN_DIR/dyna_train/dyna_train.ini"
    [ "$f" = "dyna_eval.ini" ] && src="$DYNA_BARN_DIR/cluster/dyna_eval.ini"
    dst="$PUFFER_ROOT/config/$f"
    [ -L "$dst" ] && rm -f "$dst"
    ln -sf "$src" "$dst"
    echo "  linked $dst -> $src"
done

# --- 2. raylib (only needed for dyna_eval link step) ---
if [ ! -f "$RAYLIB_PREFIX/lib/libraylib.a" ]; then
    echo "== building raylib $RAYLIB_VER =="
    cd /tmp
    rm -rf raylib-$RAYLIB_VER
    curl -sL "https://github.com/raysan5/raylib/archive/refs/tags/$RAYLIB_VER.tar.gz" | tar xz
    cd "raylib-$RAYLIB_VER/src"
    make PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC \
         RAYLIB_RELEASE_PATH="$RAYLIB_PREFIX/lib" -j8
    mkdir -p "$RAYLIB_PREFIX/include" "$RAYLIB_PREFIX/lib"
    cp libraylib.a "$RAYLIB_PREFIX/lib/"
    cp raylib.h rlgl.h raymath.h "$RAYLIB_PREFIX/include/"
    echo "raylib installed to $RAYLIB_PREFIX"
fi

# --- 3. uv venv + PufferLib install ---
cd "$PUFFER_ROOT"
if [ ! -d ".venv" ]; then
    echo "== creating uv venv =="
    uv venv --python 3.12 .venv
fi
# shellcheck disable=SC1091
. .venv/bin/activate
echo "== installing pufferlib + deps =="
# Use the editable install so our patches stay live.
uv pip install -e . wandb pyarrow opencv-python-headless tyro pandas numpy

# --- 4. Build dyna_train + dyna_eval ---
cd "$PUFFER_ROOT"
echo "== building dyna_train (standalone + _C.so) =="
RAYLIB_PREFIX="$RAYLIB_PREFIX" bash build.sh dyna_train --fast
RAYLIB_PREFIX="$RAYLIB_PREFIX" bash build.sh dyna_train
echo "== building dyna_eval =="
RAYLIB_PREFIX="$RAYLIB_PREFIX" bash build.sh dyna_eval --fast

echo
echo "== install complete =="
echo "Activate venv:   . $PUFFER_ROOT/.venv/bin/activate"
echo "Source paths:    . $DYNA_BARN_DIR/cluster/env.sh"
echo
echo "Smoke test:"
echo "  cd $PUFFER_ROOT && ./dyna_eval --traj /tmp/smoke.bin --episodes 1 --max-steps 50 --seed 42"
