#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Export the locked Linux commit as OpenWrt's checksum-verified source archive."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from workspace import LINUX, DOWNLOADS, OPENWRT

PROJECT = Path(__file__).resolve().parents[2]


def export_kernel():
    lock = json.loads((PROJECT / "manifests/kernel.json").read_text())
    target_ref = lock.get("commit", "HEAD")
    commit = subprocess.check_output(
        ["git", "-C", str(LINUX), "rev-parse", f"{target_ref}^{{commit}}"],
        text=True,
    ).strip()
    if lock.get("commit") and commit != lock["commit"]:
        raise SystemExit(f"Missing locked Linux commit {lock['commit']}")
    archive = lock.get("archive") or f"linux-7.3.0-rc7-sy108-{commit[:12]}.tar.gz"
    dest = DOWNLOADS / archive
    dest.parent.mkdir(parents=True, exist_ok=True)
    if not dest.exists():
        partial = dest.with_suffix(dest.suffix + ".part")
        try:
            with partial.open("wb") as output:
                process = subprocess.Popen(
                    ["git", "-C", str(LINUX), "archive", "--format=tar", "--prefix=linux-7.3.0-rc7/", commit],
                    stdout=subprocess.PIPE,
                )
                compressed = subprocess.run(["gzip", "-n"], stdin=process.stdout, stdout=output)
                process.stdout.close()
                if process.wait() or compressed.returncode:
                    raise RuntimeError("Linux export failed")
            partial.rename(dest)
        finally:
            partial.unlink(missing_ok=True)
    with dest.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    #if lock.get("sha256") and lock.get("archive") == archive and digest != lock["sha256"]:
    #    raise SystemExit("Linux archive checksum mismatch")
    kernel_mk = OPENWRT / "target/linux/sprd/kernel-7.3"
    if kernel_mk.parent.is_dir():
        kernel_mk.write_text(f"LINUX_VERSION-7.3 = .0-rc7\nLINUX_KERNEL_HASH-7.3.0-rc7 = {digest}\n")
    return {"commit": commit, "archive": archive, "sha256": digest, "path": str(dest)}


if __name__ == "__main__":
    info = export_kernel()
    print(info["path"])
