/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * `chip_heap` shell command: prints the CHIP sys_heap free/used/high-water,
 * for right-sizing CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE against measured peak
 * usage on hardware (commissioning and OTA are the stress cases). Present
 * whenever the shell is (the C6 production image; the classic prov.conf).
 * Malloc::GetStats is lock-guarded, so this is safe to call from the shell
 * thread while CHIP runs.
 */

#include <zephyr/shell/shell.h>

#include <lib/core/CHIPError.h>
#include <platform/Zephyr/SysHeapMalloc.h>

using chip::DeviceLayer::Malloc::GetStats;

static int cmd_chip_heap(const struct shell * sh, size_t argc, char ** argv)
{
#ifdef CONFIG_SYS_HEAP_RUNTIME_STATS
	chip::DeviceLayer::Malloc::Stats stats;
	if (GetStats(stats) != CHIP_NO_ERROR)
	{
		shell_error(sh, "chip_heap: GetStats failed");
		return -1;
	}
	shell_print(sh, "chip_heap size=%u free=%u used=%u high_water=%u",
		    (unsigned) CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE, (unsigned) stats.free, (unsigned) stats.used,
		    (unsigned) stats.maxUsed);
#else
	shell_print(sh, "chip_heap: CONFIG_SYS_HEAP_RUNTIME_STATS not enabled");
#endif
	return 0;
}

SHELL_CMD_REGISTER(chip_heap, NULL, "CHIP sys_heap free/used/high-water (bytes)", cmd_chip_heap);
