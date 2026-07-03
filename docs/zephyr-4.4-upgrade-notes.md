# Zephyr 4.4.1 upgrade — executed 2026-07-03

This began as a pre-upgrade checklist distilled from a source-level
audit (kept below, annotated); the upgrade has now been performed and
verified on hardware (commission via BLE-WiFi, attribute control +
readback, reboot persistence + WiFi rejoin). The crux held: Zephyr 4.4
ships mbedTLS 4.x + TF-PSA-Crypto and deletes the legacy `MBEDTLS_*_C`
Kconfig surface, so CHIP runs on its PSA backend
(`CONFIG_CHIP_CRYPTO_PSA`). As far as we know this is the first
generic-Zephyr chip-module build against 4.4.x.

## Final pins

- zephyr **v4.4.1** (+ `tf-psa-crypto` in the name-allowlist)
- connectedhomeip **9d45993e3fc0655c51591892a850aef89b10289a**
  (master 2026-07-01; contains #72415, #72416, #72701, #72698)
- Zephyr SDK **1.0.1** (new versioning scheme; toolchain now lives at
  `~/zephyr-sdk-1.0.1/gnu/xtensa-espressif_esp32_zephyr-elf`)
- chip-patches: 0001 deleted; 0002 unchanged; **0003 and 0004 new**
  (see below)

## What the checklist got right

- Manifest moves, including "don't add mbedtls-3.6" (it is TF-M-only).
- The prj.conf crypto swap shape: CHIP_CRYPTO_PSA + the three
  PSA_WANT key-type gaps + MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS +
  SECURE_STORAGE. SPAKE2+ indeed stays on the mbedTLS backend
  (`CHIP_CRYPTO_SPAKE2P_MBEDTLS` is the upstream choice default).
- Operational keys now live in PSA ITS (`its/` settings namespace);
  the pre-upgrade unpair was the right call. Fabric persistence across
  reboot verified on the new keystore.
- hal_espressif is restructured to ESP-IDF components layout; the
  strong `random()` is at `components/esp_wifi/esp32/esp_adapter.c`
  and rand_shim.c is still required.
- Scaffold re-diff: the only drift vs upstream at the new pin was our
  own NetworkCommissioning Init-inside-InitServer fix (upstream now
  propagates the error but still calls Init on the app thread before
  InitServer). The #72698 OTA work added new files (ota_requestor/,
  AppCLIZephyr) that we don't vendor; zero changes needed.
- RAM was indeed the fight (see below), and the risk ordering was
  roughly right, except the top risk (PSA-on-generic-Zephyr) manifested
  as one small boot-time bug, not a crypto problem.

## What the checklist got wrong / missed

1. **chip-patches/0001 did NOT fail to apply.** #72415 removed the
   `assert()` calls but the patch's context lines survived, so it
   applied cleanly as dead weight. Deleted after grep-verifying no
   `assert(` remains in WiFiManager.cpp. Lesson: verify by symbol, not
   by patch-application failure.
2. **`CONFIG_MBEDTLS_MD_C=y` is required** and was missing from the
   prj.conf block: CHIPCryptoPALPSA's SPAKE2+ fallback calls
   `mbedtls_md_info_from_type` (undefined reference at link otherwise).
   The option survives in 4.4's Kconfig.tf-psa-crypto.
3. **`MBEDTLS_PSA_KEY_SLOT_COUNT=64` does not fit**: the open-key-slot
   table is dram0 .bss inside tf-psa-crypto's `global_data`
   (656 → 2576 bytes vs 4.3) and overflowed dram0 by 808 bytes.
   Now 32 (slots bound concurrently-open keys only, not stored keys);
   commissioning + operation verified at 32.
4. **`CONFIG_THREAD_LOCAL_STORAGE=y` had to be deleted** from
   prj.conf: xtensa has no `ARCH_HAS_THREAD_LOCAL_STORAGE`, the 4.3
   assignment never took effect (verified in the old .config), and 4.4
   turns no-op Kconfig assignments into hard errors.
