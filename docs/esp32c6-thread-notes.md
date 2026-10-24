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

**The WiFi wedge — open, but no longer unmeasured.** The device drops fully off
WiFi after 1.7–6 h (looks like WiFi-blob death, not heap). Never root-caused.
Switching the C6 to Thread routed around it rather than fixing it. One 8 h soak
on an instrumented C6 WiFi build has now run clean — see "First soak" below for
what that does and does not establish. Still open: one negative under easy
conditions is not a fix, the C6 is not the classic ESP32, and the classic board
has no other transport to route around it with.

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

### First soak: 8 h clean (2026-08-05 23:08 → 08-06 07:08)

`soak-20260805-230805.log`, C6 diag build, patch 0007 applied. **No wedge**,
against a bug that was reported at 1.7–6 h — so it cleared the far end of that
window by two hours. Quality of the negative, since that was the whole problem
with the earlier attempts: the device clock tracked the host clock to within
0.3 s for the entire run (no log backlog, no stall), all 480 `wifi status`
polls answered `State: COMPLETED`, one BSSID and one channel throughout, RSSI
−36 ±2. TX volume was flat at ~300 pkt/h with an identical size mix in every
one of the eight hours.

Zero disconnects, and that is a driver-level fact rather than an inference:
`esp_wifi_event_handler` logs `Disconnect reason: %d` at `LOG_DBG` and
`esp32_wifi` was at debug the whole time. The log holds exactly three WiFi
events, all inside the first 8 s (STA_START, home-channel-change,
STA_CONNECTED), and nothing after. Every `<err>`/`<wrn>` line in 8 h — 37 of
them — falls before device time 00:15:03; the last 7h44m are clean.

**This soaked the least-likely-to-fail configuration on the least-likely-to-fail
board**, so read the result narrowly. In rough order of how much each one
undermines it:

- **The wedge was probably never a C6 phenomenon.** The C6 became the primary
  board on 2026-07-18 and switched to Thread on 07-19 — one day as a WiFi
  device. The 1.7–6 h figure is a *range*, i.e. several observations, and they
  accumulated in early July when the only WiFi device was the classic ESP32
  (OTA over WiFi 07-04, HA OTA 07-05). The earliest written mention (773681f)
  calls it "the open ACTIVE bug" and proposes Thread to sidestep it, written
  the same day the C6 arrived. A clean C6 run may simply be a board that was
  never sick.
- **The C6 runs roughly double the WiFi memory the classic board gets.** The
  classic conf trims what the C6 conf deliberately leaves at upstream defaults:
  static RX buffers 6 vs 10, dynamic RX 16 vs 32, dynamic TX 16 vs 32, AMPDU
  off vs on, and the kernel radio pool 24576 vs 51200 (plus BT 16384 vs 50000).
  The classic pool is sized ~43.8K against a measured ~42K peak — about 4%
  headroom against a blob whose multi-hour allocation behaviour is
  uncharacterised. Corroborating this from inside the clean run: the log
  contains one `<err> esp32_wifi: Failed to allocate net buffer` at 00:00:08,
  during the boot mDNS burst, *on the generous config*. If the C6 can
  transiently exhaust with double everything, the classic board does it far
  more often — and a blob that mishandles allocation failure would accumulate
  into exactly "drops fully off after hours", while looking like "not heap"
  because the CHIP heap is a different heap.
- **Patch 0007 was never exercised.** It fixes the jitter at the top of the
  recovery backoff, and recovery never ran — there was nothing to recover
  from. The clean run cannot be credited to it. It is still a real
  uninitialised-read fix; it is not yet a demonstrated fix for *this*.
- **Conditions were the friendliest available**: an idle overnight network,
  RSSI −36, no roaming. If AP-side eviction or channel contention is part of
  the mechanism, this run would not have found it.
- **The instrumentation is not free.** `LOG_MODE_IMMEDIATE` plus per-packet
  debug logging keeps the CPU and UART busy continuously, and the soak script
  pokes the driver via `wifi status` every 60 s. Either could mask a timing- or
  idleness-dependent failure.

So what this run does establish is narrow but real: **C6 WiFi is not broken out
of the box**, and the harness produces trustworthy negatives.

**Next experiment, and it isolates the top two hypotheses in one run:** soak the
C6 with the *classic* board's WiFi trims applied (the six symbols above). Same
silicon, same environment, only the buffer diet changes. Wedges → it is
configuration, and the classic board's problem is that it cannot afford the
memory the blob wants. Survives → the difference is SoC/blob and the classic
board needs a different answer. Either way it beats another run of the same
build.

Dead end worth recording so it is not rediscovered: the post-commissioning
BT-DRAM reclaim frees ~54.7K, almost exactly the classic board's pool deficit —
but into CHIP's `sys_multi_heap` for `malloc`, whereas the WiFi driver buffers
come from the *kernel* pool via `k_malloc`. Different heap; it does not relieve
this without redirecting the reclaimed region.

**The "~100 s WiFi re-establish" premise did not reproduce.** The WiFi link was
established once and never re-established. That figure appears in the
`kDnssdInitialized` comment in `scaffold/CommonDeviceCallbacks.cpp` as the
motivation for the one-shot `InitOTA` guard. The guard is still correct — one
recurrence mid-download is enough to zero `mTargetVersion` — but the cadence it
cites is not what this board does now, and the earlier plan to chase that cycle
as the fast reproducer is dead. (Caveat: `chip` was filtered to `wrn`, so a
`kDnssdInitialized` that fired *without* a WiFi re-association would not have
been logged. What is certain is that the radio never dropped.)

Two clean periodics remain unidentified, both TX-only (the driver logs sends,
not receives, so no RX line means nothing): a 103 B + 96 B pair 24 ms apart
recurring every **69.0 s**, 413 times out of 415 intervals; and 86 B frames in
bursts of two or three about 1 s apart. Sizes and retry shape fit a Matter
subscription report to HA and IPv6 neighbour solicitation respectively, but
that is inference from length alone — confirming it needs a capture on the AP
side, not more device logging.

Not covered: nothing exercised the cluster path during the soak, so "the link
stayed up" is established and "HA could still control it" is not.

Factory reset and the LED cluster path are transport-agnostic above
`LightingManager` and need no Thread-specific work.
