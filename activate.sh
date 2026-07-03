if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    echo "Error: This script must be sourced, not executed." >&2
    echo "Usage: source activate.sh" >&2
    exit 1
fi

if [ ! -d .venv-zephyr ]; then
    echo "error: .venv-zephyr/ not found — run ./bootstrap.sh first" >&2
    return 1 2>/dev/null || exit 1
fi

# CHIP's environment first (gn/ninja/zap on PATH), then our venv so its
# python wins.
source modules/connectedhomeip/scripts/activate.sh

# shellcheck disable=SC1091
source .venv-zephyr/bin/activate

export EXTRA_ZEPHYR_MODULES="$PWD/modules/connectedhomeip/config/zephyr/chip-module"
export ZAP_INSTALL_PATH="$PWD/modules/connectedhomeip/.environment/cipd/packages/zap"
