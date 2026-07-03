/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure color/brightness math: no Zephyr, no CHIP, no globals. Everything
 * here is unit-tested on native_sim (tests/color_math). Inputs use Matter
 * cluster attribute encodings (CurrentLevel 1..254, hue/sat 0..254,
 * currentX/Y 0..65535, mireds).
 */

#ifndef COLOR_MATH_H
#define COLOR_MATH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint8_t r;
	uint8_t g;
	uint8_t b;
} cm_rgb_t;

/*
 * CIE 1931 lightness curve: perceived brightness (LevelControl
 * CurrentLevel, 0..254) to linear PWM duty fraction (0.0..1.0).
 */
float cm_cie1931_brightness(uint8_t level);

/*
 * Mix a color temperature onto a 2-channel (warm + cool white) strip.
 * `mireds` is clamped to [mireds_cool, mireds_warm] (cool = smaller value).
 * The two outputs are linear channel weights in 0.0..1.0 that sum to 1.0;
 * scale them by brightness to get PWM duty.
 */
void cm_ct_mix(uint16_t mireds, uint16_t mireds_cool, uint16_t mireds_warm, float *warm,
	       float *cool);

/*
 * ColorControl color conversions for a 3-channel RGB strip, at full
 * brightness; scale the result by cm_cie1931_brightness(). Adapted from
 * connectedhomeip examples/lighting-app/lighting-common/src/ColorFormat.cpp
 * (Apache-2.0).
 */
cm_rgb_t cm_hsv_to_rgb(uint8_t hue, uint8_t sat);
cm_rgb_t cm_xy_to_rgb(uint16_t current_x, uint16_t current_y);
cm_rgb_t cm_ct_to_rgb(uint16_t mireds);

#ifdef __cplusplus
}
#endif

#endif /* COLOR_MATH_H */
