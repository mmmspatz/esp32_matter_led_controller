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
(or likely any of its many clones): three MOSFET-buffered PWM channels,
24V input, and a socketed-by-hot-air ESP32 module.

The primary target is that board with the stock ESP32-WROOM-32E replaced
by an **ESP32-C6-WROOM-1-N8**. The modules are pad-for-pad compatible on
28 castellations, so it's a desolder-and-reflow plus one trace cut; the
C6 buys 512K of SRAM (the classic board lives at 99.7% of its 136K DRAM
bank), an 802.15.4 radio, and hardware P-256. Pad mapping, the GPIO8
strap rework, and the bring-up log are in
[docs/esp32c6-rework-notes.md](docs/esp32c6-rework-notes.md).

The unmodified **ESP32-WROOM-32E** board remains a fully supported second
target, WiFi-only.

## Build variants

Strip type and operational transport are both build-time choices:

| Variant | Strip | Matter device type |
|---|---|---|
| CCT (default) | 2-channel tunable white (CH1=cool, CH2=warm) | Color Temperature Light (0x010C) |
| RGB (`rgb.conf`) | 3-channel RGB | Extended Color Light (0x010D) |

| Board | Default transport | Alternative |
|---|---|---|
| ESP32-C6 | Matter over **Thread** | WiFi (`wifi.conf` + `wifi.overlay`) |
| ESP32-WROOM-32E | Matter over **WiFi** | — (no 802.15.4 radio) |

The C6 has one 2.4 GHz radio, so WiFi and Thread are mutually exclusive
at build time. BLE is commissioning-only either way.

## Quickstart

```bash
git clone https://github.com/mmmspatz/esp32_matter_led_controller
cd esp32_matter_led_controller
./bootstrap.sh                # coffee-length: Zephyr + Matter SDK + toolchain
source activate.sh
west build --sysbuild -b btf_wled_esp32c6/esp32c6/hpcore app
west flash
```

Other combinations:

```bash
# C6, RGB strip
west build --sysbuild -b btf_wled_esp32c6/esp32c6/hpcore app -- -DEXTRA_CONF_FILE=rgb.conf

# C6 over WiFi instead of Thread
west build --sysbuild -b btf_wled_esp32c6/esp32c6/hpcore app \
    -- -DEXTRA_CONF_FILE=wifi.conf -DEXTRA_DTC_OVERLAY_FILE=wifi.overlay

# Classic ESP32-WROOM-32E
west build --sysbuild -b btf_wled_esp32/esp32/procpu app
```

Then add it from your Matter controller with the development pairing code
`34970112332`. A Thread build needs a border router on the network
(Home Assistant's works).

To give a device its own pairing code + QR instead of the shared test
code, see the provisioning runbook in [AGENTS.md](AGENTS.md#provisioning-per-device-pairing-codes).

### Home Assistant needs its test-certificate toggle

This firmware uses the Matter SDK's example attestation certificates and
test VID 0xFFF1. HA's Matter Server rejects those under its default
production trust policy — commissioning gets as far as device attestation
and fails. Turn on Settings → Apps → Matter Server → Configuration →
**"Enable test-net DCL usage"** first, and leave it on (adoption needs it
too, not just the attestation step). Google Home rejects test certs
outright; Alexa is unverified.

## Buttons

Hold S1 for 5 seconds to factory reset (unpair). Then power-cycle —
the reboot-after-reset currently trips an upstream hal bug and halts
after wiping state.

## Firmware updates

OTA works end to end via both Home Assistant and chip-tool: MCUboot (dual
slots, ECDSA-P256 signed) plus the CHIP OTA Requestor. Building and serving
a signed `.ota` is documented in [AGENTS.md](AGENTS.md#ota-firmware-update).

## Status

Working today on the C6: BLE commissioning onto Thread or WiFi,
on/off/dim/color-temperature with smooth transitions, state persistence,
factory reset, per-device provisioning, and OTA. The classic ESP32 does
the same over WiFi.

One open bug: on WiFi the device drops fully off the network after a few
hours (suspected WiFi-blob death). Thread builds route around it, which is
part of why the C6 defaults to Thread; the classic board has no such escape.

Getting here took patching four upstream trees — CHIP, the Espressif HAL,
TF-PSA-Crypto, and Zephyr's esp32 802.15.4 driver. Those patches live in
`*-patches/` and are re-applied by `bootstrap.sh` after every `west
update`; [AGENTS.md](AGENTS.md#workspace) says what each one is for.
