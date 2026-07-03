/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CHIP's DeviceLayer Entropy seeds libc rand() with srand(). Picolibc
 * implements srand()/rand() on top of srandom()/random(), but random() is
 * also defined (strongly) by hal_espressif for the WiFi blob (since the
 * ESP-IDF-components restructuring: components/esp_wifi/esp32/
 * esp_adapter.c), so pulling picolibc's random.o is a multiple-definition
 * error. Defining rand()/srand() here keeps picolibc's chain out of the
 * link entirely. Non-cryptographic by contract (crypto goes through
 * mbedTLS entropy), so a plain C99 LCG suffices.
 */

#include <stdlib.h>

static unsigned long rand_state = 1;

void srand(unsigned int seed)
{
	rand_state = seed;
}

int rand(void)
{
	rand_state = rand_state * 1103515245UL + 12345UL;
	return (int)((rand_state >> 16) & RAND_MAX);
}
