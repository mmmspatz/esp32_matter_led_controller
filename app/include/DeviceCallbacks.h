/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "CHIPDeviceManager.h"
#include "CommonDeviceCallbacks.h"

namespace LedCtrl {

class DeviceCallbacks : public chip::Zephyr::App::CommonDeviceCallbacks
{
public:
    void PostAttributeChangeCallback(chip::EndpointId endpoint, chip::ClusterId clusterId, chip::AttributeId attributeId,
                                     uint8_t type, uint16_t size, uint8_t * value) override;
};

} // namespace LedCtrl

namespace chip::Zephyr::App {

/* Scaffold hook: AppTaskBase::Init registers this with CHIPDeviceManager. */
chip::DeviceManager::CHIPDeviceManagerCallbacks & GetDeviceCallbacks();

} // namespace chip::Zephyr::App
