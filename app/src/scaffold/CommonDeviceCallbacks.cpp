/*
 *
 *    Copyright (c) 2025 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 * @file CommonDeviceCallbacks.cpp
 *
 * Implements all the callbacks to the application from the CHIP Stack
 *
 **/
#include "CommonDeviceCallbacks.h"
#include "AppTaskBase.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/server/Dnssd.h>
#include <app/util/attribute-storage.h>
#include <app/util/attribute-table.h>

#include <lib/support/CodeUtils.h>
#if CHIP_ENABLE_OPENTHREAD && CHIP_DEVICE_CONFIG_CHIPOBLE_DISABLE_ADVERTISING_WHEN_PROVISIONED
#include "openthread-system.h"
#endif /* CHIP_ENABLE_OPENTHREAD && CHIP_DEVICE_CONFIG_CHIPOBLE_DISABLE_ADVERTISING_WHEN_PROVISIONED */

#if CHIP_DEVICE_CONFIG_ENABLE_OTA_REQUESTOR
#include "OTARequestorInitiator.h"
#endif

#ifdef CONFIG_LEDCTRL_RECLAIM_BT_DRAM_AFTER_COMMISSIONING
#include <platform/Zephyr/SysHeapMalloc.h>
#include <platform/internal/BLEManager.h>

#include <zephyr/kernel.h>
#include <esp_bt.h>
#endif

using namespace chip::app;
using namespace ::chip;
using namespace ::chip::Inet;
using namespace ::chip::System;
using namespace ::chip::DeviceLayer;

#ifdef CONFIG_LEDCTRL_RECLAIM_BT_DRAM_AFTER_COMMISSIONING
namespace {

// Reclaim the ESP32 BT-controller DRAM reserve once BLE-based commissioning is
// done. When the device is fully provisioned and on WiFi and the commissioner
// has dropped the BLE link, tear the controller down (bt_disable, via
// BLEMgr().Shutdown()) and hand the CONFIG_ESP32_BT_RESERVE_DRAM window to the
// CHIP heap as a second bank. One-way: BLE cannot return without a reboot (see
// chip-patches 0005/0006). Net gain is only the ~54.8 KiB linker reserve.

constexpr uint32_t kBleReclaimRetryIntervalSec = 2;
constexpr int kBleReclaimMaxRetries            = 15;

// Linker symbol for the bottom of the app's dram0 segment == one past the top
// of the BT-controller reserve. The reserve window is
// [procpu_dram0_org - CONFIG_ESP32_BT_RESERVE_DRAM, procpu_dram0_org).
extern "C" uint8_t procpu_dram0_org[];

BUILD_ASSERT(CONFIG_ESP32_BT_RESERVE_DRAM >= 4096, "BT reserve unexpectedly small");

bool sBleReclaimDone   = false;
int sBleReclaimRetries = 0;

void ReclaimBtDramWork(intptr_t);

void ReclaimRetryTimer(chip::System::Layer *, void *)
{
    ReclaimBtDramWork(0);
}

void ReclaimBtDramWork(intptr_t)
{
    VerifyOrReturn(!sBleReclaimDone);

    // Don't tear BLE down while the commissioner still holds the PASE link.
    if (chip::DeviceLayer::Internal::BLEMgr().NumConnections() != 0)
    {
        if (++sBleReclaimRetries > kBleReclaimMaxRetries)
        {
            ChipLogProgress(DeviceLayer, "BT reclaim: BLE still connected after %d retries; proceeding",
                            kBleReclaimMaxRetries);
        }
        else
        {
            LogErrorOnFailure(SystemLayer().StartTimer(System::Clock::Seconds32(kBleReclaimRetryIntervalSec),
                                                       ReclaimRetryTimer, nullptr));
            return;
        }
    }

    Malloc::Stats stats;
    size_t freeBefore = (Malloc::GetStats(stats) == CHIP_NO_ERROR) ? stats.free : 0;

    // Synchronous on this port: BleLayer shutdown + bt_disable() aborts the BT
    // RX workqueue and deinits the controller to IDLE.
    chip::DeviceLayer::Internal::BLEMgr().Shutdown();

    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE)
    {
        ChipLogError(DeviceLayer, "BT reclaim: controller not IDLE after shutdown; skipping");
        sBleReclaimDone = true;
        return;
    }

    // Do the math in uintptr_t: pointer arithmetic on the linker symbol (an
    // array type) would look like a negative index to -Werror=array-bounds.
    const uintptr_t org = reinterpret_cast<uintptr_t>(procpu_dram0_org);
    void * base         = reinterpret_cast<void *>(org - CONFIG_ESP32_BT_RESERVE_DRAM);
    __ASSERT(reinterpret_cast<uintptr_t>(base) == 0x3ffb0000u, "BT reserve base moved to 0x%lx",
             (unsigned long) reinterpret_cast<uintptr_t>(base));

