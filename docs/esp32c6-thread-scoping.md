# Matter-over-Thread on the ESP32-C6: scoping

Status: **scoping only — no Thread code exists yet.** This records what
adding Matter-over-Thread as a second commissioning/transport path on the C6
would take, so the WiFi bring-up can finish first and Thread can be picked up
deliberately. Assume no prior art (CHIP generic-Zephyr + esp32c6 802.15.4
would likely be another first) and verify the unknowns below before building.

## Why this is a C6-only option, and why bother

The classic ESP32-WROOM-32E has no 802.15.4 radio; the C6 does (the same
2.4 GHz front end shared with WiFi and BLE). So Thread is a capability the
rework unlocks for free in hardware.

Motivations, in priority order:
- **It may sidestep the WiFi wedge.** The open ACTIVE bug is the device
  dropping fully off WiFi after 1.7–6 h (looks like WiFi-blob death, not
  heap). If that proves unfixable in the blob, Matter-over-Thread is a
  different MAC/PHY and a different network stack — it would route around the
  failure entirely. This is the strongest reason to keep Thread in reach.
- No WiFi credentials to provision; join is via the Thread operational
  dataset delivered during commissioning.
- Mesh reliability / lower RF footprint (marginal for a mains-powered,
  always-on controller).

## One radio → WiFi **or** Thread is a build-time choice

The C6 has a single 2.4 GHz radio. BLE is commissioning-only (already true
today), but WiFi and 802.15.4 cannot both be the operational network, and a
Matter node runs a single operational transport. So this is **not** a runtime
toggle — model it like the existing CCT-vs-RGB split: a build-time network
choice, e.g. `-DEXTRA_CONF_FILE=thread.conf` selecting an
`LEDCTRL_NET_THREAD` Kconfig, mutually exclusive with the default WiFi build.
The BLE→commissioning path is shared; only the NetworkCommissioning cluster
and the L2 differ.

## What already exists in-tree (checked)

- Zephyr radio driver: `zephyr/drivers/ieee802154/ieee802154_esp32.c`
  (+ `.h`, `Kconfig.esp32`).
- HAL: `modules/hal/espressif/components/ieee802154` +
  `esp_hal_ieee802154/esp32c6/ieee802154_periph.c`; coex header
  `esp_coex/include/esp_coex_i154.h`. The esp32c6 HAL CMakeLists already
  gates 802.15.4 sources on `CONFIG_IEEE802154` and treats WiFi/BT/802.15.4
  as shared radio resources (line ~462).
- OpenThread is **declared** in `manifest/west.yml` but never fetched or
  compiled (the west.yml comment says as much; `CONFIG_NET_L2_OPENTHREAD=n`
  everywhere). Enabling Thread means actually pulling and building it.

So the physical-layer plumbing is present; the untrodden parts are the
Zephyr 802.15.4 driver ↔ esp32c6 HAL bring-up, OpenThread on this SoC, and
CHIP's Thread path over the generic Zephyr platform.

## Config deltas (first cut, expect to grow)

Zephyr / networking:
- `CONFIG_IEEE802154=y`, enable the `ieee802154_esp32` driver + the radio
  node in the board DTS.
- `CONFIG_NET_L2_OPENTHREAD=y` (this repo currently force-disables it in
  prj.conf — the WiFi-only landmine override; the Thread build wants the
  opposite), pull the OpenThread module.
- OpenThread stack Kconfig (FTD vs MTD — a mains-powered always-on device is
  a Router-eligible **FTD**, not a sleepy MTD).
- The WiFi stack (`CONFIG_WIFI`, `CHIP_WIFI`, `CHIP_ENABLE_WIFI_STATION`, the
  net-buf/context sizing tuned for WiFi) comes OUT of the Thread build.

