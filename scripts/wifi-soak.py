#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Long-running soak logger for the open "drops fully off WiFi" bug.

Needs a device flashed with the wifi-diag.conf overlay:

    west build --sysbuild -d build_wifidiag -p always \\
        -b btf_wled_esp32c6/esp32c6/hpcore app \\
        -- -DEXTRA_CONF_FILE="wifi.conf;wifi-diag.conf" \\
           -DEXTRA_DTC_OVERLAY_FILE=wifi.overlay \\
           -DCONFIG_LEDCTRL_WIFI_CREDS_SHELL=y
    west flash -d build_wifidiag
    ./scripts/wifi-soak.py --hours 8

WHY THIS SCRIPT EXISTS -- read before replacing it with `west espressif
monitor` and a pipe.

Opening the serial port RESETS the board. The USB-UART bridge's DTR/RTS
auto-reset circuit fires on open, and clearing dtr/rts in software beforehand
does not prevent it (verified: two attaches 32 s apart both saw the device
clock start at 00:00:05). So anything that attaches, samples and detaches
measures nothing but the first few seconds of a fresh boot, over and over --
which is exactly how an afternoon gets spent concluding "no disconnects yet"
about a board that has never been up longer than 90 s.

The rule that follows: attach ONCE, hold the handle for the whole soak, and
inject shell commands through that same handle. That is all this script is --
one open, wall-clock timestamps, periodic `wifi status`, everything to a file.