    CHIP_ERROR err = Malloc::AddReclaimedRegion(base, CONFIG_ESP32_BT_RESERVE_DRAM);
    sBleReclaimDone = true;
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "BT reclaim: AddReclaimedRegion failed: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    size_t freeAfter = (Malloc::GetStats(stats) == CHIP_NO_ERROR) ? stats.free : 0;
    ChipLogProgress(DeviceLayer, "BT reclaim: added %u bytes at %p; heap free %u -> %u",
                    (unsigned) CONFIG_ESP32_BT_RESERVE_DRAM, (void *) base, (unsigned) freeBefore,
                    (unsigned) freeAfter);
}

void MaybeStartBtReclaim()
{
    VerifyOrReturn(!sBleReclaimDone);
    // The only precondition is that the device is provisioned: once it is, BLE
    // is no longer needed for initial commissioning, and its teardown is
    // independent of WiFi state. We deliberately do NOT gate on
    // IsWiFiStationConnected() -- that lags the kWiFiConnectivityChange event
    // (it flips only once WiFiManager reaches CONNECTED), so gating on it here
    // would miss the already-provisioned-reboot trigger. During a fresh
    // commission the WiFi-established trigger fires before provisioning
    // completes and bails here; kCommissioningComplete drives it instead.
    VerifyOrReturn(ConfigurationMgr().IsFullyProvisioned());

    sBleReclaimRetries = 0;
    LogErrorOnFailure(PlatformMgr().ScheduleWork(ReclaimBtDramWork, 0));
}

} // namespace
#endif // CONFIG_LEDCTRL_RECLAIM_BT_DRAM_AFTER_COMMISSIONING

void chip::Zephyr::App::CommonDeviceCallbacks::DeviceEventCallback(const ChipDeviceEvent * event, intptr_t arg)
{
    switch (event->Type)
    {
    case DeviceEventType::kWiFiConnectivityChange:
        OnWiFiConnectivityChange(event);
        break;

    case DeviceEventType::kInternetConnectivityChange:
        OnInternetConnectivityChange(event);
        break;

    case DeviceEventType::kInterfaceIpAddressChanged:
#if !CHIP_ENABLE_OPENTHREAD // No need to do this for OT mDNS server
        if ((event->InterfaceIpAddressChanged.Type == InterfaceIpChangeType::kIpV4_Assigned) ||
            (event->InterfaceIpAddressChanged.Type == InterfaceIpChangeType::kIpV6_Assigned))
        {
            // MDNS server restart on any ip assignment: if link local ipv6 is configured, that
            // will not trigger a 'internet connectivity change' as there is no internet
            // connectivity. MDNS still wants to refresh its listening interfaces to include the
            // newly selected address.
            chip::app::DnssdServer::Instance().StartServer();
        }
#endif
        OnInterfaceIpAddressChanged(event);
        break;

    case DeviceEventType::kCommissioningComplete:
#if CHIP_ENABLE_OPENTHREAD
        CommonDeviceCallbacks::OnCommissioningComplete(event);
#endif
#ifdef CONFIG_LEDCTRL_RECLAIM_BT_DRAM_AFTER_COMMISSIONING
        MaybeStartBtReclaim();
#endif
        break;
#if CHIP_ENABLE_OPENTHREAD
#if CHIP_DEVICE_CONFIG_ENABLE_WPA
    case DeviceEventType::kThreadConnectivityChange:
        if (event->ThreadConnectivityChange.Result == kConnectivity_Established)
        {
            ChipLogProgress(DeviceLayer, "Restarting Dnssd server for Thread connectivity change");
            // Restart DnsSd service when operating as Matter over Thread
            chip::app::DnssdServer::Instance().StartServer();
        }
        else if (event->ThreadConnectivityChange.Result == kConnectivity_Lost)
        {
            ChipLogProgress(DeviceLayer, "Thread connection lost");
        }
        break;
#endif // CHIP_DEVICE_CONFIG_ENABLE_WPA
#endif // CHIP_ENABLE_OPENTHREAD
    case DeviceLayer::DeviceEventType::kDnssdInitialized:
#if CHIP_DEVICE_CONFIG_ENABLE_OTA_REQUESTOR
        ChipLogProgress(DeviceLayer, "kDnssdInitialized");
        /* Initialize OTA Requestor */
        OTARequestorInitiator::Instance().InitOTA(reinterpret_cast<intptr_t>(&OTARequestorInitiator::Instance()));
#endif
        break;
    }
}

