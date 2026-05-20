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
# System gcc is 8.5 which doesn't accept C99-style designated initializers
# in C++17 mode (pufferlib's tensor init code). Load gcc-12.2 from the
# OHPC module system.
if [ -f /etc/profile.d/lmod.sh ]; then
    . /etc/profile.d/lmod.sh
    module load gnu12 2>/dev/null || true
fi
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

# Symlink env source dirs into pufferlib/ocean/ (matches docker setup_container.sh).
for env in dyna_train dyna_eval; do
    src="$DYNA_BARN_DIR/$env"
    dst="$PUFFER_ROOT/ocean/$env"
    [ -e "$dst" ] && rm -rf "$dst"
    ln -sfn "$src" "$dst"
    echo "  linked $dst -> $src"
done

# Symlink configs into pufferlib/config so `pufferl train dyna_train` finds them.
for f in dyna_train.ini dyna_eval.ini; do
    src="$DYNA_BARN_DIR/dyna_train/dyna_train.ini"
    [ "$f" = "dyna_eval.ini" ] && src="$DYNA_BARN_DIR/cluster/dyna_eval.ini"
    dst="$PUFFER_ROOT/config/$f"
    [ -L "$dst" ] && rm -f "$dst"
    ln -sf "$src" "$dst"
    echo "  linked $dst -> $src"
done

# --- 2. raylib ---
# PufferLib's build.sh downloads a prebuilt raylib-5.5_linux_amd64 binary
# tarball on first run, so we don't need to build it locally.
# dyna_eval.c is built with -DDYNA_HEADLESS on the cluster to skip the
# interactive raylib code paths anyway (no X11 / no DISPLAY).
echo "== raylib will be fetched by pufferlib build.sh; using DYNA_HEADLESS =="

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
uv pip install -e . wandb pyarrow opencv-python-headless tyro pandas numpy polars imageio imageio-ffmpeg

# --- 4. Build dyna_train (standalone + _C.so via PufferLib) ---
# Cluster nodes have gcc not clang. PufferLib's build.sh respects $CC for
# the compiler, but uses clang-specific flags (-ferror-limit, the various
# -Wno-error=*-discards-qualifiers) in CLANG_WARN. Patch them out
# idempotently for the cluster path; the resulting build still warns
# loudly but doesn't fail.
export CC=gcc
cd "$PUFFER_ROOT"

if ! grep -q "# CLUSTER PATCHED" build.sh; then
    echo "== patching build.sh CLANG_WARN for gcc compatibility =="
    # Strip clang-only flags + add -ldl/-lrt for raylib's runtime
    # dynamic loading.
    sed -i.bak \
        -e 's/-ferror-limit=3//g' \
        -e 's/-Wno-error=incompatible-pointer-types-discards-qualifiers//g' \
        -e 's/-Wno-incompatible-pointer-types-discards-qualifiers//g' \
        -e 's/-Wno-deprecated-declarations//g' \
        -e 's/-Wno-error=array-parameter//g' \
        -e 's/-Werror=incompatible-pointer-types//g' \
        -e 's/STANDALONE_LDFLAGS=(-lGL)/STANDALONE_LDFLAGS=(-lGL -ldl -lrt)/' \
        -e 's|NVCC="ccache \(.*\)"|NVCC="\1"|' \
        -e 's/OMP_LIB=-lomp5/OMP_LIB=-lomp/' \
        build.sh

    # Inject NCCL_IFLAG / NCCL_LFLAG (build.sh assumed system-installed nccl)
    # using nvidia-nccl-cu12 wheel paths from the venv.
    NCCL_PY='python -c "import nvidia.nccl, os; print(os.path.join(nvidia.nccl.__path__[0]))"'
    # Insert detection block after the CUDNN block (idempotent — guarded by
    # the marker check above).
    awk -v py="$NCCL_PY" '
        /^if \[ -z "\$CUDNN_LFLAG" \]; then$/ { inblock=1 }
        inblock && /^fi$/ {
            print
            print ""
            print "NCCL_IFLAG=$(" py " 2>/dev/null | xargs -I{} echo -I{}/include)"
            print "NCCL_LFLAG=$(" py " 2>/dev/null | xargs -I{} echo -L{}/lib)"
            inblock=0
            next
        }
        { print }
    ' build.sh > build.sh.tmp && mv build.sh.tmp build.sh
    chmod +x build.sh

    # Add NCCL flags to the include + link lines (cudnn block already pattern).
    sed -i \
        -e 's|-I\$CUDA_HOME/include \$CUDNN_IFLAG|-I$CUDA_HOME/include $CUDNN_IFLAG $NCCL_IFLAG|g' \
        -e 's|-L\$CUDA_HOME/lib64 \$CUDNN_LFLAG|-L$CUDA_HOME/lib64 $CUDNN_LFLAG $NCCL_LFLAG|g' \
        build.sh

    echo "# CLUSTER PATCHED" >> build.sh
