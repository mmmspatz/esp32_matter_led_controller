/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Zephyr stand-in for ESP-IDF's esp_crypto_lock.h, covering only what
 * hal_espressif's esp_ecc.c uses (hal_espressif ships no C6 implementation
 * of either header on the Zephyr port). Implemented in esp_crypto_shims.c.
 */

#ifndef ESP_CRYPTO_LOCK_H
#define ESP_CRYPTO_LOCK_H

#ifdef __cplusplus
extern "C" {
#endif

void esp_crypto_ecc_lock_acquire(void);
void esp_crypto_ecc_lock_release(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_CRYPTO_LOCK_H */
