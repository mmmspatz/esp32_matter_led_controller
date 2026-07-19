/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Lock and peripheral-clock providers for hal_espressif's esp_ecc.c, which
 * is written against ESP-IDF services the Zephyr port doesn't carry. The
 * ECC engine is a single shared block; every PSA entry point reaches it
 * through esp_ecc.c's acquire/release, so a mutex (all callers are thread
 * context) plus PCR clock gating is the whole story.
 */

#include <zephyr/kernel.h>

#include <hal/ecc_ll.h>

#include "esp_crypto_lock.h"
#include "esp_crypto_periph_clk.h"

static K_MUTEX_DEFINE(ecc_lock);

void esp_crypto_ecc_lock_acquire(void)
{
	k_mutex_lock(&ecc_lock, K_FOREVER);
}

void esp_crypto_ecc_lock_release(void)
{
	k_mutex_unlock(&ecc_lock);
}

void esp_crypto_ecc_enable_periph_clk(bool enable)
{
	if (enable) {
		ecc_ll_enable_bus_clock(true);
		/* Cheap, and leaves the engine in a known state regardless
		 * of what earlier boot stages did with it.
		 */
		ecc_ll_reset_register();
		ecc_ll_power_up();
	} else {
		ecc_ll_enable_bus_clock(false);
	}
}
