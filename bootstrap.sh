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

# CHIP's pigweed env setup pip-compiles its requirements against the host
# python; under 3.10 that resolution fails (mobly conflict). 3.12 is proven.
MIN_PY="3.12"
HOST_PY=$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')
if [ "$(printf '%s\n%s\n' "$MIN_PY" "$HOST_PY" | sort -V | head -n1)" != "$MIN_PY" ]; then
    echo "error: python3 on PATH is $HOST_PY; need >= $MIN_PY" >&2
    echo "hint:  pyenv install 3.12 && pyenv local 3.12" >&2
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
# Drop our patches before updating, so a pin bump checks out over pristine
# trees and the apply below starts clean. `west patch` is a Zephyr extension
# command, so a fresh workspace (no zephyr/ yet) has nothing to clean anyway.
if [ -d zephyr ]; then
    west patch clean
fi
west update

pip install --quiet pytest pyserial ecdsa qrcode

# Zephyr's own requirements.txt (superset of requirements-base.txt) plus
# per-module python deps (notably esptool from hal_espressif).
west packages pip --install

pip install --quiet \
    -r modules/connectedhomeip/scripts/setup/requirements.build.txt \
    -r modules/connectedhomeip/scripts/setup/requirements.zephyr.txt \
    -r modules/connectedhomeip/scripts/setup/requirements.setuppayload.txt

# WiFi/BT MAC, PHY and coexistence libraries are proprietary Espressif blobs.
west blobs fetch hal_espressif

# xtensa: classic ESP32 board; riscv64: the ESP32-C6 rework (one multilib
# toolchain covers rv32 targets).
west sdk install -t xtensa-espressif_esp32_zephyr-elf -t riscv64-zephyr-elf

modules/connectedhomeip/scripts/checkout_submodules.py --shallow
# Zephyr provides OpenThread (and we don't even enable it); CHIP's copy is
# never referenced once chip_enable_openthread=false.
(cd modules/connectedhomeip && git submodule deinit -f third_party/openthread/repo)

# Local patches to upstream projects (manifest/zephyr/patches.yml). Applied
# onto the trees `west patch clean` reset above; --roll-back undoes a
# half-applied set if one stops applying.
west patch apply --roll-back

# CHIP's own environment (gn, ninja, zap, ...) via pigweed CIPD.
bash modules/connectedhomeip/scripts/bootstrap.sh -p none

# The MCUboot OTA signing key is a per-developer secret, so keys/ is gitignored
# and a fresh clone has none -- without it the very first build dies in
# zephyr/cmake/mcuboot.cmake ("can't find file .../ota-signing-ecdsa-p256.pem").
# Generated last so its back-it-up banner lands next to "Bootstrap complete"
# instead of scrolling past under the toolchain output. Never overwrites.
./scripts/gen-signing-key.sh

deactivate

echo
echo "Bootstrap complete.  Use: source activate.sh"
