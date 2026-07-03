# esp32_matter_led_controller

Matter firmware for the cheap "ESP32 WLED" LED-strip controllers, built
on **Zephyr RTOS** and the connectedhomeip SDK's generic Zephyr platform.

Your AI slop under-cabinet lighting deserves an artisanal, hand-debugged
firmware stack that no vendor supports. This is, as far as we can tell,
the first time Matter's generic Zephyr platform has run on an ESP32 — the
officially supported path is ESP-IDF, and every step off it is documented
in [AGENTS.md](AGENTS.md) and the git history.

It is deliberately **not WLED**: no effects, no app, no cloud. It's a
light. It turns on, off, dims, and hits the shade of white you asked
for, from any Matter controller (commissioned like any store-bought
device: BLE + QR code, standard everything).

## Hardware

A [BTF-Lighting ESP32 WLED controller](https://www.btf-lighting.com/products/esp-32-wled-wifi-music-led-controller)
(or likely any of its many clones): ESP32-WROOM-32E, three MOSFET-buffered
PWM channels, 24V input. Two firmware variants:

| Variant | Strip | Matter device type |
|---|---|---|
| CCT (default) | 2-channel tunable white (CH1=cool, CH2=warm) | Color Temperature Light (0x010C) |
| RGB (`rgb.conf`) | 3-channel RGB | Extended Color Light (0x010D) |

## Quickstart

```bash
git clone https://github.com/mmmspatz/esp32_matter_led_controller
cd esp32_matter_led_controller
./bootstrap.sh                # coffee-length: Zephyr + Matter SDK + toolchain
source activate.sh
west build -b btf_wled_esp32/esp32/procpu app
west flash
```

Then add it from your Matter controller with the development pairing code
`34970112332`. Works out of the box with Home Assistant (test
certificates accepted); Google Home requires developer-console setup.

To give a device its own pairing code + QR instead of the shared test
code, see the provisioning runbook in [AGENTS.md](AGENTS.md#provisioning-per-device-pairing-codes).

## Buttons

Hold S1 for 5 seconds to factory reset (unpair). Then power-cycle —
the reboot-after-reset currently trips an upstream hal bug and halts
after wiping state.

## Status / roadmap

Working today: BLE commissioning, WiFi, on/off/dim/color-temperature
with smooth transitions, state persistence, factory reset, per-device
provisioning. On the horizon: Zephyr 4.4 + PSA crypto
([notes](docs/zephyr-4.4-upgrade-notes.md)), OTA (dual 1856K slots are
already carved out), and maybe someday an ESP32-S6 for Matter-over-Thread.
