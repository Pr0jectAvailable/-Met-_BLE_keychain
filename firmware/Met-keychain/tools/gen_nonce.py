#!/usr/bin/env python3
"""Generate a server-side pairing nonce (16 random bytes, hex).

The backend reserves a device and issues a one-time `nonce` that the phone
supplies to the keychain in `PAIR_FINISH <resp> <kowner> <nonce> <utc>` (and
in `UNPAIR <nonce>`). This helper mints such a nonce for lab testing or as a
stand-in for the real backend (see PROTOCOL.md, 'Server order (pair)').

Usage:
    python gen_nonce.py                 # -> one nonce (hex32)
    python gen_nonce.py --count 5       # -> five nonces
    python gen_nonce.py --json          # -> JSON array
"""

import argparse
import json
import secrets


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate a 16-byte pairing nonce (hex).")
    ap.add_argument("--count", type=int, default=1, help="how many nonces (default 1)")
    ap.add_argument("--json", action="store_true", help="print a JSON array")
    args = ap.parse_args()

    if args.count < 1:
        raise SystemExit("--count must be >= 1")

    nonces = [secrets.token_bytes(16).hex() for _ in range(args.count)]

    if args.json:
        print(json.dumps(nonces))
    else:
        for n in nonces:
            print(n)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
