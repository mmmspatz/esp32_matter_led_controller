/* SPDX-License-Identifier: Apache-2.0 */

#include "led_pwm.h"

#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(led_pwm, CONFIG_LOG_DEFAULT_LEVEL);

static const struct pwm_dt_spec channels[LED_PWM_NUM_CH] = {
	PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0)),
	PWM_DT_SPEC_GET(DT_ALIAS(pwm_led1)),
	PWM_DT_SPEC_GET(DT_ALIAS(pwm_led2)),
};

int led_pwm_init(void)
{
	for (int i = 0; i < LED_PWM_NUM_CH; i++) {
		if (!pwm_is_ready_dt(&channels[i])) {
			LOG_ERR("PWM channel %d not ready", i);
			return -ENODEV;
		}
	}

	/* The MOSFET gates float until the LEDC channel is configured;
	 * drive everything off before anyone else runs.
	 */
	led_pwm_all_off();
	return 0;
}

int led_pwm_set(uint8_t ch, float duty)
{
	if (ch >= LED_PWM_NUM_CH) {
		return -EINVAL;
	}
	if (duty < 0.0f) {
		duty = 0.0f;
	}
	if (duty > 1.0f) {
		duty = 1.0f;
	}

	const struct pwm_dt_spec *spec = &channels[ch];
	uint32_t pulse = (uint32_t)((float)spec->period * duty);

	return pwm_set_pulse_dt(spec, pulse);
}

void led_pwm_all_off(void)
{
	for (uint8_t i = 0; i < LED_PWM_NUM_CH; i++) {
		(void)led_pwm_set(i, 0.0f);
	}
}
