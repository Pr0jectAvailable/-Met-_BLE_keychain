#!/usr/bin/env python3
"""Generate an age keypair and save the public key to a separate folder.

Uses the `age-keygen` binary (from the age tool) to mint a fresh keypair,
writes the private key next to it (chmod 600) and stores the public key in
`<out>/age_public.key`. The public key is what you pass to
`provision.py --recipient age1...`.

Requires: age (https://github.com/FiloSottile/age)
    Windows: scoop install age   /   choco install age
    Linux:   apt install age

Usage:
    python gen_age_key.py                 # -> ./keys/age_public.key
    python gen_age_key.py --out ./keys    # explicit folder
    python gen_age_key.py --force         # overwrite existing keys
"""

import argparse
import os
import re
import shutil
import stat
import subprocess
import sys


PUB_RE = re.compile(r"#\s*public key:\s*(age1[0-9a-z]+)")


def find_age_keygen() -> str:
    exe = shutil.which("age-keygen")
    if not exe:
        raise SystemExit(
            "age-keygen not found in PATH. Install age: "
            "https://github.com/FiloSottile/age"
        )
    return exe


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate an age keypair.")
    ap.add_argument("--out", default="./keys",
                    help="folder for the key files (default: ./keys)")
    ap.add_argument("--force", action="store_true",
                    help="overwrite existing key files")
    args = ap.parse_args()

    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    priv_path = os.path.join(out_dir, "age_private.key")
    pub_path = os.path.join(out_dir, "age_public.key")

    for p in (priv_path, pub_path):
        if os.path.exists(p) and not args.force:
            raise SystemExit("%s exists; use --force to overwrite" % p)

    exe = find_age_keygen()
    proc = subprocess.run([exe, "-o", priv_path], capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise SystemExit("age-keygen failed")

    # Restrict permissions on the private key (best-effort on Windows).
    try:
        os.chmod(priv_path, stat.S_IRUSR | stat.S_IWUSR)
    except OSError:
        pass

    with open(priv_path, "r", encoding="utf-8") as f:
        content = f.read()

    m = PUB_RE.search(content)
    if not m:
        raise SystemExit("could not parse public key from age-keygen output")
    public_key = m.group(1)

    with open(pub_path, "w", encoding="utf-8") as f:
        f.write(public_key + "\n")

    print("private key -> %s" % priv_path)
    print("public  key -> %s" % pub_path)
    print("PUBLIC_KEY %s" % public_key)
    print("\nUse it with:\n  python provision.py --count N --recipient %s --out ./batch"
          % public_key)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