Interrupt with Ctrl-C for a summary. The log file is appended to, so an
interrupted run can be resumed against the same file (the board restarts, so
device timestamps restart with it; the host timestamps stay monotonic).
"""

import argparse
import datetime
import pathlib
import re
import signal
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing -- run this from the venv: "
             "source activate.sh, or .venv-zephyr/bin/python scripts/wifi-soak.py")

ANSI = re.compile(r"\x1b\[[0-9;]*[a-zA-Z]")
DEV_TS = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.(\d{3}),\d+\]")

# Events worth surfacing live rather than burying in the log.
EVENTS = [
    ("DISCONNECT", re.compile(r"disconn|deauth|disassoc", re.I)),
    ("CONNECT", re.compile(r"Connected to WiFi|CONNECT_RESULT|connection done", re.I)),
    ("RECOVERY", re.compile(r"connection recover|re-scanning|next attempt in", re.I)),
    ("SCAN", re.compile(r"No requested SSID found|scan done", re.I)),
    ("REBOOT", re.compile(r"Booting Zephyr|MCUboot|rst:0x", re.I)),
    ("FAULT", re.compile(r"panic|fatal|assert|halt", re.I)),
]

# The diag image logs the PSK in cleartext at NET_L2_WIFI_MGMT debug level
# (net_wifi_mgmt: wifi_connect: psk <hexdump>). Redact it on the way to disk so
# the log can be shared or pasted into an issue.
PSK_LINE = re.compile(r"(wifi_connect: psk)", re.I)
# ...and the hexdump continuation lines that follow it, which are where the
# cleartext actually lands. They carry no device timestamp, so suppress
# untimestamped lines until the next timestamped one.
HEXDUMP = re.compile(r"^[0-9a-f]{2}(\s+[0-9a-f]{2})*\s*\|")

# Seconds after attach during which a boot banner is the attach-reset, not a
# spontaneous reboot.
STARTUP_GRACE = 20.0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--hours", type=float, default=8.0, help="soak duration (default 8)")
    ap.add_argument("--poll", type=float, default=60.0,
                    help="seconds between `wifi status` polls (default 60)")
    ap.add_argument("--log", default=None, help="log file (default soak-<timestamp>.log)")
    ap.add_argument("--no-quiet-chip", action="store_true",
                    help="leave the chip log module at debug (floods the UART; see "
                         "wifi-diag.conf)")
    args = ap.parse_args()

    path = pathlib.Path(args.log or
                        f"soak-{datetime.datetime.now():%Y%m%d-%H%M%S}.log")

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()

    print(f"attached to {args.port} -- NOTE: this reset the board, t=0 is now")
    print(f"logging to {path}   (Ctrl-C for summary)")

    stop = False

    def onsig(_s, _f):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, onsig)

    t0 = time.time()
    log = path.open("a", buffering=1)
    log.write(f"\n=== soak start {datetime.datetime.now().isoformat()} "
              f"port={args.port} duration={args.hours}h ===\n")

    time.sleep(8)  # let it boot before typing at it
    if not args.no_quiet_chip:
        # CHIP hardcodes LOG_MODULE_REGISTER(chip, LOG_LEVEL_DBG), so this is the
        # only way to stop it outrunning a 115200 UART. Runtime state: it is lost
        # on every reboot, so re-apply whenever a REBOOT event shows up.
        for cmd in (b"log enable wrn chip\r\n", b"log enable dbg esp32_wifi\r\n"):
            ser.write(cmd)
            time.sleep(1.0)

    counts = {name: 0 for name, _ in EVENTS}
    last_poll = 0.0
    pending = b""
    redacting = False
    deadline = t0 + args.hours * 3600
    last_dev_ts = None

    while not stop and time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            pending += chunk
            *lines, pending = pending.split(b"\n")
            for raw in lines:
                line = ANSI.sub("", raw.decode("utf-8", "replace")).replace("\x08", "")
                line = line.replace("uart:~$ ", "").strip()
                if not line:
                    continue
                if redacting:
                    if DEV_TS.search(line):
                        redacting = False
                    elif HEXDUMP.match(line):
                        continue
                if PSK_LINE.search(line):
                    line = PSK_LINE.sub(r"\1 <redacted>", line)
                    redacting = True
                el = time.time() - t0
                stamp = f"{el:9.1f}s {datetime.datetime.now():%H:%M:%S}"
                log.write(f"{stamp} | {line}\n")
                m = DEV_TS.search(line)
                if m:
                    last_dev_ts = ":".join(m.groups()[:3])
                for name, pat in EVENTS:
                    if pat.search(line):
                        # The attach itself resets the board, so the opening boot
                        # banner is expected, not a finding.
                        if name == "REBOOT" and el < STARTUP_GRACE:
                            break
                        counts[name] += 1
                        print(f"{stamp} [{name}] {line[:120]}")
                        if name == "REBOOT" and not args.no_quiet_chip:
                            # Filters are RAM state; the board just lost them.
                            time.sleep(8)
                            for c in (b"log enable wrn chip\r\n",
                                      b"log enable dbg esp32_wifi\r\n"):
                                ser.write(c)
                                time.sleep(1.0)
                        break

        if time.time() - last_poll >= args.poll:
            last_poll = time.time()
            ser.write(b"wifi status\r\n")
            log.write(f"{time.time()-t0:9.1f}s --- poll: wifi status ---\n")

    ser.close()
    el = time.time() - t0
    summary = [
        "",
        f"=== soak summary: {el/3600:.2f} h ({el:.0f} s) ===",
        f"log file          : {path}",
        f"last device clock : {last_dev_ts or 'none seen'}",
    ] + [f"{name:<18}: {counts[name]}" for name, _ in EVENTS]
    text = "\n".join(summary)
    print(text)
    log.write(text + "\n")
    log.close()

    if counts["REBOOT"]:
        print(f"\nNOTE: {counts['REBOOT']} unexpected reboot(s) after the attach reset. "
              "That is a different bug from the WiFi wedge -- chase it first.")
    if not counts["DISCONNECT"] and el > 3600:
        print("\nNOTE: no disconnects in over an hour -- if the wedge used to appear "
              "at 1.7-6 h, keep going before concluding connectedhomeip patch 0007 fixed it.")


if __name__ == "__main__":
    main()
