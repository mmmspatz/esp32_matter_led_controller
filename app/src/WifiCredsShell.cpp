/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bench injection of WiFi station credentials.
 *
 * Normally the only writer of the WiFi credential keys is a commissioning
 * session: the NetworkCommissioning cluster stages a network in
 * ZephyrWifiDriver and the fail-safe commit persists it. That leaves no way
 * back for a board that already holds a fabric but no credentials -- e.g. one
 * commissioned over Thread and then reflashed with the WiFi transport. Such a
 * board boots commissioned, so Server::Init takes the FabricCount() != 0
 * branch and disables BLE advertising instead of opening a commissioning
 * window, and the only route to the NetworkCommissioning cluster is the
 * operational network it cannot join. These commands break that cycle by
 * driving the same driver calls a commissioner would.
 *
 * `set` is the real thing, not a runtime shim: it stages the network, calls
 * CommitConfiguration() (the two g/wi KVS keys that ZephyrWifiDriver::Init
 * reloads at boot) and then connects, leaving storage byte-identical to a
 * WiFi commissioning. Note CHIP's own `matter wifi connect` shell command is
 * NOT equivalent -- it stages and connects but never commits, so it is lost on
 * reboot.
 *
 * Bench tooling, same as matter_prov: only present where CONFIG_SHELL is on,
 * and it takes the PSK in plaintext over the console.
 */

#include <cstring>

#include <zephyr/shell/shell.h>

#include <lib/support/Span.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ConnectivityManager.h>
#include <platform/KeyValueStoreManager.h>
#include <platform/Zephyr/wifi/ZephyrWifiDriver.h>

using chip::ByteSpan;
using chip::MutableCharSpan;
using chip::DeviceLayer::ConnectivityMgr;
using chip::DeviceLayer::PlatformMgr;
using chip::DeviceLayer::NetworkCommissioning::Network;
using chip::DeviceLayer::NetworkCommissioning::NetworkIterator;
using chip::DeviceLayer::NetworkCommissioning::Status;
using chip::DeviceLayer::NetworkCommissioning::ZephyrWifiDriver;
using chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr;

namespace
{

ByteSpan SpanOfArg(const char *arg)
{
	return ByteSpan(reinterpret_cast<const uint8_t *>(arg), strlen(arg));
}

/* Caller holds the CHIP stack lock. Returns true if a network was staged. */
bool TakeStagedNetwork(Network &out)
{
	NetworkIterator *networks = ZephyrWifiDriver::Instance().GetNetworks();
	bool configured = false;

	if (networks != nullptr) {
		configured = networks->Next(out);
		networks->Release();
	}
	return configured;
}

/*
 * The driver holds exactly one network and AddOrUpdateNetwork() rejects a
 * retarget to a different SSID with kBoundsExceeded rather than replacing it.
 * Drop the incumbent and retry so `set` is idempotent across SSID changes.
 */
Status StageNetwork(ByteSpan ssid, ByteSpan pass)
{
	auto &driver = ZephyrWifiDriver::Instance();
	uint8_t index;
	MutableCharSpan debugText;
	Status status = driver.AddOrUpdateNetwork(ssid, pass, debugText, index);
	Network staged;

	if (status != Status::kBoundsExceeded) {
		return status;
	}

	if (TakeStagedNetwork(staged)) {
		MutableCharSpan removeText;
		(void)driver.RemoveNetwork(ByteSpan(staged.networkID, staged.networkIDLen),
					   removeText, index);
	}

	MutableCharSpan retryText;
	return driver.AddOrUpdateNetwork(ssid, pass, retryText, index);
}

int cmd_set(const struct shell *sh, size_t argc, char **argv)
{
	ByteSpan ssid = SpanOfArg(argv[1]);
	ByteSpan pass = (argc == 3) ? SpanOfArg(argv[2]) : ByteSpan();
	CHIP_ERROR err = CHIP_NO_ERROR;
	Status status;

	if (ssid.empty()) {
		shell_error(sh, "usage: matter_wifi set <ssid> [<psk>]");
		return -EINVAL;
	}

	/*
	 * The driver expects the Matter thread's context (the cluster handlers
	 * call it with the stack lock held); the shell has its own thread.
	 * Holding the lock rather than ScheduleWork()ing also keeps the result
	 * reportable to the console.
	 */
	PlatformMgr().LockChipStack();

	status = StageNetwork(ssid, pass);
	if (status == Status::kSuccess) {
		err = ZephyrWifiDriver::Instance().CommitConfiguration();
		if (err == CHIP_NO_ERROR) {
			/* Async; the outcome arrives as a connectivity event. */
			ZephyrWifiDriver::Instance().ConnectNetwork(ssid, nullptr);
		}
	}

	PlatformMgr().UnlockChipStack();

	if (status != Status::kSuccess) {
		shell_error(sh, "WIFI_ERR staging rejected, status=%d",
			    static_cast<int>(status));
		return -EINVAL;
	}
	if (err != CHIP_NO_ERROR) {
		shell_error(sh, "WIFI_ERR %" CHIP_ERROR_FORMAT, err.Format());
		return -EIO;
	}

	shell_print(sh, "WIFI_OK saved ssid=%s, connecting", argv[1]);
	return 0;
}

int cmd_show(const struct shell *sh, size_t argc, char **argv)
{
	Network staged;
	bool configured, provisioned, connected;

	PlatformMgr().LockChipStack();
	configured = TakeStagedNetwork(staged);
	provisioned = ConnectivityMgr().IsWiFiStationProvisioned();
	connected = ConnectivityMgr().IsWiFiStationConnected();
	PlatformMgr().UnlockChipStack();

	/* networkID is not NUL-terminated. */
	shell_print(sh, "WIFI_SHOW ssid=%.*s provisioned=%s connected=%s",
		    configured ? static_cast<int>(staged.networkIDLen) : 6,
		    configured ? reinterpret_cast<const char *>(staged.networkID) : "<none>",
		    provisioned ? "yes" : "no", connected ? "yes" : "no");
	return 0;
}

int cmd_clear(const struct shell *sh, size_t argc, char **argv)
{
	Network staged;
	uint8_t index;

	PlatformMgr().LockChipStack();

	if (TakeStagedNetwork(staged)) {
		MutableCharSpan debugText;
		(void)ZephyrWifiDriver::Instance().RemoveNetwork(
			ByteSpan(staged.networkID, staged.networkIDLen), debugText, index);
	}

	/*
	 * Delete the keys outright rather than committing the emptied staging
	 * network: CommitConfiguration() would store zero-length values, which
	 * LoadFromStorage() reads back as a present-but-unconfigured network.
	 */
	(void)KeyValueStoreMgr().Delete(ZephyrWifiDriver::kSsidKey);
	(void)KeyValueStoreMgr().Delete(ZephyrWifiDriver::kPassKey);

	PlatformMgr().UnlockChipStack();

	shell_print(sh, "WIFI_CLEARED (effective after reboot)");
	return 0;
}

} // namespace

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_matter_wifi,
	SHELL_CMD_ARG(set, NULL, "set <ssid> [<psk>] -- persist credentials and connect", cmd_set,
		      2, 1),
	SHELL_CMD(show, NULL, "show stored SSID and station state", cmd_show),
	SHELL_CMD(clear, NULL, "erase stored WiFi credentials", cmd_clear), SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(matter_wifi, &sub_matter_wifi, "Matter WiFi station credentials", NULL);
