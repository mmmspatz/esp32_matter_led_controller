/* SPDX-License-Identifier: Apache-2.0 */

#include "AppTask.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "LightingManager.h"

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace chip;
using namespace chip::DeviceLayer;

AppTask AppTask::sAppTask;

/* -------------------------------------------------------------------------- */
/*                        Button S1 (GPIO0, active-low)                       */
/* -------------------------------------------------------------------------- */

#if DT_NODE_EXISTS(DT_ALIAS(sw0))
#define HAS_BUTTON0 1
static const struct gpio_dt_spec sButton0 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback sButton0CbData;
static struct k_work_delayable sFactoryResetWarningWork;
static struct k_work_delayable sFactoryResetTriggerWork;

static void FactoryResetWarningWorkHandler(struct k_work *work)
{
	LOG_INF("Keep holding to factory reset in 3 seconds. Release to cancel.");
}

static void FactoryResetTriggerWorkHandler(struct k_work *work)
{
	LOG_INF("Factory reset triggered");
	chip::Server::GetInstance().ScheduleFactoryReset();
}

static void Button0Handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	if (gpio_pin_get_dt(&sButton0) > 0) {
		k_work_schedule(&sFactoryResetWarningWork, K_SECONDS(2));
		k_work_schedule(&sFactoryResetTriggerWork, K_SECONDS(5));
	} else {
		if (k_work_cancel_delayable(&sFactoryResetTriggerWork) != 0) {
			LOG_INF("Factory reset canceled");
		}
		k_work_cancel_delayable(&sFactoryResetWarningWork);
	}
}

static void ButtonInit()
{
	if (!gpio_is_ready_dt(&sButton0)) {
		LOG_ERR("Button device not ready");
		return;
	}
	gpio_pin_configure_dt(&sButton0, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&sButton0, GPIO_INT_EDGE_BOTH);
	gpio_init_callback(&sButton0CbData, Button0Handler, BIT(sButton0.pin));
	gpio_add_callback(sButton0.port, &sButton0CbData);
	k_work_init_delayable(&sFactoryResetWarningWork, FactoryResetWarningWorkHandler);
	k_work_init_delayable(&sFactoryResetTriggerWork, FactoryResetTriggerWorkHandler);
}
#endif /* DT_NODE_EXISTS(DT_ALIAS(sw0)) */

/* -------------------------------------------------------------------------- */
/*                              AppTask hooks                                 */
/* -------------------------------------------------------------------------- */

void AppTask::PreInitMatterStack()
{
	/* MOSFET gates float at reset; drive every channel off before anything
	 * else runs.
	 */
	LightingManager::Instance().Init();

#ifdef HAS_BUTTON0
	ButtonInit();
#endif
}

void AppTask::PostInitMatterServerInstance()
{
	/* OnOff/Level/ColorTemp persist across reboot and are restored by the
	 * cluster servers WITHOUT firing attribute-change callbacks (and a
	 * command that doesn't change an attribute never fires one either).
	 * Prime LightingManager from live cluster state so its cache can't
	 * start out disagreeing with the data model.
	 */
	using namespace chip::app::Clusters;
	constexpr chip::EndpointId kLight = 1;

	auto &lighting = LightingManager::Instance();

	chip::app::DataModel::Nullable<uint8_t> level;
	if (LevelControl::Attributes::CurrentLevel::Get(kLight, level) ==
		    chip::Protocols::InteractionModel::Status::Success &&
	    !level.IsNull()) {
		lighting.SetLevel(level.Value());
	}

	uint16_t mireds;
	if (ColorControl::Attributes::ColorTemperatureMireds::Get(kLight, &mireds) ==
	    chip::Protocols::InteractionModel::Status::Success) {
		lighting.SetColorTempMireds(mireds);
	}

	bool isOn = false;
	if (OnOff::Attributes::OnOff::Get(kLight, &isOn) ==
	    chip::Protocols::InteractionModel::Status::Success) {
		lighting.SetOnOff(isOn);
	}

	LOG_INF("LED controller ready (on=%d)", isOn);
}

AppTask &AppTask::GetDefaultInstance()
{
	return sAppTask;
}

/* Declared in AppTaskBase.h; the scaffold's main() enters through this. */
chip::Zephyr::App::AppTaskBase &chip::Zephyr::App::GetAppTask()
{
	return AppTask::GetDefaultInstance();
}
