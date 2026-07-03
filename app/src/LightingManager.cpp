/* SPDX-License-Identifier: Apache-2.0 */

#include "LightingManager.h"

#include <zephyr/logging/log.h>

#include "color_math.h"
#include "led_pwm.h"

LOG_MODULE_REGISTER(lighting, CONFIG_CHIP_APP_LOG_LEVEL);

LightingManager & LightingManager::Instance()
{
    static LightingManager sInstance;
    return sInstance;
}

void LightingManager::Init()
{
    led_pwm_init();
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
    Apply();
}

void LightingManager::Apply()
{
    if (!mOn)
    {
        led_pwm_all_off();
        return;
    }

    float brightness = cm_cie1931_brightness(mLevel);

#if defined(CONFIG_LEDCTRL_MODE_CCT)
    float warm, cool;
    cm_ct_mix(mMireds, CONFIG_LEDCTRL_MIREDS_COOL, CONFIG_LEDCTRL_MIREDS_WARM, &warm, &cool);
    led_pwm_set(CONFIG_LEDCTRL_CH_WARM, warm * brightness);
    led_pwm_set(CONFIG_LEDCTRL_CH_COOL, cool * brightness);
#else
    /* RGB mode grows XY/HS handling with the rgb_light data model (M4);
     * until then CT is emulated onto the RGB channels.
     */
    cm_rgb_t rgb = cm_ct_to_rgb(mMireds);
    led_pwm_set(0, ((float)rgb.r / 255.0f) * brightness);
    led_pwm_set(1, ((float)rgb.g / 255.0f) * brightness);
    led_pwm_set(2, ((float)rgb.b / 255.0f) * brightness);
#endif

    LOG_DBG("on=%d level=%u mireds=%u", mOn, mLevel, mMireds);
}
