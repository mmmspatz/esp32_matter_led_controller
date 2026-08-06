# Matter-over-Thread on the ESP32-C6

Thread is the C6's default operational transport. BLE→Thread commissioning
into Home Assistant is hardware-verified end to end. This file records the
design constraints and the two non-obvious things that had to be fixed to get
there; the mechanics (build variants, patch list) are in `AGENTS.md`.

## Why this is a C6-only option

The classic ESP32-WROOM-32E has no 802.15.4 radio; the C6 does (the same
2.4 GHz front end shared with WiFi and BLE). Thread is a capability the
rework unlocks for free in hardware.

Beyond that: no WiFi credentials to provision (the join is via the Thread
operational dataset delivered during commissioning), and a different MAC/PHY
and network stack than the WiFi blob — which routes around the open WiFi-wedge
bug (see "Not yet done").

## One radio → WiFi **or** Thread is a build-time choice

The C6 has a single 2.4 GHz radio. BLE is commissioning-only, but WiFi and
802.15.4 cannot both be the operational network, and a Matter node runs a
single operational transport. So this is **not** a runtime toggle: it is the
`LEDCTRL_TRANSPORT` Kconfig choice in `app/Kconfig`, modeled on the CCT-vs-RGB
split. It defaults to Thread on `SOC_SERIES_ESP32C6` and drives both the CHIP
and the Zephyr networking Kconfig; the C6 board DTS enables `&ieee802154` and
disables `&wifi`, and `app/wifi.conf` + `app/wifi.overlay` flip both back for
a WiFi build.

The choice is declared *before* the chip-module `rsource` so its
transport-conditional re-defaults win on "first satisfied default" — including
re-inverting the WiFi-era overrides (`NET_L2_OPENTHREAD`, `NET_IPV6_NBR_CACHE`,
the WiFi stats/station symbols), which is the one place this repo's usual
"override chip-module's Thread bias" posture reverses.

The BLE→commissioning path is shared; only the NetworkCommissioning cluster
and the L2 differ. A mains-powered always-on controller is a Router-eligible
**FTD**, not a sleepy MTD.

## The two things that actually bit

**1. The esp32 802.15.4 driver swallowed TX failures.**
`esp_ieee802154_transmit_failed` discarded the HAL error code and posted the
TX-complete semaphore exactly like the success path, so `esp32_tx()` always
returned 0. The driver advertises no `HW_RETRANSMISSION`, so OpenThread owns
retransmission and never saw a failure to retry. Single frames on a clear
channel almost never hit the failure path — which is why basic traffic worked
and hid it — but back-to-back fragments under WiFi/BLE coex do, and with no
retry the loss compounds with fragment count. Multi-fragment 6LoWPAN datagrams
(notably SRP registration) never reassembled and commissioning stalled at
"checking connectivity". Carried as `patches/zephyr/0001` (mirrors the nrf5
driver: CCA/coex → `-EBUSY`, no-ack → `-ENOMSG`, else `-EIO`); upstream
candidate, sibling of Zephyr #113666.

**2. Thread NetworkCommissioning init ordering**, the same trap the WiFi path
hit. `InstanceAndDriver::Init()` registers into the `CodegenDataModelProvider`
registry that `Server::Init()` builds, so `sThreadNetworkDriver.Init()` had to
move out of `AppTaskBase::Init()` into `InitServer()` after `Server::Init()`.
Otherwise the cluster has no command handlers and the network-config step
silently no-ops. (3rd vendored scaffold change — re-diff on re-pin.)

`app/thread-diag.conf` builds an OT-shell + ping-sender image for on-air
fragmentation testing; not a production image.

## Home infrastructure required

A **Thread Border Router** on the LAN, with its credential shared into the
Matter fabric. Here that is Home Assistant's OpenThread Border Router add-on.

## RAM

Measured 2026-07-18, compile+link only, before the C6 heap sizing was
re-tuned — so read the delta, not the absolutes. A no-WiFi/yes-Thread build
(OpenThread FTD, BT still on for commissioning) linked at **86.5% of
`sram0_0_seg`** vs the WiFi build's **95.2%**: Thread is **~42 KB cheaper in
static RAM, not more expensive**. Dominant lever is turning WiFi off, which
drops its 51.2 KB kernel radio pool entirely
(`HEAP_MEM_POOL_ADD_SIZE_ESP_WIFI` is a WiFi-driver promptless default, not
sourced under `CONFIG_WIFI=n`) plus the WiFi driver/blob `.bss`; OpenThread
FTD and the 802.15.4 driver add some back.

