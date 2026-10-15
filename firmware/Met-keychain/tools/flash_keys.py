#!/usr/bin/env python3
"""Met keychain provisioning client: writes devNo / K_dev / K_pair into the
nRF52840 over the BLE Nordic UART Service (NUS).

Firmware side (src/ble_uart_module.cpp):
  PROVISION <devNo> <kDev_hex32> <kPair_hex32>
      accepted ONLY while the device has never been provisioned (K_dev == 0)
      and is not paired. Writes /keys.dat, returns `OK dev=<devNo>`.
  PROV_VERIFY <nonce_hex16>
      returns HMAC-SHA256(K_dev, nonce)[0:16], so the host can confirm the key
      was stored without the key ever leaving the device (task.txt rule #2).

Requires: bleak

    pip install bleak

Examples:
    # list Met devices currently advertising as pairing beacons
    python flash_keys.py --list

    # mint keys, flash one device, print the backend row on stdout
    python flash_keys.py --gen --index 1 --address AA:BB:CC:DD:EE:FF

    # flash explicit keys
    python flash_keys.py --devno MET-00001 \\
        --kdev 00112233445566778899aabbccddeeff \\
        --kpair ffeeddccbbaa99887766554433221100 --address AA:BB:CC:DD:EE:FF

    # flash one row from a plaintext CSV (dev_no,k_dev_hex,k_pair_hex,...)
    python flash_keys.py --csv ./plain.csv --row 3 --address AA:BB:CC:DD:EE:FF

    # flash one row straight from the age-encrypted backend CSV (in memory)
    python flash_keys.py --csv-age ./batch/backend_keys.csv.age --row 3
"""

import argparse
import asyncio
import csv
import hashlib
import hmac
import io
import os
import secrets
import sys

# Reuse the generator from provision.py so key/QR formats stay in sync.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from provision import make_device, pair_uri
    _HAS_PROVISION = True
except Exception:  # standalone fallback
    _HAS_PROVISION = False

    def make_device(index):
        import base64
        def b32(d):
            return base64.b32encode(d).decode().rstrip("=")
        dev_no = "MET-%05d" % index
        k_dev = secrets.token_bytes(16)
        k_pair = secrets.token_bytes(16)
        return {
            "dev_no": dev_no,
            "k_dev_hex": k_dev.hex(),
            "k_pair_hex": k_pair.hex(),
            "k_pair_b32": b32(k_pair),
        }

    def pair_uri(dev):
        return "met://pair?d=%s&k=%s" % (dev["dev_no"], dev["k_pair_b32"])


# Nordic UART Service (matches Adafruit Bluefruit BLEUart used in firmware).
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # host -> device (write)
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # device -> host (notify)

MET_COMPANY_ID = 0xFFFF
MET_ADV_TYPE_ANON = 0x01
MET_ADV_TYPE_PAIR = 0x02


class Lines:
    """Reassemble newline-terminated NUS notifications into a queue of lines."""

    def __init__(self):
        self.q = asyncio.Queue()
        self.buf = ""

    def feed(self, data: bytes) -> None:
        self.buf += data.decode("utf-8", "replace")
        while "\n" in self.buf:
            line, self.buf = self.buf.split("\n", 1)
            line = line.strip()
            if line:
                self.q.put_nowait(line)


async def scan_met(timeout: float):
    """Return {ADDRESS: {kind, devno, anon, rssi, name}} for Met keychains.

    Matches ANY Met advertisement (pairing beacon OR paired anonymous ID) so
    that --list shows only our keychains, never unrelated BLE devices.
    """
    from bleak import BleakScanner

    found = {}

    def cb(device, adv):
        md = adv.manufacturer_data.get(MET_COMPANY_ID)
        if not md or len(md) < 5 or md[0:3] != b"Met":
            return
        adv_type = md[4]
        if adv_type == MET_ADV_TYPE_PAIR:
            kind = "pair"
            devno = md[5:].decode("ascii", "replace")
            anon = ""
        elif adv_type == MET_ADV_TYPE_ANON and len(md) >= 5 + 16:
            kind = "anon"
            devno = ""
            anon = md[5:21].hex()
        else:
            return
        found[device.address.upper()] = {
            "kind": kind,
            "devno": devno,
            "anon": anon,
            "rssi": adv.rssi,
            "name": device.name or "",
        }

    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    await asyncio.sleep(timeout)
    await scanner.stop()
    return found


async def send_command(client, lines, cmd_id, cmd, args="", timeout=8.0):
    """Send `<id> <cmd> [args]\n`, collect response lines until END/ERR."""
    payload = "%d %s %s\n" % (cmd_id, cmd, args) if args else "%d %s\n" % (cmd_id, cmd)
    await client.write_gatt_char(NUS_RX, payload.encode("ascii"), response=False)
    prefix = "%d " % cmd_id
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    collected = []
    while True:
        remaining = deadline - loop.time()
        if remaining <= 0:
            raise TimeoutError("no response for id=%d (%s)" % (cmd_id, cmd))
        line = await asyncio.wait_for(lines.q.get(), remaining)
        if not line.startswith(prefix):
            continue
        body = line[len(prefix):].strip()
        collected.append(body)
        if body.startswith("END") or body.startswith("ERR"):
            return collected


