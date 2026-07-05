# AGENTS.md

Matter-over-WiFi LED strip controller: ESP32-WROOM-32E under **Zephyr**
with connectedhomeip's **generic Zephyr platform** (`src/platform/Zephyr`)
— not the official ESP-IDF Matter path. To our knowledge the first working
instance of that combination; when something breaks, assume no prior art
and read the source.

## Workspace

T2 west topology: this repo is the manifest repo; `west update` populates
`zephyr/` and `modules/` in-tree (gitignored).

```bash
./bootstrap.sh          # once: west workspace, SDK, blobs, CHIP env, chip-patches
source activate.sh      # every shell: CHIP env (gn/ninja/zap) + venv
```

Builds REQUIRE `source activate.sh` — CHIP is compiled by GN, and gn
lives in CHIP's CIPD environment. Host python must be ≥3.12 (pyenv local
is set); CHIP's pigweed env pip-solve fails under 3.10.

Version pins (manifest/west.yml) — do not bump casually:
- **Zephyr v4.4.1**: mbedTLS 4.x + TF-PSA-Crypto split; CHIP runs on
  its PSA crypto backend (`CONFIG_CHIP_CRYPTO_PSA`, SPAKE2+ on the
  mbedTLS fallback). Upgrade log: `docs/zephyr-4.4-upgrade-notes.md`.
  Zephyr SDK 1.0.1 (new versioning; `west sdk install` handles it).
