/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See color_math.h. The RGB conversions are adapted from connectedhomeip
 * examples/lighting-app/lighting-common/src/ColorFormat.cpp (Apache-2.0),
 * reworked to pure single-precision C.
 */

#include "color_math.h"

#include <math.h>

#define CLAMPF(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

float cm_cie1931_brightness(uint8_t level)
{
	if (level == 0) {
		return 0.0f;
	}

	/* CIE 1931 lightness: L* in 0..100, Y relative luminance 0..1. */
	float lstar = ((float)level / 254.0f) * 100.0f;

	if (lstar <= 8.0f) {
		return lstar / 903.3f;
	}
	float t = (lstar + 16.0f) / 116.0f;
	return t * t * t;
}

void cm_ct_mix(uint16_t mireds, uint16_t mireds_cool, uint16_t mireds_warm,
	       float *warm, float *cool)
{
	if (mireds < mireds_cool) {
		mireds = mireds_cool;
	}
	if (mireds > mireds_warm) {
		mireds = mireds_warm;
	}

	/* Linear blend in mired space between the two strip endpoints. */
	float f = ((float)mireds - (float)mireds_cool) /
		  ((float)mireds_warm - (float)mireds_cool);

	*warm = f;
	*cool = 1.0f - f;
}

cm_rgb_t cm_hsv_to_rgb(uint8_t hue, uint8_t sat)
{
	cm_rgb_t rgb;
	const uint32_t v = 255;

	if (sat == 0) {
		rgb.r = rgb.g = rgb.b = (uint8_t)v;
		return rgb;
	}

	uint32_t h = hue;
	uint32_t s = sat;
	uint32_t region = h / 43;
	uint32_t remainder = (h - (region * 43)) * 6;
	uint8_t p = (uint8_t)((v * (255 - s)) >> 8);
	uint8_t q = (uint8_t)((v * (255 - ((s * remainder) >> 8))) >> 8);
	uint8_t t = (uint8_t)((v * (255 - ((s * (255 - remainder)) >> 8))) >> 8);

	switch (region) {
	case 0:
		rgb.r = (uint8_t)v, rgb.g = t, rgb.b = p;
		break;
	case 1:
		rgb.r = q, rgb.g = (uint8_t)v, rgb.b = p;
		break;
	case 2:
		rgb.r = p, rgb.g = (uint8_t)v, rgb.b = t;
		break;
	case 3:
		rgb.r = p, rgb.g = q, rgb.b = (uint8_t)v;
		break;
	case 4:
		rgb.r = t, rgb.g = p, rgb.b = (uint8_t)v;
		break;
	case 5:
	default:
		rgb.r = (uint8_t)v, rgb.g = p, rgb.b = q;
		break;
	}

	return rgb;
}

static float srgb_gamma(float c)
{
	return c <= 0.00304f ? 12.92f * c
			     : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

cm_rgb_t cm_xy_to_rgb(uint16_t current_x, uint16_t current_y)
{
	cm_rgb_t rgb = { 0, 0, 0 };

	/* CIE xyY -> XYZ at full luminance (Y=1), D65. */
	float x = (float)current_x / 65535.0f;
	float y = (float)current_y / 65535.0f;

	if (y <= 0.0f) {
		return rgb;
	}

	float z = 1.0f - x - y;
	float Y = 1.0f;
	float X = (Y / y) * x;
	float Z = (Y / y) * z;

	/* XYZ -> linear sRGB. */
	float r = (X * 3.2410f) - (Y * 1.5374f) - (Z * 0.4986f);
	float g = -(X * 0.9692f) + (Y * 1.8760f) + (Z * 0.0416f);
	float b = (X * 0.0556f) - (Y * 0.2040f) + (Z * 1.0570f);

	/* Normalize so the dominant channel saturates rather than clips:
	 * out-of-gamut and high-luminance results otherwise flatten to
	 * white.
	 */
	float maxc = r > g ? r : g;

	maxc = maxc > b ? maxc : b;
	if (maxc > 1.0f) {
		r /= maxc;
		g /= maxc;
		b /= maxc;
	}

	r = srgb_gamma(CLAMPF(r, 0.0f, 1.0f));
	g = srgb_gamma(CLAMPF(g, 0.0f, 1.0f));
	b = srgb_gamma(CLAMPF(b, 0.0f, 1.0f));

	rgb.r = (uint8_t)(CLAMPF(r, 0.0f, 1.0f) * 255.0f);
	rgb.g = (uint8_t)(CLAMPF(g, 0.0f, 1.0f) * 255.0f);
	rgb.b = (uint8_t)(CLAMPF(b, 0.0f, 1.0f) * 255.0f);

	return rgb;
}

cm_rgb_t cm_ct_to_rgb(uint16_t mireds)
{
	cm_rgb_t rgb;
	float r, g, b;

	if (mireds == 0) {
		mireds = 1;
	}

	/* Tanner Helland's blackbody approximation, in centiKelvin:
	 * https://tannerhelland.com/2012/09/18/convert-temperature-rgb-algorithm-code.html
	 */
	float ct_ck = 10000.0f / (float)mireds;

	if (ct_ck <= 66.0f) {
		r = 255.0f;
	} else {
		r = 329.698727446f * powf(ct_ck - 60.0f, -0.1332047592f);
	}

	if (ct_ck <= 66.0f) {
		g = 99.4708025861f * logf(ct_ck) - 161.1195681661f;
	} else {
		g = 288.1221695283f * powf(ct_ck - 60.0f, -0.0755148492f);
	}

	if (ct_ck >= 66.0f) {
		b = 255.0f;
	} else if (ct_ck <= 19.0f) {
		b = 0.0f;
	} else {
		b = 138.5177312231f * logf(ct_ck - 10.0f) - 305.0447927307f;
	}

	rgb.r = (uint8_t)CLAMPF(r, 0.0f, 255.0f);
	rgb.g = (uint8_t)CLAMPF(g, 0.0f, 255.0f);
	rgb.b = (uint8_t)CLAMPF(b, 0.0f, 255.0f);

	return rgb;
}