async def flash_one(address, devno, kdev_hex, kpair_hex, timeout, reset=False,
                    reset_kdev_hex=None, reset_candidates=None):
    from bleak import BleakClient

    kdev = bytes.fromhex(kdev_hex)
    kpair = bytes.fromhex(kpair_hex)
    if len(kdev) != 16 or len(kpair) != 16:
        raise SystemExit("kDev/kPair must be 16 bytes (32 hex chars)")
    # Candidate K_dev values used only to authenticate the manufacturing reset
    # of an already-paired unit. The device's current K_dev is tried first,
    # then any other candidate. Never sent over BLE (only an HMAC is).
    reset_candidates = list(reset_candidates or [])
    if reset_kdev_hex:
        reset_candidates.insert(0, reset_kdev_hex)
    if kdev_hex not in reset_candidates:
        reset_candidates.insert(0, kdev_hex)
    for c in reset_candidates:
        if len(bytes.fromhex(c)) != 16:
            raise SystemExit("reset candidate K_dev must be 32 hex chars")

    lines = Lines()
    async with BleakClient(address, timeout=timeout) as client:
        await client.start_notify(NUS_TX, lambda _s, d: lines.feed(d))
        try:
            await send_command(client, lines, 1, "HELLO", timeout=timeout)
        except TimeoutError:
            pass  # best-effort info

        if reset:
            r = await send_command(client, lines, 2, "FACTORY_RESET", timeout=timeout)
            print("FACTORY_RESET -> " + " | ".join(r))
            if any(x.startswith("ERR ALREADY_PAIRED") for x in r):
                # Paired device: prove knowledge of the current on-device K_dev
                # (factory secret) to wipe it. If the on-device K_dev differs
                # from the row being flashed this fails and the unit must be
                # erased over USB (see FLASH_AND_PAIR.md).
                wiped = False
                for cand in reset_candidates:
                    rb = await send_command(client, lines, 21,
                                            "FACTORY_RESET_BEGIN", timeout=timeout)
                    nonce_hex = next((x.split(" ", 1)[1].strip()
                                      for x in rb if x.startswith("NONCE ")), None)
                    if not nonce_hex:
                        raise SystemExit("factory reset failed: %s" % " | ".join(rb))
                    proof = hmac.new(bytes.fromhex(cand),
                                     bytes.fromhex(nonce_hex),
                                     hashlib.sha256).digest()
                    rf = await send_command(client, lines, 22,
                                            "FACTORY_RESET_FINISH",
                                            proof.hex(), timeout)
                    if not any(x.startswith("ERR") for x in rf):
                        print("FACTORY_RESET_FINISH -> " + " | ".join(rf))
                        wiped = True
                        break
                if not wiped:
                    raise SystemExit(
                        "device is paired and its current K_dev is not among "
                        "the supplied keys; pass --current-kdev <hex32> (the "
                        "key it was provisioned with) or erase over USB "
                        "(pio run -t erase / SWD --eraseall)")
            elif any(x.startswith("ERR") for x in r):
                raise SystemExit("factory reset failed: %s" % " | ".join(r))

        prov_args = "%s %s %s" % (devno, kdev.hex(), kpair.hex())
        resp = await send_command(client, lines, 3, "PROVISION", prov_args, timeout)
        print("PROVISION -> " + " | ".join(resp))
        if any(r.startswith("ERR") for r in resp):
            raise SystemExit("provisioning failed: %s" % " | ".join(resp))

        nonce = secrets.token_bytes(16)
        verify = await send_command(client, lines, 4, "PROV_VERIFY", nonce.hex(), timeout)
        print("PROV_VERIFY -> " + " | ".join(verify))
        mac_field = None
        for r in verify:
            if r.startswith("PROV") and "mac=" in r:
                mac_field = r.split("mac=", 1)[1].strip()
        if not mac_field:
            raise SystemExit("verification failed: no MAC returned")
        expected = hmac.new(kdev, nonce, hashlib.sha256).digest()[:16].hex()
        if not hmac.compare_digest(mac_field.lower(), expected):
            raise SystemExit("verification failed: MAC mismatch (got %s want %s)"
                             % (mac_field, expected))

        await client.stop_notify(NUS_TX)

    print("OK %s provisioned and verified" % devno)


def row_from_rows(rows, row_no):
    if row_no < 1 or row_no > len(rows):
        raise SystemExit("row %d out of range (1..%d)" % (row_no, len(rows)))
    r = rows[row_no - 1]
    return r["dev_no"], r["k_dev_hex"], r["k_pair_hex"]