void chip::Zephyr::App::CommonDeviceCallbacks::OnWiFiConnectivityChange(const ChipDeviceEvent * event)
{
    if (event->WiFiConnectivityChange.Result == kConnectivity_Established)
    {
        ChipLogProgress(DeviceLayer, "WiFi connection established");
#ifdef CONFIG_LEDCTRL_RECLAIM_BT_DRAM_AFTER_COMMISSIONING
        // Belt-and-suspenders: if WiFi comes up (or re-establishes) after the
        // device is already provisioned, this drives the one-shot reclaim too.
        MaybeStartBtReclaim();
#endif
    }
    else if (event->WiFiConnectivityChange.Result == kConnectivity_Lost)
    {
        ChipLogProgress(DeviceLayer, "WiFi connection lost");
    }
}

void chip::Zephyr::App::CommonDeviceCallbacks::OnInternetConnectivityChange(const ChipDeviceEvent * event)
{
    if (event->InternetConnectivityChange.IPv4 == kConnectivity_Established)
    {
        char ip_addr[Inet::IPAddress::kMaxStringLength];
        event->InternetConnectivityChange.ipAddress.ToString(ip_addr);
        ChipLogProgress(DeviceLayer, "Server ready at: %s:%d", ip_addr, CHIP_PORT);
#if !CHIP_ENABLE_OPENTHREAD // No need to do this for OT mDNS server
        chip::app::DnssdServer::Instance().StartServer();
#endif
    }
    else if (event->InternetConnectivityChange.IPv4 == kConnectivity_Lost)
    {
        ChipLogProgress(DeviceLayer, "Lost IPv4 connectivity...");
    }
    if (event->InternetConnectivityChange.IPv6 == kConnectivity_Established)
    {
        char ip_addr[Inet::IPAddress::kMaxStringLength];
        event->InternetConnectivityChange.ipAddress.ToString(ip_addr);
        ChipLogProgress(DeviceLayer, "IPv6 Server ready at: [%s]:%d", ip_addr, CHIP_PORT);

        chip::app::DnssdServer::Instance().StartServer();
    }
    else if (event->InternetConnectivityChange.IPv6 == kConnectivity_Lost)
    {
        ChipLogProgress(DeviceLayer, "Lost IPv6 connectivity...");
    }
}

void chip::Zephyr::App::CommonDeviceCallbacks::OnInterfaceIpAddressChanged(const ChipDeviceEvent * event)
{
    switch (event->InterfaceIpAddressChanged.Type)
    {
    case InterfaceIpChangeType::kIpV4_Assigned:
        ChipLogProgress(DeviceLayer, "Interface IPv4 address assigned");
        break;
    case InterfaceIpChangeType::kIpV4_Lost:
        ChipLogProgress(DeviceLayer, "Interface IPv4 address lost");
        break;
    case InterfaceIpChangeType::kIpV6_Assigned:
        ChipLogProgress(DeviceLayer, "Interface IPv6 address assigned");
        break;
    case InterfaceIpChangeType::kIpV6_Lost:
        ChipLogProgress(DeviceLayer, "Interface IPv6 address lost");
        break;
    }
}

void chip::Zephyr::App::CommonDeviceCallbacks::OnSessionEstablished(chip::DeviceLayer::ChipDeviceEvent const *)
{
    /* Empty */
}

#if CHIP_ENABLE_OPENTHREAD
void chip::Zephyr::App::CommonDeviceCallbacks::OnCommissioningComplete(const chip::DeviceLayer::ChipDeviceEvent * event)
{
#if CHIP_DEVICE_CONFIG_ENABLE_WPA
    if (!ConnectivityMgr().IsWiFiStationConnected() && ConnectivityMgr().IsThreadProvisioned())
    {
        // Set WIFI cluster interface attribute to disable.
        app::Clusters::NetworkCommissioning::Attributes::InterfaceEnabled::Set(0, 0);
    }
#endif // CHIP_DEVICE_CONFIG_ENABLE_WPA

#if CHIP_DEVICE_CONFIG_CHIPOBLE_DISABLE_ADVERTISING_WHEN_PROVISIONED
    /*
     * If a transceiver supporting a multiprotocol scenario is used, a check of the provisioning state is required,
     * so that we can inform the transceiver to stop BLE to give the priority to another protocol.
     * For example it is the case when a K32W0 transceiver supporting OT+BLE+Zigbee is used. When the device is already provisioned,
     * BLE is no more required and the transceiver needs to be informed so that Zigbee can be switched on and BLE switched off.
     *
     * If a transceiver does not support such vendor property the cmd would be ignored.
     */
    if (ConfigurationMgr().IsFullyProvisioned())
    {
        ChipLogDetail(DeviceLayer, "Provisioning complete, stopping BLE\n");
        ThreadStackMgrImpl().LockThreadStack();
        PlatformMgrImpl().StopBLEConnectivity();
        ThreadStackMgrImpl().UnlockThreadStack();
    }
#endif
}
#endif
