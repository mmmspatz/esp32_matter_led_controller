/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Owns the mapping from Matter cluster state (OnOff / LevelControl /
 * ColorControl) to PWM channel duties. Called from the Matter thread
 * (attribute-change callbacks); PWM writes are cheap and driver-locked.
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
    void SetLevel(uint8_t level);            /* LevelControl CurrentLevel, 1..254 */
    void SetColorTempMireds(uint16_t mireds); /* ColorControl ColorTemperatureMireds */

private:
    void Apply();

    bool mOn         = false;
    uint8_t mLevel   = 254;
    uint16_t mMireds = 250; /* ~4000K neutral default */
};
