/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * M0 board bring-up: cycle each LED channel through a CIE-curve fade and
 * log S1 button events. Replaced by the Matter application at milestone M2;
 * led_pwm.c and color_math.c carry forward unchanged.
 */

#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "color_math.h"
#include "led_pwm.h"

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

static volatile bool paused;

static void button_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type == INPUT_EV_KEY && evt->code == INPUT_KEY_0) {
		LOG_INF("S1 button %s", evt->value ? "pressed" : "released");
		if (evt->value) {
			paused = !paused;
			LOG_INF("fade %s", paused ? "paused" : "resumed");
		}
	}
}
INPUT_CALLBACK_DEFINE(NULL, button_cb, NULL);

int main(void)
{
	LOG_INF("BTF WLED ESP32 bring-up (M0)");

	if (led_pwm_init() != 0) {
		LOG_ERR("led_pwm_init failed");
		return -1;
	}

	uint8_t ch = 0;

	while (true) {
		LOG_INF("fading CH%d", ch + 1);

		for (int level = 0; level <= 508; level += 2) {
			/* 0..254 up then back down */
			uint8_t l = level <= 254 ? (uint8_t)level
						 : (uint8_t)(508 - level);

			led_pwm_set(ch, cm_cie1931_brightness(l));
			k_sleep(K_MSEC(8));

			while (paused) {
				k_sleep(K_MSEC(50));
			}
		}

		led_pwm_set(ch, 0.0f);
		ch = (ch + 1) % LED_PWM_NUM_CH;
	}

	return 0;
}
