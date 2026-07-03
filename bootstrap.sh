#!/usr/bin/env bash
# Bootstrap the west workspace: Zephyr, connectedhomeip, toolchain, blobs.
#
#   ./bootstrap.sh

set -e

for arg in "$@"; do
    case "$arg" in
        -h|--help)
            sed -n '2,4p' "$0" | sed 's/^# \?//'
            exit 0 ;;
        *) echo "unknown flag: $arg" >&2; exit 2 ;;
    esac
done

MIN_PY="3.10"
HOST_PY=$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')
if [ "$(printf '%s\n%s\n' "$MIN_PY" "$HOST_PY" | sort -V | head -n1)" != "$MIN_PY" ]; then
    echo "error: python3 on PATH is $HOST_PY; need >= $MIN_PY" >&2
    exit 1
fi

if [ ! -d .venv-zephyr ]; then
    python3 -m venv .venv-zephyr
fi
# shellcheck disable=SC1091
source .venv-zephyr/bin/activate

pip install --upgrade --quiet pip west

if [ ! -d .west ]; then
    west init -l manifest
fi
west update

pip install --quiet -r zephyr/scripts/requirements-base.txt
pip install --quiet pytest pyserial ecdsa qrcode

pip install --quiet \
    -r modules/connectedhomeip/scripts/setup/requirements.build.txt \
    -r modules/connectedhomeip/scripts/setup/requirements.zephyr.txt

# WiFi/BT MAC, PHY and coexistence libraries are proprietary Espressif blobs.
west blobs fetch hal_espressif

west sdk install -t xtensa-espressif_esp32_zephyr-elf

modules/connectedhomeip/scripts/checkout_submodules.py --shallow
# Zephyr provides OpenThread (and we don't even enable it); CHIP's copy is
# never referenced once chip_enable_openthread=false.
(cd modules/connectedhomeip && git submodule deinit -f third_party/openthread/repo)

# Local patches to CHIP that have not (yet) landed upstream. `west update`
# resets the tree, so bootstrap re-applies them idempotently.
shopt -s nullglob
for p in "$PWD"/chip-patches/*.patch; do
    if git -C modules/connectedhomeip apply --reverse --check "$p" 2>/dev/null; then
        echo "chip patch already applied: $(basename "$p")"
    else
        echo "applying chip patch: $(basename "$p")"
        git -C modules/connectedhomeip apply "$p"
    fi
done
shopt -u nullglob

# CHIP's own environment (gn, ninja, zap, ...) via pigweed CIPD.
bash modules/connectedhomeip/scripts/bootstrap.sh -p none

deactivate

echo
echo "Bootstrap complete.  Use: source activate.sh"
