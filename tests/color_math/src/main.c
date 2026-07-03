/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>

#include "color_math.h"

/* Strip endpoints used across CT tests: 6500K cool, 2700K warm. */
#define MIREDS_COOL 154
#define MIREDS_WARM 370

ZTEST_SUITE(color_math, NULL, NULL, NULL, NULL, NULL);

ZTEST(color_math, test_cie1931_endpoints)
{
	zassert_equal(cm_cie1931_brightness(0), 0.0f);
	zassert_within(cm_cie1931_brightness(254), 1.0f, 1e-5f);
}

ZTEST(color_math, test_cie1931_monotonic)
{
	float prev = -1.0f;

	for (int level = 0; level <= 254; level++) {
		float y = cm_cie1931_brightness((uint8_t)level);

		zassert_true(y > prev, "not monotonic at level %d", level);
		zassert_true(y >= 0.0f && y <= 1.0f + 1e-5f,
			     "out of range at level %d", level);
		prev = y;
	}
}

ZTEST(color_math, test_cie1931_perceptual_midpoint)
{
	/* CIE 1931: ~50% perceived (level 127) is ~18% linear duty. */
	float y = cm_cie1931_brightness(127);

	zassert_within(y, 0.18f, 0.02f);
}

ZTEST(color_math, test_ct_mix_endpoints)
{
	float warm, cool;

	cm_ct_mix(MIREDS_COOL, MIREDS_COOL, MIREDS_WARM, &warm, &cool);
	zassert_within(warm, 0.0f, 1e-6f);
	zassert_within(cool, 1.0f, 1e-6f);

	cm_ct_mix(MIREDS_WARM, MIREDS_COOL, MIREDS_WARM, &warm, &cool);
	zassert_within(warm, 1.0f, 1e-6f);
	zassert_within(cool, 0.0f, 1e-6f);
}

ZTEST(color_math, test_ct_mix_clamps_out_of_range)
{
	float warm, cool;

	cm_ct_mix(1, MIREDS_COOL, MIREDS_WARM, &warm, &cool);
	zassert_within(warm, 0.0f, 1e-6f);

	cm_ct_mix(1000, MIREDS_COOL, MIREDS_WARM, &warm, &cool);
	zassert_within(warm, 1.0f, 1e-6f);
}

ZTEST(color_math, test_ct_mix_sums_to_one)
{
	float warm, cool;

	for (uint16_t m = MIREDS_COOL; m <= MIREDS_WARM; m++) {
		cm_ct_mix(m, MIREDS_COOL, MIREDS_WARM, &warm, &cool);
		zassert_within(warm + cool, 1.0f, 1e-5f, "at %u mireds", m);
	}
}

ZTEST(color_math, test_hsv_primaries)
{
	/* Saturated hue 0 = red, ~85 = green, ~170 = blue. */
	cm_rgb_t red = cm_hsv_to_rgb(0, 254);

	zassert_equal(red.r, 255);
	zassert_true(red.g < 20 && red.b < 20, "red: %u %u %u", red.r, red.g,
		     red.b);

	cm_rgb_t green = cm_hsv_to_rgb(85, 254);

	zassert_true(green.g > 200, "green: %u %u %u", green.r, green.g,
		     green.b);
	zassert_true(green.r < 40 && green.b < 20, "green: %u %u %u", green.r,
		     green.g, green.b);

	cm_rgb_t blue = cm_hsv_to_rgb(170, 254);

	zassert_true(blue.b > 200, "blue: %u %u %u", blue.r, blue.g, blue.b);
	zassert_true(blue.r < 40 && blue.g < 20, "blue: %u %u %u", blue.r,
		     blue.g, blue.b);
}

ZTEST(color_math, test_hsv_zero_saturation_is_white)
{
	cm_rgb_t w = cm_hsv_to_rgb(123, 0);

	zassert_equal(w.r, 255);
	zassert_equal(w.g, 255);
	zassert_equal(w.b, 255);
}

ZTEST(color_math, test_xy_known_points)
{
	/* D65 white: x=0.3127, y=0.3290 -> roughly equal channels. */
	cm_rgb_t w = cm_xy_to_rgb((uint16_t)(0.3127f * 65535.0f),
				  (uint16_t)(0.3290f * 65535.0f));

	zassert_true(w.r > 200 && w.g > 200 && w.b > 200, "white: %u %u %u",
		     w.r, w.g, w.b);

	/* sRGB red primary: x=0.64, y=0.33 -> red dominates. */
	cm_rgb_t r = cm_xy_to_rgb((uint16_t)(0.64f * 65535.0f),
				  (uint16_t)(0.33f * 65535.0f));

	zassert_true(r.r > 200, "red: %u %u %u", r.r, r.g, r.b);
	zassert_true(r.r > r.g && r.r > r.b, "red: %u %u %u", r.r, r.g, r.b);

	/* sRGB blue primary: x=0.15, y=0.06 -> blue dominates. */
	cm_rgb_t b = cm_xy_to_rgb((uint16_t)(0.15f * 65535.0f),
				  (uint16_t)(0.06f * 65535.0f));

	zassert_true(b.b > 200, "blue: %u %u %u", b.r, b.g, b.b);
	zassert_true(b.b > b.r && b.b > b.g, "blue: %u %u %u", b.r, b.g, b.b);
}

ZTEST(color_math, test_xy_degenerate_y_zero)
{
	cm_rgb_t c = cm_xy_to_rgb(30000, 0);

	zassert_equal(c.r, 0);
	zassert_equal(c.g, 0);
	zassert_equal(c.b, 0);
}

ZTEST(color_math, test_ct_to_rgb_warm_vs_cool)
{
	/* 2700K (370 mireds): warm — red-heavy, blue-light. */
	cm_rgb_t warm = cm_ct_to_rgb(370);

	zassert_equal(warm.r, 255);
	zassert_true(warm.b < warm.g && warm.g < warm.r, "warm: %u %u %u",
		     warm.r, warm.g, warm.b);

	/* 6500K (154 mireds): near-white. */
	cm_rgb_t cool = cm_ct_to_rgb(154);

	zassert_true(cool.r > 200 && cool.g > 200 && cool.b > 200,
		     "cool: %u %u %u", cool.r, cool.g, cool.b);

	zassert_true(warm.b < cool.b, "blue should rise with temperature");
}
