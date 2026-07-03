/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Owns the mapping from Matter cluster state (OnOff / LevelControl /
 * ColorControl) to PWM channel duties. Called from the Matter thread
 * (attribute-change callbacks); the actual PWM writes happen on the
 * system workqueue through a short fade smoother.
 */

#pragma once

#include <cstdint>

class LightingManager
{
public:
    static LightingManager & Instance();

    /* Drives all channels off; call before the Matter stack starts. */
    void Init();

    void SetOnOff(bool on);
    void SetLevel(uint8_t level);             /* LevelControl CurrentLevel, 1..254 */
    void SetColorTempMireds(uint16_t mireds); /* ColorControl ColorTemperatureMireds */

#ifdef CONFIG_LEDCTRL_MODE_RGB
    /* ColorControl ColorMode / EnhancedColorMode values */
    enum ColorMode : uint8_t
    {
        kHueSaturation = 0,
        kXy            = 1,
        kColorTemp     = 2,
    };

    void SetColorMode(uint8_t mode);
    void SetHue(uint8_t hue);
    void SetSaturation(uint8_t sat);
    void SetXy(uint16_t x, uint16_t y);
#endif

private:
    void Apply();

    bool mOn         = false;
    uint8_t mLevel   = 254;
    uint16_t mMireds = 250; /* ~4000K neutral default */

#ifdef CONFIG_LEDCTRL_MODE_RGB
    uint8_t mColorMode = kColorTemp;
    uint8_t mHue       = 0;
    uint8_t mSat       = 0;
    uint16_t mX        = 20500; /* ~D65 white */
    uint16_t mY        = 21500;
#endif
};
