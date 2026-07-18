# ESP32-C6-WROOM-1-N8 rework: migration notes

Plan: desolder the ESP32-WROOM-32E from the BTF board and solder an
ESP32-C6-WROOM-1-N8 onto the same footprint. This file records the
hardware mapping, the firmware deltas, and which of the classic-ESP32
battle scars stop applying. Firmware target: `btf_wled_esp32c6/esp32c6/hpcore`
(board skeleton is in-tree; **not yet validated on hardware**).

Sources: ESP32-WROOM-32E/32UE datasheet v2.0, ESP32-C6-WROOM-1/-1U
datasheet v1.4.

## Footprint / pad mapping

The modules are both 18×25.5 mm, and the **28 side castellations align
pad-for-pad** (identical land pattern: 1.27 mm pitch, same pad-1 datum).
The C6 has no bottom-edge pads, so the 32E's 10 bottom pads (GND, IO13,
6×NC, IO15, IO2 — all unused by this board except the redundant GND) are
left open. The center EPAD/GND grids differ slightly but soldering them is
optional on both. Use the **-1** (PCB antenna) variant, not the shorter
-1U. N8 = 8 MB quad-SPI flash, no PSRAM.

Pads this board actually uses:

| PCB pad (32E #) | Old signal | New C6 signal | Notes |
|---|---|---|---|
| 1 / 38 | GND | GND | match |
| 2 | 3V3 | 3V3 | match (same ≥0.5 A budget, same decoupling) |
| 3 | EN | EN | match; stock RC covers the C6's longer 3 ms strap hold |
| 10 | IO25 = silk CH3 | **GPIO8** | **boot strap.** As-built: trace to the '245 cut; carries only the strap pull-up (drives nothing); silk CH3 moved to pad 19. See below. |
| 11 | IO26 = silk CH2 (warm) | GPIO10 | plain GPIO, clean |
| 12 | IO27 = silk CH1 (cool) | GPIO11 | plain GPIO, clean |
| 19 | NC (unused) | **GPIO21** | silk CH3 rewired here (jumper to the '245 CH3 input) after the GPIO8 trace cut; carries LEDC ch2 |
| 25 | IO0 (button S1 + boot strap) | **GPIO9** | the C6's boot pin: identical hold-low-for-download semantics, default weak pull-up — button and DTR/RTS autoboot work unchanged |
| 34 / 35 | RXD0 / TXD0 | RXD0 / TXD0 | UART0 aligns (software GPIO numbers become 17/16) |
| 4 | IO36 (mic, unused) | IO4 | input-only pin becomes full I/O + ADC |
| 27 | IO16 (DAT, unused) | IO19 | plain GPIO |

## The one hardware watch-item: GPIO8

Serial download mode on the C6 requires **GPIO8 = 1 while GPIO9 is held
low through reset** (Table 4-3, ESP32-C6-WROOM-1 datasheet v1.4). GPIO8
(the old IO25/CH3 pad) has no internal pull and drives a 74HC245 buffer
input on this board, so during reset it floats — download-mode entry
(i.e. every `west flash`) would be unreliable.

**Recommendation: add a 10 kΩ pull-up on the old-IO25/CH3 net during
rework.** Trade-off: the pull-up biases the buffer high while the chip is
in reset, so the CH3 MOSFET turns on briefly at power-on/reset — invisible
on CCT boards (CH3 unconnected), a blue blink on RGB builds. A pull-down
instead would permanently lock out UART download mode; do not. (The C6's
USB-Serial-JTAG pads land on the old IO14/IO12 nets, so USB flashing is
not an escape hatch on this PCB — those pins aren't routed to the USB-C
connector without extra rework. Caveat on the strap: it governs only the
*strap-based* download this board uses over the UART bridge. esptool over
native USB instead triggers download via the USB-Serial-JTAG peripheral's
force-download reset, which bypasses the GPIO8/GPIO9 strap entirely — that
is how devkits repurposing GPIO8 as a plain GPIO (e.g. the C3-DevKitM's LED)
still flash. So the pull-up is required for the bridge path we are keeping,
and would become unnecessary only if the board were reworked to native-USB
flashing.)

**As-built (HW bring-up 2026-07-18):** the pull-up alone was *not* enough.
GPIO8 measured ~1.7V — dragged toward the '245 input net (no discrete
pull-down present), landing right on the logic threshold — so `west flash`
could not enter download mode (`esptool: Wrong boot mode detected (0x0)`).
Cutting the pad-10 trace to the '245 removed the load; GPIO8 then sat at a
clean 3.3V and download-mode entry became reliable with no BOOT-button hold.
The cut severs silk CH3, so CH3 was restored by jumpering footprint pad 19
(IO21, previously NC) to the '245 CH3 input; the board def now drives LEDC
ch2 on GPIO21. (This also moots the "brief CH3 blink at reset" trade-off
above — GPIO8 no longer drives that buffer.) Net rework: 10k pull-up
GPIO8→3V3, cut pad-10→'245, jumper pad-19→'245 CH3 input.

### Why it's two pins (this reads as arbitrary otherwise)

The classic ESP32 selected download with a *single* pin, GPIO0 (internal
pull-up → SPI boot; hold low → download), and GPIO0 was already the button.
The C6 spreads boot mode across GPIO8 **and** GPIO9, but they are not
coequal bits:

- **GPIO9 is the real selector** (the GPIO0 analog): weak pull-up default,
  =1 → SPI boot, =0 → download. It landed on the same button pad (pad 25),
  so autoboot / DTR-RTS work unchanged.
- **GPIO8 is a qualifier, not a second selector.** Its primary documented
  job is ROM-log print control (§4.3 / Table 4-5); the boot logic reuses
  it. That asymmetry is the whole story — from Table 4-3:

  | GPIO8 | GPIO9 | Result |
  |---|---|---|
  | any | 1 | SPI boot |
  | 1 | 0 | joint download boot |
  | 0 | 0 | undocumented — not a defined mode |

Two consequences fall out, and they are exactly our situation:
1. **Normal SPI boot never depends on GPIO8** — only on GPIO9, which is
   internally pulled up. A board leaving the pull-less GPIO8 floating still
   boots from flash reliably; the "don't care" is deliberate, dumping all
   the cost of the pull-less pin onto the download path.
2. **A floating GPIO8 breaks *flashing* specifically**: when the bridge's
   auto-reset drives GPIO9 low to request (strap-based) download, an
   indeterminate GPIO8 can land in the undocumented 0/0 combo instead of
   1/0 — precisely the "unreliable `west flash`" symptom the pull-up cures.
   Strapping GPIO8 *low* is **not** a way to disable download: 0/0 is
   undocumented (not "download off"), and real download-lockout is an eFuse
   (`EFUSE_DIS_DOWNLOAD_MODE` / `EFUSE_DIS_USB_SERIAL_JTAG`), never a strap.
   Straps are power-on convenience, not a security boundary.

The app may drive GPIO8 freely after the 3 ms strap-hold window (t_H,
Table 4-2). The other C6 strap pins land on old input-only/unused pads with
no conditioning on this board — MTMS/MTDI (= GPIO4/5, SDIO clock-edge
select) and GPIO15 (JTAG source) — harmless here.

## Bring-up results (2026-07-18, first power-on)

First flash + boot of the reworked board. Verified working:
- **`west flash` over the CH340 bridge, unassisted** (no BOOT-button hold),
  after the GPIO8 trace-cut above — esptool auto-reset enters download;
  GPIO9's DTR path is unchanged from the classic board. (ModemManager, still
  running, is not a factor: esptool reached the chip through it.)
- **MCUboot on RISC-V**: validates slot 0 (ECDSA-P256 / PSA verify,
  mcuboot-patches/0001) and boots the app — answers the MCUboot open
  question below.
- **Zephyr 4.4.1 boots**, WiFi/coex ROM inits, and the **BLE controller
  enables** (`esp_bt_controller_init`/`_enable` OK; BT MAC + libbtbb PHY
  print).

**Blocking issue — BLE host init panics.** `bt_enable()` → Zephyr host
`common_init()` sends HCI_Reset (opcode 0x1003), then blocks on the
command-complete semaphore **with IRQs already locked** → `kswap.h:98`
"Context switching while holding lock!", FATAL ERROR 4, system halts (no
watchdog). Root-caused to the C6 NimBLE controller's critical-section layer
(`modules/hal/espressif/components/bt/porting/npl/zephyr/src/npl_os_zephyr.c`,
`npl_zephyr_hw_enter`/`_exit_critical`) leaving IRQs locked across the VHCI
send path — the classic ESP32 uses the btdm `osi_funcs` mechanism instead,
which is why this is C6-only. No prior art found. Under investigation.

## What the C6 makes obsolete (the fun list)

| Classic-ESP32 arrangement | On the C6 |
|---|---|
| Split DRAM banks, dram0 at 99.7%, `__noinit` heap relocation (chip-patches/0002), `ESP32_REGION_1_NOINIT` pin | Gone: single unified 512K HP-SRAM bank. Patch 0002 stays for the old board but is inert on C6 (`__noinit` just lands in the same bank). |
| 56K BT-controller DRAM reserve + post-commissioning reclaim (chip-patches/0005/0006, `LEDCTRL_RECLAIM_BT_DRAM_AFTER_COMMISSIONING`) | Gone: no linker carve-out exists on C6 (BT memory is kernel-heap). The Kconfig gates on `SOC_SERIES_ESP32`, so the reclaim (and its one-way-BLE restriction!) compiles out — BLE stays available for additional-fabric commissioning. |
| Kernel-heap floor lowering (`HEAP_MEM_POOL_ADD_SIZE_ESP_WIFI/BT` re-defaults 24576/16384) | Not needed: C6 keeps the soak-tested driver defaults (51200 + 50000). Now conditional on `SOC_SERIES_ESP32` in app/Kconfig. |
| WiFi driver buffer trims, AMPDU off, `MBEDTLS_PSA_KEY_SLOT_COUNT=32`, 40K CHIP heap | Stock defaults; CHIP heap starts at 96K (`app/boards/btf_wled_esp32c6_esp32c6_hpcore.conf`). |
| `rand_shim.c` (WiFi blob's strong `random()` vs picolibc) | Not needed: the C6 hal adapter/blobs export no `random`/`rand`/`srand` (verified with nm + source grep). Now compiled only for `SOC_SERIES_ESP32`. |
| 4 MB flash, 1856K slots (Matter barely fits) | 8 MB, 3840K slots (`dts/btf/partitions_btf_8M.dtsi`, sys partition dropped, boot at 0x0). |
| Slow software P-256 (`MBEDTLS_ECP_NIST_OPTIM` load-bearing) | Still software: Zephyr wires up only SHA/AES accelerators for C6, no ECC/MPI — keep `NIST_OPTIM` (it's in common prj.conf). The 160 MHz RISC-V core will need re-benchmarking for CASE latency. |

New landmine found while porting: chip-module's `Kconfig.defaults`
re-declares `config FPU` with a bare `default y` and no `depends on`,
which ORs past the arch `CPU_HAS_FPU` gate — so FPU silently turns on
for the FPU-less rv32imac core and the RISC-V `reset.S` fails to
assemble (`fscsr`). Forced off in the C6 board conf; the classic ESP32
keeps its real FPU on via its board conf.

Open questions to verify at bring-up (assume no prior art — this would be
CHIP generic-Zephyr on C6, likely another first). Several now answered — see
Bring-up results above:
- Does the `esp_restart`-under-`irq_lock` factory-reset panic reproduce on
  C6's hal? (Different reset path; re-test before documenting.)
- BLE is a different controller generation (`libble_app`, BLE-5 feature
  set) driven through the same `esp32_bt_hci` VHCI driver — CHIPoBLE
  should be transparent, but commissioning needs a full smoke test.
- MCUboot ECDSA-P256/PSA verify (mcuboot-patches/0001 + heap overlay) is
  arch-independent in principle; confirm the C6 MCUboot image links the
  same backend and boots.
- Kernel heap: `HEAP_MEM_POOL_ADD_SIZE_*` defaults total ~105K on C6.
  Measure actual radio peaks before dieting (don't import the ESP32
  numbers blind).
- Re-run provisioning per reworked board (fresh flash = empty storage;
  the chip-fct codes from the old module do not carry over unless the
  flash content is cloned).

## How dual-board support is structured

- `app/prj.conf` — portable Matter/app policy only.
- `app/boards/btf_wled_esp32_esp32_procpu.conf` — classic-ESP32 RAM diet.
- `app/boards/btf_wled_esp32c6_esp32c6_hpcore.conf` — C6 sizing.
- `app/Kconfig` — heap-floor re-defaults now `if SOC_SERIES_ESP32`.
- `boards/btf/btf_wled_esp32c6/` — new board (selects the WROOM-1U-N8 SoC
  symbol; Zephyr defines no non-U variant and they differ only by antenna).
- LEDC channel numbering keeps the silk mapping (ch0=CH1 cool, ch1=CH2
  warm, ch2=CH3), so `LEDCTRL_CH_*` defaults and both ZAP variants apply
  unchanged.

Build: `west build --sysbuild -b btf_wled_esp32c6/esp32c6/hpcore app`
(variants via `-DEXTRA_CONF_FILE=rgb.conf` / `prov.conf` as before).
`bootstrap.sh` now installs the `riscv64-zephyr-elf` toolchain alongside
xtensa.
