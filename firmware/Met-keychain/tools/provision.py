#!/usr/bin/env python3
"""Met keychain provisioning tool.

Generates per-device identifiers/keys for a batch, emits an encrypted CSV for
the backend and a QR image per device. The plaintext CSV MUST NOT be written to
disk, sent via messengers or committed to the repository.

Requires: qrcode[pil] (for PNG QR), age or gpg (for encryption).

    pip install "qrcode[pil]"

Usage:
    # one-time: generate an age keypair (public key -> keys/age_public.key)
    python gen_age_key.py

    # provision a batch (recipient read from keys/age_public.key by default)
    python provision.py --count 10 --start 1 --out ./batch

    # or point at an explicit recipient / file
    python provision.py --count 10 --recipient age1... --out ./batch
    python provision.py --count 10 --recipient-file ./keys/age_public.key
"""

import argparse
import base64
import csv
import io
import os
import secrets
import subprocess
import sys


def rand(n: int) -> bytes:
    return secrets.token_bytes(n)


def b32(data: bytes) -> str:
    """RFC 4648 base32 without padding (QR-friendly, upper case)."""
    return base64.b32encode(data).decode().rstrip("=")


# Mirrors firmware derive: K_log/K_time/K_rcpt = HKDF-SHA256(K_dev, info).
# Only informational; the firmware derives them on-device. Backend needs K_dev.
HKDF_INFO = {
    "k_log": "met-log-v1",
    "k_time": "met-time-v1",
    "k_rcpt": "met-pair-v1",
}


def make_device(index: int) -> dict:
    dev_no = "MET-%05d" % index
    k_dev = rand(16)
    k_pair = rand(16)
    return {
        "dev_no": dev_no,
        "k_dev_hex": k_dev.hex(),
        "k_pair_hex": k_pair.hex(),
        "k_pair_b32": b32(k_pair),
    }


def pair_uri(dev: dict) -> str:
    return "met://pair?d=%s&k=%s" % (dev["dev_no"], dev["k_pair_b32"])


def write_qr(dev: dict, out_dir: str) -> str:
    try:
        import qrcode
    except ImportError:
        return ""
    img = qrcode.make(pair_uri(dev))
    path = os.path.join(out_dir, dev["dev_no"] + ".png")
    img.save(path)
    return path


def encrypt_csv(csv_bytes: bytes, recipient: str, out_path: str) -> None:
    if recipient.startswith("age1"):
        cmd = ["age", "-r", recipient, "-o", out_path]
    else:
        cmd = ["gpg", "--encrypt", "--recipient", recipient,
               "--output", out_path, "--yes"]
    proc = subprocess.run(cmd, input=csv_bytes, capture_output=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode(errors="replace"))
        raise SystemExit("encryption failed (age/gpg?)")


def read_recipient_file(path: str) -> str:
    """Return the first non-empty, non-comment line of an age public key file."""
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                return line
    raise SystemExit("no public key found in %s" % path)


def resolve_recipient(args) -> str:
    if args.recipient:
        return args.recipient
    if os.path.exists(args.recipient_file):
        return read_recipient_file(args.recipient_file)
    raise SystemExit(
        "no recipient: pass --recipient age1... or generate a keypair with "
        "gen_age_key.py (expected %s)" % args.recipient_file)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, required=True)
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--recipient",
                    help="age public key (age1...) or gpg recipient")
    ap.add_argument("--recipient-file", default="./keys/age_public.key",
                    help="file with the age public key (default: ./keys/age_public.key)")
    ap.add_argument("--out", default="./batch")
    ap.add_argument("--firmware", default="",
                    help="optional: push to device via nrfutil/ble; see README")
    args = ap.parse_args()

    recipient = resolve_recipient(args)
    os.makedirs(args.out, exist_ok=True)

    buf = io.StringIO()
    writer = csv.writer(buf)
    writer.writerow(["dev_no", "k_dev_hex", "k_pair_hex", "pair_uri"])

    for i in range(args.start, args.start + args.count):
        dev = make_device(i)
        writer.writerow([dev["dev_no"], dev["k_dev_hex"],
                         dev["k_pair_hex"], pair_uri(dev)])
        qr = write_qr(dev, args.out)
        print("provisioned %s qr=%s" % (dev["dev_no"], qr or "(qrcode missing)"))

    csv_bytes = buf.getvalue().encode()
    out_csv = os.path.join(args.out, "backend_keys.csv.age")
    encrypt_csv(csv_bytes, recipient, out_csv)
    print("encrypted CSV -> %s" % out_csv)
    print("plaintext CSV kept in memory only, not written to disk.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
