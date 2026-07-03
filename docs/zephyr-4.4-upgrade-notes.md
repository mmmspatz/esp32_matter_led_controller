# Zephyr 4.4.1 upgrade checklist

Distilled from a source-level audit (2026-07-03) of every workaround in
this repo against Zephyr v4.4.1 and CHIP master. The crux: Zephyr 4.4
ships mbedTLS 4.x + TF-PSA-Crypto and deletes the legacy `MBEDTLS_*_C`
Kconfig surface, so CHIP must switch to its PSA crypto backend. Nobody
has publicly built the generic-Zephyr chip-module against 4.4.x; expect
to be first again.

## Manifest

- `zephyr` → `v4.4.1`; add `tf-psa-crypto` to the name-allowlist (do NOT
  add `mbedtls-3.6`, it's TF-M-only).
- `connectedhomeip` → master SHA ≥ 2026-06-30. Must include:
  - #72415 (`149d9fd67d`): retires chip-patches/0001 (the patch will
    fail to apply — that's the signal to delete it)
  - #72416: guards `mbedtls/ecp.h` include in CHIPCryptoPALmbedTLSCert —
    mandatory for mbedTLS 4
  - #72701: removes deprecated Zephyr API uses
- Re-diff `app/src/scaffold/` against upstream
  `examples/platform/silabs/zephyr` (expect OTA-related drift, #72698).
  Keep our NetworkCommissioning-Init-in-InitServer fix.

## prj.conf

- Delete `CONFIG_MBEDTLS_USE_PSA_CRYPTO=n` (symbol removed in 4.4) and
  the entire legacy mbedTLS feature block.
- Add:
  ```
  CONFIG_CHIP_CRYPTO_PSA=y
  CONFIG_MBEDTLS_PSA_CRYPTO_C=y
  CONFIG_PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT=y
  CONFIG_PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_EXPORT=y
  CONFIG_PSA_WANT_KEY_TYPE_HMAC=y
  CONFIG_MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS=y   # SPAKE2+ fallback, CHIP #71479
  CONFIG_SECURE_STORAGE=y                        # PSA ITS for operational keys
  CONFIG_MBEDTLS_PSA_KEY_SLOT_COUNT=64
  ```
  (ECDSA/ECDH/P-256/CCM/HKDF/SHA-256 arrive via `CHIP_CRYPTO_PSA` implies.)
  SPAKE2+ must stay on the mbedTLS backend (`chip_crypto_spake2p`
  choice default): TF-PSA-Crypto has no `PSA_WANT_ALG_SPAKE2P_MATTER`.
- `CONFIG_ESP32_REGION_1_NOINIT=y` is already pinned (4.4 defaults it
  off under MCUboot, which would sink the CHIP heap back into dram0).

## RAM re-measurement (mandatory)

- 4.4 raised `HEAP_MEM_POOL_ADD_SIZE_ESP_WIFI` 40960→51200 citing real
  allocation failures; our re-default to 24576 assumes the 4.3 driver
  with trimmed buffers and only ~1.8K pool headroom over the measured
  42K peak. Re-run the WiFi+BLE coex spike on the new hal before
  trusting it.
- PSA adds key-slot tables and secure_storage; redo the dram0/dram1
  accounting (the budget is measured-to-fit with 408 bytes spare).

## Behavior changes

- Operational keys move to PSA ITS: **commissioned devices lose their
  fabrics across this upgrade.** Factory-reset and recommission.
- hal_espressif is restructured (ESP-IDF components layout); the strong
  `random()` shim moves to `components/esp_wifi/esp32/esp_adapter.c` —
  rand_shim.c still needed, update its comment.

## Risk order

1. PSA crypto on generic Zephyr (untested upstream combination)
2. WiFi driver heap demand under the new blobs
3. CHIP re-pin churn in platform/Zephyr and the vendored scaffold
4. RAM regressions (PSA tables, secure_storage)
5. BLE (lowest: dynamic GATT untouched in 4.4)

## Upstream filing opportunities (not part of this effort; fork instead)

- `CHIP_ENABLE_WIFI_STATION` defaults n even with `CHIP_WIFI=y` (generic
  Zephyr NetworkCommissioning rejects AddOrUpdateWiFiNetwork) — NXP
  carries the default y, gated to their SoCs. Unreported.
- chip-module `Kconfig.defaults` sets `NET_IPV6_NBR_CACHE` default n
  unconditionally (Thread-think); breaks the WiFi platform's own
  `net_if_start_rs`. Unreported.
- hal_espressif's strong `random()` vs picolibc's rand family (our
  rand_shim.c). Unreported.
- `__noinit` on CHIP's SysHeapMalloc buffer (chip-patches/0002).
- The scaffold's NetworkCommissioning Init-before-InitServer race
  (fixed in our vendored copy; upstream silabs/zephyr still has it).
