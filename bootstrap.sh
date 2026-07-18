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
west update

pip install --quiet pytest pyserial ecdsa qrcode

# Zephyr's own requirements.txt (superset of requirements-base.txt) plus
# per-module python deps (notably esptool from hal_espressif).
west packages pip --install

pip install --quiet \
    -r modules/connectedhomeip/scripts/setup/requirements.build.txt \
    -r modules/connectedhomeip/scripts/setup/requirements.zephyr.txt

# WiFi/BT MAC, PHY and coexistence libraries are proprietary Espressif blobs.
west blobs fetch hal_espressif

# xtensa: classic ESP32 board; riscv64: the ESP32-C6 rework (one multilib
# toolchain covers rv32 targets).
west sdk install -t xtensa-espressif_esp32_zephyr-elf -t riscv64-zephyr-elf

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

# Local patches to MCUboot (west module, reset by `west update`). 0001 adds the
# psa_crypto_init() that MCUboot's ECDSA-PSA verify path is missing (ed25519 has
# it) -- without it the ESP32 bootloader wedges verifying the OTA signature.
for p in "$PWD"/mcuboot-patches/*.patch; do
    if git -C bootloader/mcuboot apply --reverse --check "$p" 2>/dev/null; then
        echo "mcuboot patch already applied: $(basename "$p")"
    else
        echo "applying mcuboot patch: $(basename "$p")"
        git -C bootloader/mcuboot apply "$p"
    fi
done

# Local patches to hal_espressif (west module, reset by `west update`). 0001
# makes the esp_os_*_critical* macros recursion-safe; without it the ESP32-C6
# BLE controller init nests the modem-clock critical section, leaves IRQs
# disabled, and the BLE host panics on its first blocking wait ("Context
# switching while holding lock!").
for p in "$PWD"/hal-patches/*.patch; do
    if git -C modules/hal/espressif apply --reverse --check "$p" 2>/dev/null; then
        echo "hal patch already applied: $(basename "$p")"
    else
        echo "applying hal patch: $(basename "$p")"
        git -C modules/hal/espressif apply "$p"
    fi
done
shopt -u nullglob

# CHIP's own environment (gn, ninja, zap, ...) via pigweed CIPD.
bash modules/connectedhomeip/scripts/bootstrap.sh -p none

deactivate

echo
echo "Bootstrap complete.  Use: source activate.sh"