- **CHIP master SHA 9d45993e3f** (2026-07-01; not a release: v1.5.x's
  `platform/Zephyr` uses BLE flags removed in Zephyr 4.x. This pin has
  the mbedTLS-4 include guards, #72416).
- `chip-patches/*.patch` are re-applied by bootstrap after every
  `west update`. Current set: 0002 `__noinit` heap placement
  (ESP32-specific, load-bearing), 0003 NXP Kconfig.defaults legacy
  mbedTLS symbols (fatal typeless defaults under 4.4), 0004 skip
  add_entropy_source under CHIP_CRYPTO_PSA (boot fails 0x6C without),
  0005 `sys_multi_heap` + `Malloc::AddReclaimedRegion()` in SysHeapMalloc
  (runtime BT-DRAM reclaim; stacks on 0002), 0006 Zephyr BLE `_Shutdown`
  sets `mServiceMode = Disabled` (makes BLE strictly one-way for the reclaim).

## Build / flash / monitor

```bash
west build -b btf_wled_esp32/esp32/procpu app                                  # CCT (default)
west build -b btf_wled_esp32/esp32/procpu app -- -DEXTRA_CONF_FILE=rgb.conf    # RGB variant
west build -b btf_wled_esp32/esp32/procpu app -- -DEXTRA_CONF_FILE=prov.conf   # provisioning image
west flash                            # esptool; board has working DTR/RTS autoboot
west twister -p native_sim/native/64 -T tests   # color math unit tests
```

Serial console: 115200 on /dev/ttyUSB0. Flashing does NOT touch the
storage partition: fabrics, WiFi credentials and provisioning survive.

## Provisioning (per-device pairing codes)

Production images have no shell (RAM). Once per board:

```bash
west build -d build_prov -b btf_wled_esp32/esp32/procpu app -- -DEXTRA_CONF_FILE=prov.conf
west flash -d build_prov
./scripts/provision.py --port /dev/ttyUSB0     # writes codes, prints QR
west build -b btf_wled_esp32/esp32/procpu app && west flash
```

The separate `-d build_prov` dir matters: `EXTRA_CONF_FILE` is sticky in
`CMakeCache.txt` and `auto` pristine ignores conf-file changes, so reusing
the default `build/` silently rebuilds prov.

Codes live in the `chip-fct` settings namespace: read by the
commissionable-data provider before the test-code fallback
(20202021/0xF00, manual code 34970112332), untouched by factory reset
(as long as `CONFIG_CHIP_FACTORY_RESET_ERASE_SETTINGS` stays off) and by
reflashing. Records land in gitignored `devices/`.

## Architecture

`app/src/scaffold/` (vendored CHIP example scaffold, see below) provides
main/AppTaskBase/AppTaskZephyr/CHIPDeviceManager/DataModelCallbacks.
Attribute writes flow: cluster server → `MatterPostAttributeChangeCallback`
(scaffold DataModelCallbacks) → `DeviceCallbacks` → `LightingManager`
(cluster state → channel duties; 20ms exponential smoother over the
clusters' 100ms transition ticks) → `led_pwm` (the only LEDC-aware file).
`color_math.c` is pure C, unit-tested on native_sim.

CCT vs RGB is build-time (`CONFIG_LEDCTRL_MODE_*`): two ZAP files, two
device types (0x010C / 0x010D). The `.matter` files are codegen inputs,
not just review artifacts — after editing a `.zap`, regenerate:
`./modules/connectedhomeip/scripts/tools/zap/generate.py --zcl
modules/connectedhomeip/src/app/zap-templates/zcl/zcl.json <file>.zap`,
or the build links stale plugin callbacks.

### Hardware map

| Function | GPIO | Notes |
|---|---|---|
| PWM CH1 | 27 | LEDC ch0; CCT strip: **cool** white |
| PWM CH2 | 26 | LEDC ch1; CCT strip: **warm** white |
| PWM CH3 | 25 | LEDC ch2; unused in CCT mode |
| Button S1 | 0 | BOOT strap; short=nothing yet, 5s hold=factory reset |
| DAT / mic | 16 / 36 | unused |

WROOM-32E: 4MB flash, no PSRAM. Custom partitions
(`dts/btf/partitions_btf_4M.dtsi`): 1856K app slots (OTA-shaped, unused
slot1), 192K `storage`. ESP Simple Boot, no MCUboot.

## RAM: the defining constraint

Usable DRAM is two banks: **dram0 ≈ 136K** (SRAM2 minus the 56K BT-blob
reserve) for `.bss`+`.data`, and **dram1 = 96K** (SRAM1) which on this
port only receives `.noinit`. The build sits at ~99.7% of dram0; every
static allocation matters. Standing arrangements:
- All runtime allocation goes through CHIP's sys_heap (40K, `--wrap=malloc`),
  placed in dram1 via `__noinit` (chip-patches/0002);
  `CONFIG_COMMON_LIBC_MALLOC=n` because a zero-size libc arena panics at boot.
- Kernel pool floor lowered by re-defaulting the radios' promptless
  `HEAP_MEM_POOL_ADD_SIZE_*` in app/Kconfig (parsed first, first default
  wins). Measured radio peak ~42K; pool ≈ 43.8K. Re-measure on any
  Zephyr/hal bump. (4.4 raised the WiFi default to 51200; our 24576
  survived the commissioning/operation smoke test but has not been
  re-soaked under WiFi+BLE coex on the new blobs.)
- IM/session pools scale from `CHIP_CONFIG_MAX_FABRICS 4` (spec floor —
  don't hand-trim the pools).
- When dram0 overflows: force the link with `-Wl,--noinhibit-exec`, then
  `nm --size-sort` on `zephyr_pre0.elf` and cut what's actually there.
- Post-commissioning BT-DRAM reclaim (`CONFIG_LEDCTRL_RECLAIM_BT_DRAM_AFTER_COMMISSIONING`,
  default y): once provisioned, the app (CommonDeviceCallbacks, on
  `kCommissioningComplete` and on WiFi-established-while-provisioned) tears BLE
  down (`BLEMgr().Shutdown()` → `bt_disable()`, waits for `NumConnections()==0`),
  verifies the controller is IDLE, then `sys_multi_heap`-adds the 54.8K reserve
  window `[procpu_dram0_org − CONFIG_ESP32_BT_RESERVE_DRAM, procpu_dram0_org)` =
  `[0x3ffb0000, 0x3ffbdb5c)` to the CHIP malloc heap. The reserve is a *link-time*
  carve-out below dram0, so this is runtime-only (doesn't relieve static dram0);
  it hands ~54.7K to `malloc` for OTA/session headroom. HW-verified: heap free
  jumps ~+54.7K on both trigger paths, cluster control fine afterward. One-way:
  BLE can't return without a reboot (patch 0006), so additional-fabric
  commissioning is on-network. `esp_bt_mem_release` is a Zephyr no-op stub and
  `heap_caps` is just a `k_malloc` shim, so the reclaim is done Zephyr-natively
  in the app/CHIP layer, not via the HAL.

## Gotchas (each cost real debugging time)

- **`DataModelCallbacks.cpp` must be in target_sources** or attribute
  writes silently no-op.
- **`CONFIG_CHIP_ENABLE_WIFI_STATION=y`** is mandatory and defaults off:
  without it the NetworkCommissioning WiFi commands are compiled out of
  libCHIP and every commissioner fails right after credential install
  with UNSUPPORTED_COMMAND.
- chip-module's `Kconfig.defaults` is Thread-biased: we override
  `NET_L2_OPENTHREAD=n`, `CHIP_OTA_REQUESTOR=n`, `NET_IPV6_NBR_CACHE=y`
  (its `n` breaks CHIP's own WiFi router solicitation at link time).
- Do not rsource CHIP's `Kconfig.mbedtls` (deprecated symbols → fatal);
  the crypto feature set arrives via `CHIP_CRYPTO_PSA` implies plus a
  few explicit PSA_WANT/MBEDTLS symbols in prj.conf. `MBEDTLS_MD_C=y`
  is load-bearing (SPAKE2+ mbedTLS fallback links against it).
- `MBEDTLS_PSA_KEY_SLOT_COUNT` is 32, not the NXP-default 64: the slot
  table is dram0 .bss (~40 B/slot) and 64 overflows the bank.
- The cluster servers restore persisted state and process no-op commands
  WITHOUT firing attribute callbacks: any state cache (LightingManager)
  must be primed from cluster reads at server start, or you get SUCCESS
  responses and a dark strip.
- Factory reset works but the final reboot panics in hal_espressif
  (`esp_restart` stops WiFi under `irq_lock`); state is already wiped —
  power-cycle completes it.
- picolibc's `srand` chain collides with the WiFi blob's strong
  `random()`: app-local `rand_shim.c` breaks the chain.
- Float math only (`powf`, `f` suffixes): `-Werror=double-promotion`.
- Test certs (this device uses the example DAC + test VID 0xFFF1): HA's
  Matter Server (matter.js, HA 2026.7+) rejects them under its default
  production trust policy — commissioning reaches DeviceAttestation then
  fails. Enable Settings → Apps → Matter Server → Configuration →
  "Enable test-net DCL usage" to commission (needed all the way through
  adoption, not just DeviceAttestation). Google rejects; Alexa unverified.

## Local controller (chip-tool)

Built at `build_chiptool/chip-tool` (host build:
`./scripts/examples/gn_build_example.sh examples/chip-tool build_chiptool
'chip_support_thread_meshcop=false chip_enable_wifi=false
chip_enable_thread=false'`; needs gdbus-codegen + extra submodules:
perfetto, libwebsockets, editline). Recipes:

```bash
chip-tool pairing ble-wifi 1 "<ssid>" "<psk>" <passcode> <discriminator>
chip-tool onoff on 1 1
chip-tool levelcontrol move-to-level 200 5 0 0 1 1
chip-tool colorcontrol move-to-color-temperature 370 6 0 0 1 1
chip-tool pairing unpair 1
```

## OTA (firmware update over WiFi)

Working end-to-end via BOTH chip-tool (dev harness) and Home Assistant (the
production path). MCUboot (sysbuild, overwrite-only, RSA-2048 signed) + CHIP OTA
Requestor; config in `app/sysbuild.conf` + `prj.conf`. See also [OTA approach] and
[HA OTA integration] in the auto-memory for the full bring-up story.

### Build a signed `.ota`
```bash
# 1. bump the version the requestor compares (prj.conf), rebuild
#    CONFIG_CHIP_DEVICE_SOFTWARE_VERSION=<N> + _STRING="0.0.<N>"
west build -b btf_wled_esp32/esp32/procpu app
# 2. wrap the SIGNED slot image (NOT raw zephyr.bin) as a Matter OTA image
python modules/connectedhomeip/src/app/ota_image_tool.py create \
  -v 0xFFF1 -p 0x8005 -vn <N> -vs "0.0.<N>" -da sha256 \
  build/app/zephyr/zephyr.signed.bin fw.ota
```
- PID is **0x8005** (0x010C/0x010D are device-type IDs, not PIDs); VID 0xFFF1 (test).
- Payload is `zephyr.signed.bin` (MCUboot-headered + RSA-signed). Signature type is
  set by sysbuild (`SB_CONFIG_BOOT_SIGNATURE_TYPE_RSA` + key), not the SoC overlay;
  MCUboot validates the SECONDARY before swap (slot0 itself is unvalidated,
  `BOOT_VALIDATE_SLOT0=n`). Editing a `sysbuild/<image>.conf` overlay needs a
  pristine build (`-p always`) — overlays are discovered only at configure time.

### chip-tool path (dev)
`ota-provider-app` at `build_ota_provider/` (`gn_build_example.sh
examples/ota-provider-app build_ota_provider 'chip_config_network_layer_ble=false
chip_enable_thread=false chip_support_thread_meshcop=false'` — else the
ot-commissioner submodule breaks gn gen).
```bash
chip-ota-provider-app --discriminator 3840 --secured-device-port 5560 \
  --KVS /tmp/ota_provider.kvs --filepath fw.ota          # reuse KVS to stay commissioned
chip-tool pairing onnetwork-long 2 20202021 3840          # provider = node 2
# then write an ACL on node 2 granting Operate (priv 3) on the OTA Provider cluster
# (0x0029) to the requestor -- keep the admin entry, add an operate entry with
# targets=[{cluster:41}] -- else the requestor's QueryImage is denied:
chip-tool accesscontrol write acl '<admin+operate JSON>' 2 0
chip-tool otasoftwareupdaterequestor announce-otaprovider 2 0 0 0 1 0   # tell node 1
```
BDX download of the ~1.3 MB image runs a few minutes. NOTE: this provider tolerates
`ApplyUpdateRequest.newVersion=0`, so it will mask a device-side apply bug that a
strict provider (HA) rejects — don't treat "works on chip-tool" as fully validated.

### Home Assistant path (production, matter.js)
- Enable **test-net-DCL** (Settings → Apps → Matter Server → Configuration): required
  for the test certs AND for the custom local-OTA dir.
- Put `fw.ota` + a JSON descriptor in the ROOT of
  `/addon_configs/core_matter_server/updates/`, then restart the add-on to import:
  ```json
  { "modelVersion": { "vid": 65521, "pid": 32773, "softwareVersion": <N>,
    "softwareVersionString": "0.0.<N>", "minApplicableSoftwareVersion": 0,
    "maxApplicableSoftwareVersion": 2147483647, "otaUrl": "file:///fw.ota",
    "otaFileSize": <bytes>, "otaChecksum": "<base64(sha256(fw.ota))>",
    "otaChecksumType": 1 } }
  ```
- Trigger: the HA update entity, or the matter-server WS API
  `update_node{node_id, software_version}` at `ws://core-matter-server:5580/ws`. The
  port is not published to the HA host and the SSH add-on prohibits `-L` forwarding —
  run the WS client on the box (`apk add python3 py3-pip`, `pip install websockets`).
  HA persistently re-drives queued updates: a node left below its target version gets
  re-announced/re-downloaded on its own (a trigger may return "already in the process
  of updating").

### Device-side facts (load-bearing)
- **`InitOTA` must run once, not per `kDnssdInitialized`.** The Zephyr WiFi driver
  re-posts that event on every (re)connection (~every 100 s here) to rebind mDNS;
  re-running InitOTA re-enters `DefaultOTARequestor::Init → LoadCurrentUpdateInfo`,
  which reloads state from KVS and zeroes the in-RAM `mTargetVersion` mid-download →
  `ApplyUpdateRequest.newVersion=0` → strict providers discontinue. Guarded one-shot
  in `CommonDeviceCallbacks` (kDnssdInitialized case).
- **Overwrite-only:** `Swap type: none` at boot is normal EVEN after a successful swap
  — verify by the reported software version changing, not the swap log. MCUboot
  self-erases slot1 after a successful swap (no app-level cleanup needed). No revert
  (`CONFIG_CHIP_OTA_REQUEST_UPGRADE_PERMANENT=y`).
- The apply reboot uses `sys_reboot(WARM)` → `rst:0x10 (RTCWDT_RTC_RESET)` (the
  esp_restart-under-WiFi quirk); the pending flag is written first, so the upgrade
  still takes.
- Recovery from a bad/pending secondary: `esptool --chip esp32 erase-region 0x1F0000
  0x1D0000` (slot1). slot0 is at 0x20000; both slots are 1856K.

## Scaffold provenance

`app/src/scaffold/` is CHIP's `examples/platform/silabs/zephyr` (generic
despite the name), vendored at pin 79b07ebe6b with one fix: the
NetworkCommissioning instance Init moved into InitServer (Matter thread,
after Server::Init) — upstream calls it on the app thread before the
registry exists and discards the error. Re-diff against upstream when
re-pinning.
