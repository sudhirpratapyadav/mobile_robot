#!/bin/bash
# One-shot bootstrap: symlink the dyna_train and dyna_eval envs into PufferLib's
# ocean/ layout, plus the matching .ini files into config/.
#
# Run from the HOST, not inside the container:
#   bash setup_container.sh
#
# Assumes the puffertank container is up and that the parent dir of dyna_barn
# is already bind-mounted at /puffertank/host inside the container (this was
# done when the container was started for mobile_robot_env work; the path
# /puffertank/host/dyna_barn just appears).

set -e

CONTAINER=puffertank
HOST_ROOT=/puffertank/host/dyna_barn
PUFFER_ROOT=/puffertank/pufferlib

if ! docker ps --filter "name=$CONTAINER" --format '{{.Names}}' | grep -q "^$CONTAINER$"; then
    echo "Error: container '$CONTAINER' not running. Start it first." >&2
    exit 1
fi

# Verify the host bind-mount exposes our dir
if ! docker exec "$CONTAINER" test -d "$HOST_ROOT"; then
    echo "Error: $HOST_ROOT not visible inside the container." >&2
    echo "Check that /puffertank/host is bind-mounted to the parent of dyna_barn." >&2
    exit 1
fi

echo "=== Symlinking envs into ocean/ ==="
docker exec "$CONTAINER" bash -c "
    ln -sfn $HOST_ROOT/dyna_train $PUFFER_ROOT/ocean/dyna_train
    ln -sfn $HOST_ROOT/dyna_eval  $PUFFER_ROOT/ocean/dyna_eval
"

echo "=== Symlinking config .ini files into config/ ==="
docker exec "$CONTAINER" bash -c "
    ln -sf $HOST_ROOT/dyna_train/dyna_train.ini $PUFFER_ROOT/config/dyna_train.ini
    ln -sf $HOST_ROOT/dyna_eval/dyna_eval.ini   $PUFFER_ROOT/config/dyna_eval.ini
"

echo "=== Verifying ==="
docker exec "$CONTAINER" ls -la "$PUFFER_ROOT/ocean/dyna_train" "$PUFFER_ROOT/ocean/dyna_eval"
docker exec "$CONTAINER" ls -la "$PUFFER_ROOT/config/dyna_train.ini" "$PUFFER_ROOT/config/dyna_eval.ini"

echo
echo "Setup complete. Inside the container, you can now run:"
echo "  bash build.sh dyna_train --fast    # standalone build"
echo "  bash build.sh dyna_train           # python build"
echo "  python -m pufferlib.pufferl train dyna_train"
