# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# The Zephyr module list is fixed at find_package time, so registering
# CHIP's chip-module (its Kconfig tree + the GN ExternalProject that builds
# libCHIP.a) is the one thing that must happen first.
get_filename_component(CHIP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../modules/connectedhomeip REALPATH)
list(APPEND ZEPHYR_EXTRA_MODULES ${CHIP_ROOT}/config/zephyr/chip-module)

find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})


# unused-result: the reused example scaffold ignores a nodiscard
# CHIP_ERROR on the WiFi path; not our code to fix here.
target_compile_options(app PRIVATE -Werror -Wno-error=maybe-uninitialized -Wno-error=unused-result)
target_compile_options(app PRIVATE -Wdouble-promotion -Werror=double-promotion)

project(esp32_matter_led_controller)

include(${CHIP_ROOT}/config/zephyr/app/enable-gnu-std.cmake)
include(${CHIP_ROOT}/src/app/chip_data_model.cmake)

target_include_directories(app
    PRIVATE
    include
    src
    src/scaffold
)

# src/scaffold is CHIP's examples/platform/silabs/zephyr app scaffold
# (generic despite the upstream path), vendored so we own its bugfixes —
# notably NetworkCommissioning registration ordering in AppTaskBase.cpp.
target_sources(app
    PRIVATE
    src/AppTask.cpp
    src/DeviceCallbacks.cpp
    src/LightingManager.cpp
    src/color_math.c
    src/led_pwm.c
    src/rand_shim.c
    src/scaffold/main.cpp
    src/scaffold/AppTaskBase.cpp
    src/scaffold/AppTaskZephyr.cpp
    src/scaffold/CHIPDeviceManager.cpp
    src/scaffold/CommonDeviceCallbacks.cpp
    # Without this file cluster attribute writes silently no-op (it defines
    # MatterPostAttributeChangeCallback and routes it to DeviceCallbacks).
    src/scaffold/DataModelCallbacks.cpp
)

if(CONFIG_LEDCTRL_PROV_SHELL)
    target_sources(app PRIVATE src/ProvisionShell.cpp)
endif()

if(CONFIG_LEDCTRL_MODE_RGB)
    set(LEDCTRL_ZAP_FILE ${CMAKE_CURRENT_SOURCE_DIR}/data_model/rgb_light.zap)
else()
    set(LEDCTRL_ZAP_FILE ${CMAKE_CURRENT_SOURCE_DIR}/data_model/lighting.zap)
endif()

chip_configure_data_model(app
    ZAP_FILE ${LEDCTRL_ZAP_FILE}
    ZCL_PATH ${CHIP_ROOT}/src/app/zap-templates/zcl/zcl.json
)
