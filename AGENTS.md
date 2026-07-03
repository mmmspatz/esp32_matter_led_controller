# AGENTS.md

Matter-over-WiFi LED strip controller: ESP32-WROOM-32E under **Zephyr** with
connectedhomeip's **generic Zephyr platform** (`src/platform/Zephyr`) — not
the official ESP-IDF Matter path. This combination has no upstream prior
art; expect to be the first one hitting any given integration bug.

## Workspace

T2 west topology: this repo is the manifest repo; `west update` populates
`zephyr/` and `modules/` in-tree (gitignored).

```bash
./bootstrap.sh          # once: west workspace, SDK, blobs, CHIP env
source activate.sh      # every shell: CHIP env (gn/ninja/zap) + venv
```

Builds REQUIRE `source activate.sh` first — CHIP is compiled by GN, and gn
lives in CHIP's CIPD environment, not on the system PATH.

Version pins (manifest/west.yml) — do not bump casually:
- **Zephyr v4.3.0**: last release with monolithic mbedTLS 3.x. v4.4 split
  it into mbedtls 4.x + tf-psa-crypto, which CHIP's legacy-mbedTLS crypto
  backend cannot build against. The 4.4 upgrade path is via
  `CONFIG_CHIP_CRYPTO_PSA` + CHIP master ≥ PR #43762 (untested on Zephyr).
- **CHIP master SHA 79b07ebe6b** (not a release!): v1.5.x's
  `platform/Zephyr/BLEManagerImpl.cpp` uses `BT_LE_ADV_OPT_CONNECTABLE`,
  removed in Zephyr 4.x. This SHA is proven against v4.3.0 (see
  `~/code/hispidin/docs/matter-bringup-postmortem.md`, the bringup playbook
  this project follows).
- Local CHIP patches live in `chip-patches/*.patch`, re-applied by
  bootstrap.sh after every `west update` (which resets the module).

## Build / flash / monitor

```bash
west build -b btf_wled_esp32/esp32/procpu app                       # CCT (default)
west build -b btf_wled_esp32/esp32/procpu app -- -DEXTRA_CONF_FILE=rgb.conf  # RGB
west flash                          # esptool; add --esp-device /dev/ttyUSB0 if needed
west espressif monitor              # or any 115200 terminal on ttyUSB0
west twister -p native_sim -T tests # color math unit tests
```

The BTF board has no DTR/RTS auto-download circuit: if esptool fails to
sync, hold S1 (BOOT) while plugging in / resetting, then release.

## Hardware map

| Function | GPIO | Notes |
|---|---|---|
| PWM CH1 | 27 | LEDC ch0; 74HC245 → 80N03 low-side MOSFET |
| PWM CH2 | 26 | LEDC ch1 |
| PWM CH3 | 25 | LEDC ch2 |
| Button S1 | 0 | also the BOOT strap — safe as user button |
| DAT (digital strips) | 16 | unused |
| Microphone | 36 | unused |

WROOM-32E: 4MB flash, **no PSRAM**. Custom partition table
(`dts/btf/partitions_btf_4M.dtsi`): 1856K app slots, 192K `storage`
(settings/NVS: Matter fabrics, WiFi creds, provisioning). MVP boots via
ESP Simple Boot (no MCUboot).

## Architecture

`main` + `AppTask` (CHIP's `examples/platform/silabs/zephyr` scaffold —
generic despite the path) → Matter cluster attribute writes arrive via
`MatterPostAttributeChangeCallback` → `DeviceCallbacks` → `LightingManager`
→ `color_math.c` (pure C, unit-tested) → `led_pwm.c` (the only file that
knows about LEDC).

CCT vs RGB is a build-time choice (`CONFIG_LEDCTRL_MODE_*`): two ZAP files,
two device types (0x010C Color Temperature Light / 0x010D Extended Color
Light). A runtime toggle is deliberately not offered — the Matter device
type is static in the Descriptor cluster and controllers cache it.

## Gotchas (hard-won; read before debugging)

- **`DataModelCallbacks.cpp` must be in target_sources** or every cluster
  attribute write silently no-ops (device commissions fine, ignores all
  commands).
- **`CONFIG_CHIP_FACTORY_RESET_ERASE_SETTINGS` must stay `n`** — else
  factory reset nukes the whole settings partition including the `chip-fct`
  provisioning keys (pairing codes).
- Zephyr's ESP32 WiFi driver forces `!SMP`: everything runs on one core.
- The `CONFIG_CHIP_DEVICE_DISCRIMINATOR` / `CHIP_DEVICE_SPAKE2_*` Kconfigs
  are **inert** on the generic Zephyr platform (only consumed by
  nrfconnect/telink/nxp). Provisioning goes through Zephyr settings
  `chip-fct/*` keys instead — see scripts/provision.py.
- Do not `rsource` CHIP's `Kconfig.mbedtls` (sets a symbol deprecated in
  Zephyr 4.3 → fatal warning). Its intent is inlined in `app/prj.conf`.
- Float math only (`powf`, `f` suffixes): `-Werror=double-promotion`.
- Test certs: Home Assistant accepts them; Google rejects; Alexa TBD.

## Status

Milestones: M0 board bring-up → M1 WiFi+BLE coex gate → M2 CHIP-links gate
→ M3 commissioning → M4 full app → M5 provisioning. See git history for
current position.
