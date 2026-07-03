/* SPDX-License-Identifier: Apache-2.0 */

#include "LightingManager.h"

#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "color_math.h"
#include "led_pwm.h"

LOG_MODULE_REGISTER(lighting, CONFIG_CHIP_APP_LOG_LEVEL);

/*
 * The ColorControl/LevelControl cluster servers tick transitions every
 * 100ms (TRANSITION_UPDATE_TIME_MS), which reads as visible stepping if
 * each attribute write lands on the PWM directly. Smooth over it: every
 * tick of a 20ms work item, each channel moves a fixed fraction of its
 * remaining distance to target (~80ms time constant), turning the 10Hz
 * staircase into a glide and giving on/off a soft edge for free.
 */
namespace
{
constexpr int kFadeIntervalMs = 20;
constexpr float kFadeAlpha = 0.22f;
constexpr float kSettled = 0.0005f;

float sCurrent[LED_PWM_NUM_CH];
float sTarget[LED_PWM_NUM_CH];
struct k_work_delayable sFadeWork;

void FadeWorkHandler(struct k_work *work)
{
	bool active = false;

	for (uint8_t ch = 0; ch < LED_PWM_NUM_CH; ch++) {
		float delta = sTarget[ch] - sCurrent[ch];
		if (fabsf(delta) < kSettled) {
			sCurrent[ch] = sTarget[ch];
		} else {
			sCurrent[ch] += delta * kFadeAlpha;
			active = true;
		}
		led_pwm_set(ch, sCurrent[ch]);
	}

	if (active) {
		k_work_schedule(&sFadeWork, K_MSEC(kFadeIntervalMs));
	}
}

void SetChannelTargets(const float *targets)
{
	for (uint8_t ch = 0; ch < LED_PWM_NUM_CH; ch++) {
		sTarget[ch] = targets[ch];
	}
	k_work_schedule(&sFadeWork, K_NO_WAIT);
}
} // namespace

LightingManager &LightingManager::Instance()
{
	static LightingManager sInstance;
	return sInstance;
}

void LightingManager::Init()
{
	led_pwm_init();
	k_work_init_delayable(&sFadeWork, FadeWorkHandler);
}

void LightingManager::SetOnOff(bool on)
{
	mOn = on;
	Apply();
}

void LightingManager::SetLevel(uint8_t level)
{
	mLevel = level;
	Apply();
}

void LightingManager::SetColorTempMireds(uint16_t mireds)
{
	mMireds = mireds;
#ifdef CONFIG_LEDCTRL_MODE_RGB
	mColorMode = kColorTemp;
#endif
	Apply();
}

#ifdef CONFIG_LEDCTRL_MODE_RGB
void LightingManager::SetColorMode(uint8_t mode)
{
	mColorMode = mode;
	Apply();
}

void LightingManager::SetHue(uint8_t hue)
{
	mHue = hue;
	mColorMode = kHueSaturation;
	Apply();
}

void LightingManager::SetSaturation(uint8_t sat)
{
	mSat = sat;
	mColorMode = kHueSaturation;
	Apply();
}

void LightingManager::SetXy(uint16_t x, uint16_t y)
{
	mX = x;
	mY = y;
	mColorMode = kXy;
	Apply();
}
#endif

void LightingManager::Apply()
{
	float targets[LED_PWM_NUM_CH] = {0.0f, 0.0f, 0.0f};

	if (mOn) {
		float brightness = cm_cie1931_brightness(mLevel);

#if defined(CONFIG_LEDCTRL_MODE_CCT)
		float warm, cool;
		cm_ct_mix(mMireds, CONFIG_LEDCTRL_MIREDS_COOL, CONFIG_LEDCTRL_MIREDS_WARM, &warm,
			  &cool);
		targets[CONFIG_LEDCTRL_CH_WARM] = warm * brightness;
		targets[CONFIG_LEDCTRL_CH_COOL] = cool * brightness;
#else
		cm_rgb_t rgb;
		switch (mColorMode) {
		case kHueSaturation:
			rgb = cm_hsv_to_rgb(mHue, mSat);
			break;
		case kXy:
			rgb = cm_xy_to_rgb(mX, mY);
			break;
		case kColorTemp:
		default:
			rgb = cm_ct_to_rgb(mMireds);
			break;
		}
		targets[0] = ((float)rgb.r / 255.0f) * brightness;
		targets[1] = ((float)rgb.g / 255.0f) * brightness;
		targets[2] = ((float)rgb.b / 255.0f) * brightness;
#endif
	}

	SetChannelTargets(targets);
	LOG_DBG("on=%d level=%u mireds=%u", mOn, mLevel, mMireds);
}
