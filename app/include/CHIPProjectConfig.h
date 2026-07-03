/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Project-specific overrides of CHIP's default configuration.
 */

#pragma once

// Bench-fallback pairing code, used only when nothing has been provisioned
// into the chip-fct settings namespace (see scripts/provision.py).
#ifndef CHIP_DEVICE_CONFIG_USE_TEST_SETUP_PIN_CODE
#define CHIP_DEVICE_CONFIG_USE_TEST_SETUP_PIN_CODE 20202021
#endif

#ifndef CHIP_DEVICE_CONFIG_USE_TEST_SETUP_DISCRIMINATOR
#define CHIP_DEVICE_CONFIG_USE_TEST_SETUP_DISCRIMINATOR 0xF00
#endif

#ifdef __ZEPHYR__
#define CHIP_DEVICE_CONFIG_CHIP_TASK_NAME "Matter"
#define CHIP_DEVICE_CONFIG_CHIP_TASK_PRIORITY (K_PRIO_PREEMPT(10))
#endif

#define CHIP_CONFIG_SECURITY_TEST_MODE 0

// Test vendor ID; required while the device presents the example DAC.
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID 0xFFF1
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_ID 0x8005

#define CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE 1

#define CHIP_DEVICE_CONFIG_TEST_SERIAL_NUMBER "TEST_SN"

#define CHIP_DEVICE_CONFIG_EVENT_LOGGING_UTC_TIMESTAMPS 1
#define CHIP_DEVICE_CONFIG_EVENT_LOGGING_DEBUG_BUFFER_SIZE (512)
#define CHIP_DEVICE_CONFIG_EVENT_LOGGING_CRIT_BUFFER_SIZE (512)

#define CHIP_CONFIG_MRP_LOCAL_ACTIVE_RETRY_INTERVAL (2000_ms32)

// Zephyr 4.3 provides recvmsg natively; CHIP's static shim would collide
// with the extern declaration.
#define CHIP_SYSTEM_CONFIG_USE_ZEPHYR_SOCKET_EXTENSIONS 0

// ---- RAM budget (ESP32 dram0_0_seg is 136K after the BT reserve) ----

// Allocate System::PacketBuffers from the heap instead of a static 24K
// pool; actual concurrent usage is far below the pool's worst case.
#define CHIP_SYSTEM_CONFIG_PACKETBUFFER_POOL_SIZE 0

// Home Assistant + Alexa + a commissioning tool + one spare. The IM and
// session pools all derive their (spec-8.5.1-compliant) sizes from this.
#define CHIP_CONFIG_MAX_FABRICS 4

#define CHIP_DEVICE_CONFIG_MAX_EVENT_QUEUE_SIZE 16
