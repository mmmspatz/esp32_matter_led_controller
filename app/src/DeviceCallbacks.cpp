/* SPDX-License-Identifier: Apache-2.0 */

#include "DeviceCallbacks.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <lib/support/logging/CHIPLogging.h>

#include "LightingManager.h"

using namespace ::chip;
using namespace ::chip::app::Clusters;

namespace
{
constexpr EndpointId kLightEndpointId = 1;
} // namespace

void LedCtrl::DeviceCallbacks::PostAttributeChangeCallback(EndpointId endpoint, ClusterId clusterId, AttributeId attributeId,
                                                           uint8_t type, uint16_t size, uint8_t * value)
{
    VerifyOrReturn(endpoint == kLightEndpointId && value != nullptr);

    auto & lighting = LightingManager::Instance();

    switch (clusterId)
    {
    case OnOff::Id:
        if (attributeId == OnOff::Attributes::OnOff::Id)
        {
            lighting.SetOnOff(*value != 0);
        }
        break;
    case LevelControl::Id:
        if (attributeId == LevelControl::Attributes::CurrentLevel::Id)
        {
            lighting.SetLevel(*value);
        }
        break;
    case ColorControl::Id:
        if (attributeId == ColorControl::Attributes::ColorTemperatureMireds::Id && size == sizeof(uint16_t))
        {
            uint16_t mireds;
            memcpy(&mireds, value, sizeof(mireds));
            lighting.SetColorTempMireds(mireds);
        }
#ifdef CONFIG_LEDCTRL_MODE_RGB
        else if (attributeId == ColorControl::Attributes::CurrentHue::Id)
        {
            lighting.SetHue(*value);
        }
        else if (attributeId == ColorControl::Attributes::CurrentSaturation::Id)
        {
            lighting.SetSaturation(*value);
        }
        else if ((attributeId == ColorControl::Attributes::CurrentX::Id ||
                  attributeId == ColorControl::Attributes::CurrentY::Id) &&
                 size == sizeof(uint16_t))
        {
            /* X and Y arrive as separate writes; read the sibling from the
             * cluster so each update is applied as a consistent pair.
             */
            uint16_t x, y;
            if (ColorControl::Attributes::CurrentX::Get(endpoint, &x) ==
                    chip::Protocols::InteractionModel::Status::Success &&
                ColorControl::Attributes::CurrentY::Get(endpoint, &y) ==
                    chip::Protocols::InteractionModel::Status::Success)
            {
                lighting.SetXy(x, y);
            }
        }
        else if (attributeId == ColorControl::Attributes::ColorMode::Id)
        {
            lighting.SetColorMode(*value);
        }
#endif
        break;
    default:
        break;
    }
}

chip::DeviceManager::CHIPDeviceManagerCallbacks & chip::Zephyr::App::GetDeviceCallbacks()
{
    static LedCtrl::DeviceCallbacks sDeviceCallbacks;
    return sDeviceCallbacks;
}