def row_from_csv(path, row_no):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    return row_from_rows(rows, row_no)


def rows_from_age(path, key_path):
    """Decrypt an age-encrypted backend CSV in memory and return all rows.

    The plaintext is never written to disk (task.txt secret-handling rule).
    """
    import shutil
    import subprocess
    exe = shutil.which("age")
    if not exe:
        raise SystemExit("age not found in PATH; install age to use --csv-age")
    if not os.path.exists(key_path):
        raise SystemExit("private key not found: %s (run gen_age_key.py)" % key_path)
    proc = subprocess.run([exe, "--decrypt", "-i", key_path, path],
                          capture_output=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode(errors="replace"))
        raise SystemExit("age decrypt failed")
    text = proc.stdout.decode()
    return list(csv.DictReader(io.StringIO(text)))


def row_from_age(path, row_no, key_path):
    return row_from_rows(rows_from_age(path, key_path), row_no)


def resolve_target(address, found):
    if address:
        return address.upper()
    if len(found) == 1:
        return next(iter(found))
    raise SystemExit("multiple/no Met devices found; pass --address. Found: %s"
                     % ", ".join("%s(%s)" % (a, v["devno"])
                                 for a, v in found.items()))


async def async_main(args):
    if args.list:
        found = await scan_met(args.scan_time)
        if not found:
            print("no Met keychains found")
            return 1
        for addr, info in found.items():
            if info["kind"] == "pair":
                ident = "devno=%-10s" % info["devno"]
            else:
                ident = "anon=%s" % info["anon"]
            print("%-18s %-8s %s rssi=%-4s name=%s"
                  % (addr, info["kind"], ident, info["rssi"], info["name"]))
        return 0

    reset_candidates = []
    if args.gen:
        dev = make_device(args.index)
        devno, kdev_hex, kpair_hex = dev["dev_no"], dev["k_dev_hex"], dev["k_pair_hex"]
    elif args.csv:
        devno, kdev_hex, kpair_hex = row_from_csv(args.csv, args.row)
    elif args.csv_age:
        rows = rows_from_age(args.csv_age, args.age_key)
        devno, kdev_hex, kpair_hex = row_from_rows(rows, args.row)
        # Any row's K_dev may be the key currently on the device (e.g. a unit
        # reused across batches); try them all to authenticate the reset.
        reset_candidates = [r["k_dev_hex"] for r in rows
                            if r.get("k_dev_hex") and r["k_dev_hex"] != kdev_hex]
    else:
        if not (args.devno and args.kdev and args.kpair):
            raise SystemExit(
                "provide --gen, --csv, --csv-age, or --devno/--kdev/--kpair")
        devno, kdev_hex, kpair_hex = args.devno, args.kdev, args.kpair

    found = {}
    if not args.address:
        found = await scan_met(args.scan_time)
    target = resolve_target(args.address, found)

    await flash_one(target, devno, kdev_hex, kpair_hex, args.timeout,
                    reset=args.reset, reset_kdev_hex=args.current_kdev,
                    reset_candidates=reset_candidates)

    if args.gen:
        print("BACKEND_ROW %s,%s,%s" % (devno, kdev_hex, kpair_hex))
        print("PAIR_URI %s" % pair_uri(dev))
    return 0


def main():
    ap = argparse.ArgumentParser(description="Flash Met keychain keys over BLE (NUS).")
    ap.add_argument("--list", action="store_true", help="scan and list Met devices")
    ap.add_argument("--address", help="target BLE address (AA:BB:CC:DD:EE:FF)")
    ap.add_argument("--gen", action="store_true", help="mint new keys and flash")
    ap.add_argument("--index", type=int, default=1, help="device index for --gen")
    ap.add_argument("--reset", action="store_true",
                    help="FACTORY_RESET first; on a paired device uses a "
                         "K_dev-authenticated wipe")
    ap.add_argument("--current-kdev",
                    help="K_dev currently on the device (hex32), to authenticate "
                         "--reset of an already-paired unit. Defaults to the row's K_dev")
    ap.add_argument("--csv", help="plaintext CSV with dev_no,k_dev_hex,k_pair_hex")
    ap.add_argument("--csv-age",
                    help="age-encrypted backend_keys.csv.age (decrypted in memory)")
    ap.add_argument("--age-key", default="./keys/age_private.key",
                    help="age private key for --csv-age (default: ./keys/age_private.key)")
    ap.add_argument("--row", type=int, default=1, help="1-based data row for --csv/--csv-age")
    ap.add_argument("--devno")
    ap.add_argument("--kdev")
    ap.add_argument("--kpair")
    ap.add_argument("--scan-time", type=float, default=8.0)
    ap.add_argument("--timeout", type=float, default=10.0)
    args = ap.parse_args()
    try:
        return asyncio.run(async_main(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
