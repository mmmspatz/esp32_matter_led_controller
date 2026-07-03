#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Provision per-device Matter pairing codes over the USB console.

Run against a device flashed with the prov.conf build variant (the
production image has no shell):

    west build -b btf_wled_esp32/esp32/procpu app -- -DEXTRA_CONF_FILE=prov.conf
    west flash
    ./scripts/provision.py --port /dev/ttyUSB0
    west build -b btf_wled_esp32/esp32/procpu app && west flash

Generates a random passcode/discriminator/salt, computes the SPAKE2+
verifier with CHIP's spake2p tool, writes everything to the device's
chip-fct settings namespace (which survives reflashing and factory
reset), prints the QR / manual pairing code, and appends a record to
devices/ (gitignored: pairing codes are secrets).
"""

import argparse
import base64
import datetime
import json
import pathlib
import secrets
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
CHIP = REPO / "modules" / "connectedhomeip"
sys.path.insert(0, str(CHIP / "src" / "setup_payload" / "python"))
from SetupPayload import SetupPayload  # noqa: E402

import serial  # noqa: E402

# Test VID/PID: must match CHIPProjectConfig.h while the example DAC is in use.
VENDOR_ID = 0xFFF1
PRODUCT_ID = 0x8005
DISCOVERY_BLE = 2  # discovery-capabilities bitmask: BLE

INVALID_PASSCODES = {
    0, 11111111, 22222222, 33333333, 44444444, 55555555,
    66666666, 77777777, 88888888, 99999999, 12345678, 87654321,
}


def random_passcode() -> int:
    while True:
        code = secrets.randbelow(99999998) + 1
        if code not in INVALID_PASSCODES:
            return code


def gen_verifier(passcode: int, salt_b64: str, iterations: int) -> str:
    out = subprocess.run(
        [sys.executable, str(CHIP / "scripts" / "tools" / "spake2p" / "spake2p.py"),
         "gen-verifier", "-p", str(passcode), "-s", salt_b64, "-i", str(iterations)],
        check=True, capture_output=True, text=True)
    return out.stdout.strip()


def shell_cmd(port: serial.Serial, cmd: str, expect: str, timeout: float = 5.0) -> str:
    port.reset_input_buffer()
    # Pace the write; the console shell drains its RX ring at UART speed.
    data = (cmd + "\r\n").encode()
    for i in range(0, len(data), 32):
        port.write(data[i:i + 32])
        time.sleep(0.02)
    deadline = time.time() + timeout
    buf = ""
    while time.time() < deadline:
        buf += port.read(4096).decode(errors="replace")
        if expect in buf or "PROV_ERR" in buf:
            break
    if expect not in buf:
        raise RuntimeError(f"device did not answer {expect!r} to {cmd.split()[0]}:\n{buf}")
    return buf


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--passcode", type=int, help="default: random")
    ap.add_argument("--discriminator", type=int, help="0..4095, default: random")
    ap.add_argument("--iterations", type=int, default=10000)
    args = ap.parse_args()

    passcode = args.passcode or random_passcode()
    discriminator = args.discriminator if args.discriminator is not None else secrets.randbelow(4096)
    salt_b64 = base64.b64encode(secrets.token_bytes(32)).decode()
    verifier_b64 = gen_verifier(passcode, salt_b64, args.iterations)

    port = serial.Serial(args.port, 115200, timeout=0.2)
    shell_cmd(port, f"matter_prov set {passcode} {discriminator} {args.iterations} {salt_b64} {verifier_b64}",
              "PROV_OK")
    shell_cmd(port, "matter_prov show", "PROV_SHOW provisioned")

    # Reset so the new commissionable data takes effect.
    port.setDTR(False)
    port.setRTS(True)
    time.sleep(0.1)
    port.setRTS(False)
    port.close()

    payload = SetupPayload(discriminator=discriminator, pincode=passcode,
                           rendezvous=DISCOVERY_BLE, vid=VENDOR_ID, pid=PRODUCT_ID)
    qr = payload.generate_qrcode()
    manual = payload.generate_manualcode()

    record = {
        "provisioned_at": datetime.datetime.now().isoformat(timespec="seconds"),
        "passcode": passcode,
        "discriminator": discriminator,
        "iterations": args.iterations,
        "salt_b64": salt_b64,
        "verifier_b64": verifier_b64,
        "qr_payload": qr,
        "manual_code": manual,
        "vendor_id": VENDOR_ID,
        "product_id": PRODUCT_ID,
    }
    devices = REPO / "devices"
    devices.mkdir(exist_ok=True)
    record_path = devices / f"device-{discriminator:04d}.json"
    record_path.write_text(json.dumps(record, indent=2) + "\n")

    print(f"\nProvisioned. Record: {record_path}")
    print(f"  Manual pairing code: {manual}")
    print(f"  QR payload:          {qr}")
    try:
        import qrcode
        q = qrcode.QRCode()
        q.add_data(qr)
        q.print_ascii(invert=True)
    except ImportError:
        print("  (pip install qrcode for a terminal QR image)")
    print("Flash the production image next; the pairing data persists.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
