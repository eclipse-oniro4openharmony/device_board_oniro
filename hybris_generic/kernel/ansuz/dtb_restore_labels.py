#!/usr/bin/env python3
#
# Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Make the port repo's prebuilt DTB accept the bootstrap v3 dtbo again.
#
# Since port commit 29554f8 (VollaOS 20260828 prebuilts) the base DTB no
# longer has the root node `flashlights_ocp81378`.  The dtbo partition the
# phones carry (bootstrap v3, the same image since v2) still has a fragment
# targeting that label, so LK's overlay apply fails with FDT_ERR_NOTFOUND
# and the phone resets before the kernel starts.  This script puts the node
# back as it was in the older DTB (compatible only; the dtbo supplies the
# pinctrl states and status), with a fresh phandle and a __symbols__ entry.
#
# The DTB is an Android dt_table (magic 0xd7b7ab1e) holding one FDT; the
# table is rewritten in place with the grown FDT.  With --dtbo (repeatable),
# every FDT in each of those dt_tables is applied with fdtoverlay as a check,
# and any failure is an error, so a DTB/dtbo mismatch fails the build
# instead of the boot.
#
# Needs dtc, fdtput and fdtoverlay (Debian/Ubuntu: device-tree-compiler).
#
# Usage: dtb_restore_labels.py DTB [--dtbo DTBO_IMG]...

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

DT_TABLE_MAGIC = 0xd7b7ab1e

# label -> (node path, compatible): nodes the bootstrap dtbo fixes up
# against that newer port DTBs dropped.
RESTORE = {
    "flashlights_ocp81378": ("/flashlights_ocp81378", "mediatek,flashlights_ocp81378"),
}


def die(msg):
    sys.exit("dtb_restore_labels: " + msg)


def read_table(path):
    """Return (header fields, list of entry fields, list of FDT blobs)."""
    data = open(path, "rb").read()
    hdr = list(struct.unpack_from(">8I", data, 0))
    magic, _total, _hsz, esz, count, eoff = hdr[:6]
    if magic != DT_TABLE_MAGIC:
        die(f"{path}: not a dt_table image (magic {magic:#x})")
    entries, blobs = [], []
    for i in range(count):
        ent = list(struct.unpack_from(">8I", data, eoff + i * esz))
        entries.append(ent)
        blobs.append(data[ent[1]:ent[1] + ent[0]])
    return hdr, entries, blobs


def write_table(path, hdr, entries, blobs):
    """Lay out header, entry table, then the FDTs back to back."""
    hsz, esz = hdr[2], hdr[3]
    off = hsz + esz * len(entries)
    body = b""
    for ent, blob in zip(entries, blobs):
        ent[0], ent[1] = len(blob), off + len(body)
        body += blob
    hdr[1] = off + len(body)  # total_size
    hdr[4] = len(entries)
    hdr[5] = hsz              # dt_entries_offset
    out = struct.pack(">8I", *hdr)
    out += b"".join(struct.pack(">8I", *e) for e in entries)
    open(path, "wb").write(out + body)


def run(*cmd, check=True):
    return subprocess.run(cmd, check=check, capture_output=True, text=True)


def has_symbol(fdt, label):
    return run("fdtget", fdt, "/__symbols__", label, check=False).returncode == 0


def restore(fdt):
    changed = False
    for label, (path, compat) in RESTORE.items():
        if has_symbol(fdt, label):
            continue
        dts = run("dtc", "-q", "-I", "dtb", "-O", "dts", fdt).stdout
        handles = [int(h, 16) for h in re.findall(r"phandle = <(0x[0-9a-f]+)>;", dts)]
        phandle = max(handles, default=0) + 1
        run("fdtput", "-c", fdt, path)
        run("fdtput", "-t", "s", fdt, path, "compatible", compat)
        run("fdtput", "-t", "x", fdt, path, "phandle", f"{phandle:#x}")
        run("fdtput", "-t", "s", fdt, "/__symbols__", label, path)
        print(f"dtb_restore_labels: restored {label} at {path} (phandle {phandle:#x})")
        changed = True
    return changed


def check_overlays(fdt, dtbo_img, tmp):
    _, _, overlays = read_table(dtbo_img)
    for i, ov in enumerate(overlays):
        ov_path = os.path.join(tmp, f"ov{i}.dtbo")
        open(ov_path, "wb").write(ov)
        res = run("fdtoverlay", "-i", fdt, "-o", os.devnull, ov_path, check=False)
        if res.returncode:
            die(f"{dtbo_img} entry {i} does not apply: {res.stderr.strip() or res.stdout.strip()}")
    print(f"dtb_restore_labels: {os.path.basename(dtbo_img)}: {len(overlays)} overlay(s) apply cleanly")


def main(argv):
    if not argv or len(argv) % 2 != 1 or any(a != "--dtbo" for a in argv[1::2]):
        die("usage: dtb_restore_labels.py DTB [--dtbo DTBO_IMG]...")
    dtbos = argv[2::2]
    for tool in ("dtc", "fdtget", "fdtput", "fdtoverlay"):
        if not shutil.which(tool):
            die(f"{tool} not found (install device-tree-compiler)")
    dtb = argv[0]
    hdr, entries, blobs = read_table(dtb)
    with tempfile.TemporaryDirectory() as tmp:
        changed = False
        for i, blob in enumerate(blobs):
            fdt = os.path.join(tmp, f"dt{i}.fdt")
            open(fdt, "wb").write(blob)
            if restore(fdt):
                blobs[i] = open(fdt, "rb").read()
                changed = True
            for dtbo in dtbos:
                check_overlays(fdt, dtbo, tmp)
        if changed:
            write_table(dtb, hdr, entries, blobs)


if __name__ == "__main__":
    main(sys.argv[1:])
