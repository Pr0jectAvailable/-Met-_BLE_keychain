#!/usr/bin/env python3
"""Met keychain owner-unbind client (K_dev-authenticated, works without the owner).

Use when the owner/app key (K_owner) is lost or the device must be handed to a
new owner but its provision (devNo/K_dev/K_pair) must be kept. The device proves
knowledge of K_dev (factory/server secret, never sent in clear) before wiping the
owner binding, the encounter journal and the RAM clock. devNo/K_dev/K_pair and
the monotonic seq are preserved, so the unit can be paired again with the same
K_pair.

Firmware side (src/ble_uart_module.cpp):
  UNBIND_BEGIN  -> NONCE <hex16>
  UNBIND_FINISH <hmac32> -> OK dev=<devNo>   (HMAC-SHA256(K_dev, nonce))

Unlike UNPAIR this needs no owner session and no server nonce.

Requires: bleak

    pip install bleak

Examples:
    # list Met devices in range (pairing and anonymous beacons)
    python unbind.py --list

    # unbind using a K_dev given on the command line
    python unbind.py --kdev 00112233445566778899aabbccddeeff \
        --address AA:BB:CC:DD:EE:FF

    # unbind using one row from the age-encrypted backend CSV (in memory)
    python unbind.py --csv-age ./batch/backend_keys.csv.age --row 3
"""

import argparse
import asyncio
import hashlib
import hmac
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from flash_keys import (  # noqa: E402
    Lines,
    MET_COMPANY_ID,
    NUS_TX,
    send_command,
    resolve_target,
    rows_from_age,
    row_from_rows,
    row_from_csv,
)


async def scan_met_all(timeout: float):
    """Return {ADDRESS: {type, devno, rssi, name}} for any Met beacon.

    Both pairing beacons (type=0x02, payload=devNo) and anonymous beacons
    (type=0x01) are collected, so a paired unit that is already advertising its
    rotating anonymous ID can still be located.
    """
    from bleak import BleakScanner

    found = {}

    def cb(device, adv):
        md = adv.manufacturer_data.get(MET_COMPANY_ID)
        if not md or len(md) < 5 or md[0:3] != b"Met":
            return
        adv_type = md[4]
        devno = md[5:].decode("ascii", "replace") if adv_type == 0x02 else ""
        found[device.address.upper()] = {
            "type": adv_type,
            "devno": devno,
            "rssi": adv.rssi,
            "name": device.name or "",
        }

    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    await asyncio.sleep(timeout)
    await scanner.stop()
    return found


async def unbind_one(address, kdev_hex, timeout):
    from bleak import BleakClient

    kdev = bytes.fromhex(kdev_hex)
    if len(kdev) != 16:
        raise SystemExit("K_dev must be 16 bytes (32 hex chars)")

    lines = Lines()
    async with BleakClient(address, timeout=timeout) as client:
        await client.start_notify(NUS_TX, lambda _s, d: lines.feed(d))
        try:
            hello = await send_command(client, lines, 1, "HELLO", timeout=timeout)
            print("HELLO -> " + " | ".join(hello))
        except TimeoutError:
            pass

        rb = await send_command(client, lines, 2, "UNBIND_BEGIN", timeout=timeout)
        nonce_hex = next((x.split(" ", 1)[1].strip()
                          for x in rb if x.startswith("NONCE ")), None)
        if not nonce_hex:
            raise SystemExit("UNBIND_BEGIN failed: %s" % " | ".join(rb))
        proof = hmac.new(kdev, bytes.fromhex(nonce_hex), hashlib.sha256).digest()

        rf = await send_command(client, lines, 3, "UNBIND_FINISH",
                                proof.hex(), timeout)
        print("UNBIND_FINISH -> " + " | ".join(rf))
        if any(x.startswith("ERR") for x in rf):
            raise SystemExit(
                "owner unbind failed: %s (is --kdev the device's current K_dev?)"
                % " | ".join(rf))

        await client.stop_notify(NUS_TX)

    print("OK owner unbound (provision kept)")


def resolve_kdev(args):
    if args.kdev:
        return args.kdev.strip()
    if args.csv:
        _devno, kdev_hex, _kpair_hex = row_from_csv(args.csv, args.row)
        return kdev_hex
    if args.csv_age:
        rows = rows_from_age(args.csv_age, args.age_key)
        _devno, kdev_hex, _kpair_hex = row_from_rows(rows, args.row)
        return kdev_hex
    raise SystemExit("provide --kdev, --csv or --csv-age")


async def async_main(args):
    if args.list:
        found = await scan_met_all(args.scan_time)
        if not found:
            print("no Met beacons found")
            return 1
        for addr, info in found.items():
            kind = "pair" if info["type"] == 0x02 else "anon"
            print("%-18s %-4s devno=%-10s rssi=%s name=%s"
                  % (addr, kind, info["devno"], info["rssi"], info["name"]))
        return 0

    kdev_hex = resolve_kdev(args)
    if len(bytes.fromhex(kdev_hex)) != 16:
        raise SystemExit("K_dev must be 32 hex chars")

    found = {}
    if not args.address:
        found = await scan_met_all(args.scan_time)
    target = resolve_target(args.address, found)

    await unbind_one(target, kdev_hex, args.timeout)
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Unbind the owner from a Met keychain (K_dev-authenticated).")
    ap.add_argument("--list", action="store_true",
                    help="scan and list Met devices (pair and anon beacons)")
    ap.add_argument("--address", help="target BLE address (AA:BB:CC:DD:EE:FF)")
    ap.add_argument("--kdev", help="device K_dev (hex32) to authenticate the unbind")
    ap.add_argument("--csv", help="plaintext CSV with dev_no,k_dev_hex,k_pair_hex")
    ap.add_argument("--csv-age",
                    help="age-encrypted backend_keys.csv.age (decrypted in memory)")
    ap.add_argument("--age-key", default="./keys/age_private.key",
                    help="age private key for --csv-age (default: ./keys/age_private.key)")
    ap.add_argument("--row", type=int, default=1,
                    help="1-based data row for --csv/--csv-age")
    ap.add_argument("--scan-time", type=float, default=8.0)
    ap.add_argument("--timeout", type=float, default=10.0)
    args = ap.parse_args()
    try:
        return asyncio.run(async_main(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