5. **config/zephyr/chip-module/Kconfig.defaults now rsources
   config/nxp/chip-module/Kconfig.defaults**, which re-defaults seven
   legacy mbedTLS symbols; typeless defaults are fatal under 4.4
   → new **chip-patches/0003** drops them. (Upside of the same
   rsource: `CHIP_ENABLE_WIFI_STATION` now defaults y with CHIP_WIFI,
   retiring one of the "upstream filing opportunities" below.)
6. **Boot failed with 0x6C (UNSUPPORTED_CHIP_FEATURE)**:
   platform/Zephyr's PlatformManagerImpl registers an mbedTLS entropy
   source, which is a stub under the PSA PAL, and its guard doesn't
   know CHIP_CRYPTO_PSA (on 4.4+ESP32 the PSA RNG is
   `MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG` on the TRNG csprng driver, so no
   guard Kconfig is set) → new **chip-patches/0004** gates the block
   on `!CHIP_CRYPTO_PSA`. Prime upstream candidate.

## RAM outcome (CCT image)

```
FLASH:       1360688 B / 4194048 B   32.44%
iram0_0_seg:  114176 B / 224 KB      49.78%
dram0_0_seg:  139980 B / 140452 B    99.66%   (472 B spare)
dram1_0_seg:   93032 B / 96 KB       94.64%
```

Symbol-level dram0 delta vs the 4.3 build (before the slot-count fix):
PSA `global_data` +1920, WiFi blob `gChmCxt` +340, BT tx workq +176,
tx_meta +168; removals: CHIP's gsEntropyContext −744 (PSA owns RNG
now), country_info/s_log_cache/chip7_sleep_params gone. Net +1470,
brought back under the wire by halving the PSA key slots (−1280).

- The `HEAP_MEM_POOL_ADD_SIZE_ESP_WIFI` re-default (24576; 4.4 default
  is 51200) and `_ESP_BT` (16384) **held during commissioning and
  operation** — no radio allocation failures observed. A sustained
  WiFi+BLE coex soak has NOT been re-run; if radio allocs fail in the
  field, raise the re-defaults in app/Kconfig and re-balance.
- `CONFIG_ESP32_REGION_1_NOINIT=y` stays pinned (4.4 defaults it off
  under MCUboot).

## Behavior notes

- Boot logs `WARNING: Using a potentially insecure PSA ITS encryption
  key provider`: secure_storage derives its AEAD key from the device
  ID hash (no HUK on ESP32). Same trust level as 4.3's plaintext-NVS
  keys — fine for this device class; a hardware-unique-key story would
  be needed to do better.
- `IPV6_PKTINFO failed: 109` errors are pre-existing on 4.3 (compared
  boot logs), not a regression.
- Commissioned devices lose fabrics across the upgrade (keystore moved
  to PSA ITS) — factory-reset/recommission, as predicted.

## Upstream filing opportunities (fork carries them meanwhile)

- **chip-patches/0004**: add_entropy_source called under
  CHIP_CRYPTO_PSA on generic Zephyr → boot failure. Unreported.
- **chip-patches/0003**: NXP Kconfig.defaults re-defaults legacy
  mbedTLS symbols, fatal on Zephyr 4.4. Needs a version-guard story
  (NXP's downstream Zephyr still has the symbols). Unreported.
- chip-module `Kconfig.defaults` sets `NET_IPV6_NBR_CACHE` default n
  unconditionally (Thread-think); breaks the WiFi platform's own
  `net_if_start_rs`. Unreported.
- hal_espressif's strong `random()` vs picolibc's rand family (our
  rand_shim.c). Unreported.
- `__noinit` on CHIP's SysHeapMalloc buffer (chip-patches/0002).
- The scaffold's NetworkCommissioning Init-before-InitServer race
  (fixed in our vendored copy; upstream now ReturnErrorOnFailure's the
  result but still calls it on the wrong thread, before InitServer).
- ~~CHIP_ENABLE_WIFI_STATION defaults n even with CHIP_WIFI~~ — fixed
  upstream via the NXP defaults rsource at this pin (we keep the
  explicit =y in prj.conf as documentation).