CHIP / Matter:
- `CONFIG_CHIP_ENABLE_OPENTHREAD=y` (or the platform's equivalent), which
  brings in `GenericThreadStackManagerImpl` + the Thread NetworkCommissioning
  driver instead of the WiFi one.
- Re-examine the chip-module `Kconfig.defaults` overrides in prj.conf: several
  were flipped *because* we were WiFi-only (`NET_L2_OPENTHREAD=n`,
  `NET_IPV6_NBR_CACHE=y` for RS, WiFi stats). The Thread build wants
  chip-module's Thread-biased defaults back — this is the one place the
  current "override the Thread bias" posture inverts.
- OTA: the Thread build still wants the OTA Requestor; BDX over Thread is
  lower-bandwidth (the ~1.3 MB image download will be slow — minutes to tens
  of minutes). Verify the requestor timeouts tolerate it.

## Home infrastructure required

A **Thread Border Router** on the LAN. The user runs Home Assistant, so the
realistic path is HA's OpenThread Border Router add-on + a supported 802.15.4
radio (SkyConnect / HA Yellow / a Nest/Apple TV BR that HA can share a
credential with). Matter-over-Thread commissioning via HA needs the BR
credential shared into the Matter fabric. This is net-new home setup and is
the biggest non-code dependency — confirm it exists before committing.

## Cost

- **Flash:** OpenThread FTD + the 802.15.4 stack is roughly +150–250 KB
  `.text`. The C6-N8 has 8 MB with 3840 KB slots — a non-issue.
- **RAM (measured 2026-07-18, compile+link only):** a no-WiFi/yes-Thread
  build (`CHIP_WIFI=n`, `NET_L2_OPENTHREAD=y`, `IEEE802154=y`, OpenThread FTD,
  BT still on for commissioning) links at **86.5% of `sram0_0_seg` — 423,008
  / 488,976 B, ~66 KB free** — vs the WiFi build's **95.2% (~23.5 KB free)**.
  So Thread is **~42 KB cheaper in static RAM, not more expensive** (the
  scoping guess of "small or negative" was too conservative). Dominant lever:
  turning WiFi off drops its **51.2 KB kernel radio pool** entirely
  (`HEAP_MEM_POOL_ADD_SIZE_ESP_WIFI` is a WiFi-driver promptless default, not
  sourced under `CONFIG_WIFI=n`) plus the WiFi driver/blob `.bss`; OpenThread
  FTD + the 802.15.4 driver add some back. The CHIP heap (98 KB) and the BT
  commissioning pool (50 KB) are identical in both builds. Not functionally
  validated — link only. Recipe used: `thread.conf` + a 3-line `ieee802154`
  DT overlay (below).

## Effort / milestones (rough)

1. **Radio bring-up** (highest risk): `CONFIG_IEEE802154=y` on the C6, DTS
   radio node, confirm the Zephyr `ieee802154_esp32` driver + esp32c6 HAL +
   coex actually init and TX/RX on hardware. This is the "no prior art" part.
2. **OpenThread up**: pull the module, join a test Thread network (BR), ping
   over Thread — before any Matter.
3. **Matter-over-Thread**: `CHIP_ENABLE_OPENTHREAD`, Thread
   NetworkCommissioning, BLE→Thread commission from chip-tool, then HA.
4. **Parity**: OTA over Thread, factory reset, the LED cluster path (all
   transport-agnostic above LightingManager, so should be free).
5. **Build integration**: the `thread.conf` variant + a board-conf split so a
   WiFi build and a Thread build coexist cleanly (mirror CCT/RGB).

## Unknowns to verify first (assume no prior art)

- Does the Zephyr `ieee802154_esp32` driver actually work on esp32c6 under
  *this* HAL pin? **Compile/link: answered (2026-07-18) — yes**, once the
  `ieee802154` devicetree node is enabled (3-line overlay mirroring
  esp32h2_devkitm: `chosen { zephyr,ieee802154 = &ieee802154; }` +
  `&ieee802154 { status = "okay"; }`). Without the node, `CONFIG_IEEE802154=y`
  compiles the HAL sources with the IDF `CONFIG_IEEE802154_*` macros undefined
  (the Zephyr `IEEE802154_ESP32` driver, which bridges those names, only turns
  on with the DT node) and they fail to compile. **Whether it actually TX/RX
  on hardware is still unproven — that is the real milestone-1.** Also clean at
  compile/link: OpenThread FTD and CHIP's entire Thread platform layer (SRP
  DNS-SD, `GenericNetworkCommissioningThreadDriver`).
- Radio coexistence: BLE (commissioning) + 802.15.4 (operational) share the
  radio via the coex blob — does CHIPoBLE commissioning-then-Thread hand off
  cleanly, the way BLE→WiFi does?
- CHIP generic-Zephyr Thread path: the WiFi path needed the scaffold
  NetworkCommissioning-ordering fix. Re-audit the Thread NetworkCommissioning
  init the same way.
- Does the HAL's 802.15.4 need its own patch (there's a
  `tools/sync/patches/ieee802154.patch` in the HAL tree — understand what it
  is before trusting the component as-is).
