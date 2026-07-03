/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bench provisioning of per-device commissionable data (pairing code).
 *
 * The generic Zephyr platform's commissionable-data provider reads the
 * "chip-fct" settings keys before falling back to the compile-time test
 * values, and factory reset deliberately leaves that namespace alone.
 * These shell commands write those keys using CHIP's own ZephyrConfig
 * serialization, so there is no format to keep in sync. Driven by
 * scripts/provision.py over the USB console; only present in builds
 * with CONFIG_LEDCTRL_PROV_SHELL (see prov.conf).
 */

#include <cstdlib>

#include <zephyr/shell/shell.h>

#include <platform/Zephyr/ZephyrConfig.h>

using chip::DeviceLayer::Internal::ZephyrConfig;

namespace
{

int cmd_set(const struct shell * sh, size_t argc, char ** argv)
{
    if (argc != 6)
    {
        shell_error(sh, "usage: matter_prov set <passcode> <discriminator> <iterations> <salt_b64> <verifier_b64>");
        return -EINVAL;
    }

    uint32_t passcode      = strtoul(argv[1], nullptr, 10);
    uint32_t discriminator = strtoul(argv[2], nullptr, 10);
    uint32_t iterations    = strtoul(argv[3], nullptr, 10);

    if (passcode == 0 || discriminator > 0xFFF)
    {
        shell_error(sh, "invalid passcode/discriminator");
        return -EINVAL;
    }

    CHIP_ERROR err = ZephyrConfig::WriteConfigValue(ZephyrConfig::kConfigKey_SetupPinCode, passcode);
    if (err == CHIP_NO_ERROR)
        err = ZephyrConfig::WriteConfigValue(ZephyrConfig::kConfigKey_SetupDiscriminator, discriminator);
    if (err == CHIP_NO_ERROR)
        err = ZephyrConfig::WriteConfigValue(ZephyrConfig::kConfigKey_Spake2pIterationCount, iterations);
    if (err == CHIP_NO_ERROR)
        err = ZephyrConfig::WriteConfigValueStr(ZephyrConfig::kConfigKey_Spake2pSalt, argv[4]);
    if (err == CHIP_NO_ERROR)
        err = ZephyrConfig::WriteConfigValueStr(ZephyrConfig::kConfigKey_Spake2pVerifier, argv[5]);

    if (err != CHIP_NO_ERROR)
    {
        shell_error(sh, "PROV_ERR %" CHIP_ERROR_FORMAT, err.Format());
        return -EIO;
    }

    shell_print(sh, "PROV_OK discriminator=%u (reboot to apply)", discriminator);
    return 0;
}

int cmd_show(const struct shell * sh, size_t argc, char ** argv)
{
    uint32_t passcode = 0, discriminator = 0, iterations = 0;
    bool provisioned = ZephyrConfig::ReadConfigValue(ZephyrConfig::kConfigKey_SetupPinCode, passcode) == CHIP_NO_ERROR;

    (void) ZephyrConfig::ReadConfigValue(ZephyrConfig::kConfigKey_SetupDiscriminator, discriminator);
    (void) ZephyrConfig::ReadConfigValue(ZephyrConfig::kConfigKey_Spake2pIterationCount, iterations);

    if (provisioned)
    {
        shell_print(sh, "PROV_SHOW provisioned passcode=%u discriminator=%u iterations=%u", passcode, discriminator,
                    iterations);
    }
    else
    {
        shell_print(sh, "PROV_SHOW test-defaults (nothing provisioned)");
    }
    return 0;
}

int cmd_clear(const struct shell * sh, size_t argc, char ** argv)
{
    (void) ZephyrConfig::ClearConfigValue(ZephyrConfig::kConfigKey_SetupPinCode);
    (void) ZephyrConfig::ClearConfigValue(ZephyrConfig::kConfigKey_SetupDiscriminator);
    (void) ZephyrConfig::ClearConfigValue(ZephyrConfig::kConfigKey_Spake2pIterationCount);
    (void) ZephyrConfig::ClearConfigValue(ZephyrConfig::kConfigKey_Spake2pSalt);
    (void) ZephyrConfig::ClearConfigValue(ZephyrConfig::kConfigKey_Spake2pVerifier);
    shell_print(sh, "PROV_CLEARED (test defaults active after reboot)");
    return 0;
}

} // namespace

SHELL_STATIC_SUBCMD_SET_CREATE(sub_matter_prov,
                               SHELL_CMD_ARG(set, NULL, "set <passcode> <disc> <iter> <salt_b64> <verifier_b64>", cmd_set, 6, 0),
                               SHELL_CMD(show, NULL, "show provisioning state", cmd_show),
                               SHELL_CMD(clear, NULL, "clear provisioned pairing data", cmd_clear),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(matter_prov, &sub_matter_prov, "Matter pairing-code provisioning", NULL);