Flash: OpenThread FTD + the 802.15.4 stack is roughly +150–250 KB `.text`,
a non-issue against the N8's 3840 KB slots.

## Not yet done

**OTA over Thread.** The requestor is transport-agnostic and works on WiFi,
but BDX over Thread is much lower-bandwidth — the ~1.3 MB image will take
minutes to tens of minutes, and the requestor timeouts have not been verified
against that.

**The WiFi wedge — open, and now unwatched.** The device drops fully off WiFi
after 1.7–6 h (looks like WiFi-blob death, not heap). Never root-caused.
Switching the C6 to Thread routed around it rather than fixing it, and since
day-to-day use is no longer on WiFi, this will not resurface on its own —
it needs a deliberate soak on a WiFi build (`wifi.conf`) to investigate.
Still presumed present, and it still affects the classic ESP32, which has no
other transport.

### Investigating it

Everything below is set up and committed; a session picking this up should
not need to rediscover any of it.

```bash
west build --sysbuild -d build_wifidiag -p always \
    -b btf_wled_esp32c6/esp32c6/hpcore app \
    -- -DEXTRA_CONF_FILE="wifi.conf;wifi-diag.conf" \
       -DEXTRA_DTC_OVERLAY_FILE=wifi.overlay \
       -DCONFIG_LEDCTRL_WIFI_CREDS_SHELL=y
west flash -d build_wifidiag
./scripts/wifi-soak.py --hours 8          # attach once, walk away
```

**Attaching to the serial port reboots the board.** The DTR/RTS auto-reset
circuit fires on open and clearing dtr/rts in software first does not stop it
— verified by two attaches 32 s apart both seeing the device clock start at
00:00:05. Any attach-sample-detach loop therefore measures nothing but the
first seconds of a fresh boot, repeatedly, while looking like a valid
negative result. `wifi-soak.py` exists to hold one handle open for the whole
run and inject shell commands through it; use it, or reproduce that property.

Three more things that are not obvious from the config:

- **The CHIP log module drowns the UART.** `platform/Zephyr/Logging.cpp:23`
  hardcodes `LOG_MODULE_REGISTER(chip, LOG_LEVEL_DBG)`, so `MATTER_LOG_LEVEL`
  cannot lower it. At 115200 the log stream fell ~90 s behind real time inside
  a two-minute capture, and `LOG_MODE_IMMEDIATE` makes the device *block* on
  those writes — distorting the timing under test. `wifi-diag.conf` enables
  runtime filtering for this; the soak script applies `log enable wrn chip` +
  `log enable dbg esp32_wifi` after each boot. Raise `chip` to `inf` when you
  want WiFiManager's own progress lines (recovery interval, disconnect reason
  — both `ChipLogProgress`).
- **The diag image logs the PSK in cleartext** (`net_wifi_mgmt: wifi_connect:
  psk` plus hexdump, at `NET_L2_WIFI_MGMT` debug). The soak script redacts it
  on the way to disk; a raw `west espressif monitor` capture does not, so
  don't paste one into an issue.
- **`patches/connectedhomeip/0007` already fixed the terminal half.** The recovery
  backoff jitter was uninitialised, which turned any sustained recovery
  failure into a permanent one. Expect the symptom to change from "gone
  forever" to "drops and recovers" — that is progress, not a fix.

What to chase first: this board re-establishes WiFi roughly every 100 s (see
the `kDnssdInitialized` comment in `scaffold/CommonDeviceCallbacks.cpp`),
which reproduces in two minutes rather than hours and is the likely feeder of
failed scans into the backoff ramp. Establish whether that cycle is a real
disassociation (the 802.11 reason code from `SetLastDisconnectReason()`
splits AP-side eviction from device-side failure) or merely mDNS rebinding —
they are different bugs. `wifi ps off` is the other cheap early experiment;
nothing pins power save today, so the build runs on the Espressif min-modem
default.

Status as of 2026-08-05: instrumentation landed and smoke-tested, patch 0007
applied and flashed. **No soak has actually been run yet** — every
observation so far came from sub-90-second windows on a board that the
observer kept rebooting, so nothing is known about the wedge itself.

Factory reset and the LED cluster path are transport-agnostic above
`LightingManager` and need no Thread-specific work.
