/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Zephyr stand-in for ESP-IDF's esp_crypto_periph_clk.h, covering only
 * what hal_espressif's esp_ecc.c uses. Implemented in esp_crypto_shims.c.
 */

#ifndef ESP_CRYPTO_PERIPH_CLK_H
#define ESP_CRYPTO_PERIPH_CLK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void esp_crypto_ecc_enable_periph_clk(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* ESP_CRYPTO_PERIPH_CLK_H */
