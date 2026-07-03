/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The only module that knows the LED channels are LEDC PWM outputs.
 * Channel indices follow the board silkscreen: 0=CH1(GPIO27),
 * 1=CH2(GPIO26), 2=CH3(GPIO25).
 */

#ifndef LED_PWM_H
#define LED_PWM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LED_PWM_NUM_CH 3

/* Returns 0 on success; asserts all channels are device_is_ready. */
int led_pwm_init(void);

/* duty is a linear fraction 0.0..1.0 (caller applies any dimming curve). */
int led_pwm_set(uint8_t ch, float duty);

/* Convenience: all channels off. */
void led_pwm_all_off(void);

#ifdef __cplusplus
}
#endif

#endif /* LED_PWM_H */