fi

echo "== building dyna_train (standalone + _C.so) with CC=$CC =="
# Clear any stale build artifacts so we don't link gcc-8.5 objects with
# gcc-12.2-built ones.
rm -rf "$PUFFER_ROOT/build" "$PUFFER_ROOT/dyna_train" "$PUFFER_ROOT/dyna_eval"
rm -f "$PUFFER_ROOT/pufferlib/_C.cpython"-*.so

# nvidia-{nccl,cudnn}-cu12 wheels only ship versioned .so (e.g. libnccl.so.2,
# libcudnn.so.9). Linker -lnccl needs libnccl.so. Create unversioned symlinks.
for pkg in nccl cudnn; do
    libdir="$PUFFER_ROOT/.venv/lib/python3.12/site-packages/nvidia/$pkg/lib"
    if [ -d "$libdir" ]; then
        for so in "$libdir"/lib*.so.*; do
            [ -f "$so" ] || continue
            unversioned="$(echo "$so" | sed 's|\.so\..*$|.so|')"
            [ -e "$unversioned" ] || ln -sf "$(basename "$so")" "$unversioned"
        done
    fi
done

# Also make those lib dirs visible to ld at runtime.
export LD_LIBRARY_PATH="$PUFFER_ROOT/.venv/lib/python3.12/site-packages/nvidia/nccl/lib:$PUFFER_ROOT/.venv/lib/python3.12/site-packages/nvidia/cudnn/lib:${LD_LIBRARY_PATH:-}"

bash build.sh dyna_train --fast
bash build.sh dyna_train

# --- 5. Build dyna_eval headless (no raylib needed for --traj mode) ---
# We bypass pufferlib's build.sh for dyna_eval because the prebuilt raylib
# binary links against X11/GLFW/Xcursor which dgx2 doesn't have. With
# -DDYNA_HEADLESS the interactive raylib code paths are skipped and the
# binary only supports --traj (trajectory-write) mode, which is what
# run_eval_published.sh uses.
echo "== building dyna_eval (headless, --traj only) =="
cd "$PUFFER_ROOT"
SRC="$DYNA_BARN_DIR"
gcc -O2 -DNDEBUG -DDYNA_HEADLESS -DPLATFORM_DESKTOP \
    -I"$SRC" -I"$SRC/shared" -I. -I./src -I./vendor \
    "$DYNA_BARN_DIR/dyna_eval/dyna_eval.c" \
    -o "$PUFFER_ROOT/dyna_eval" \
    -lm -lpthread -fopenmp
ls -la "$PUFFER_ROOT/dyna_eval"

echo
echo "== install complete =="
echo "Activate venv:   . $PUFFER_ROOT/.venv/bin/activate"
echo "Source paths:    . $DYNA_BARN_DIR/cluster/env.sh"
echo
echo "Smoke test:"
echo "  cd $PUFFER_ROOT && ./dyna_eval --traj /tmp/smoke.bin --episodes 1 --max-steps 50 --seed 42"
