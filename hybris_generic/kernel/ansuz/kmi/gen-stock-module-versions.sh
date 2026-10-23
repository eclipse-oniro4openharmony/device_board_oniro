#!/bin/bash
#
# Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Regenerate stock-module-versions.txt: the union of every (crc, symbol) in
# the __versions tables of the Plinius' stock prebuilt modules —
# halium_vendor_dlkm_a.img + halium_system_dlkm_a.img (pull-halium-blobs.sh)
# and the port repo's vendor_boot module overlay (build_kernel.sh's
# ANSUZ_REPO_SHA checkout).
#
# Usage: gen-stock-module-versions.sh   (writes stock-module-versions.txt)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OHOS_ROOT="$(cd "$HERE/../../../../../../.." && pwd)"
KERNEL_TREE="$OHOS_ROOT/kernel/linux/volla-ansuz"
BLOBS="$OHOS_ROOT/device/board/oniro/hybris_generic/halium-blobs/ansuz"
FSCK_EROFS="$KERNEL_TREE/workdir/downloads/kernel-build-tools/linux-x86/bin/fsck.erofs"

for f in "$BLOBS/halium_vendor_dlkm_a.img" "$BLOBS/halium_system_dlkm_a.img" \
         "$KERNEL_TREE/vendor-ramdisk-overlay/lib/modules" "$FSCK_EROFS"; do
    [ -e "$f" ] || { echo "missing $f (run pull-halium-blobs.sh / build_kernel.sh first)" >&2; exit 1; }
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/vendor_dlkm" "$TMP/system_dlkm"
"$FSCK_EROFS" --extract="$TMP/vendor_dlkm" "$BLOBS/halium_vendor_dlkm_a.img" >/dev/null
"$FSCK_EROFS" --extract="$TMP/system_dlkm" "$BLOBS/halium_system_dlkm_a.img" >/dev/null

python3 - "$TMP/vendor_dlkm" "$TMP/system_dlkm" \
    "$KERNEL_TREE/vendor-ramdisk-overlay/lib/modules" > "$HERE/stock-module-versions.txt" <<'PYEOF'
import os, struct, sys

def versions(path):
    # ELF64 little-endian; __versions is an array of
    # struct modversion_info { unsigned long crc; char name[56]; }
    b = open(path, 'rb').read()
    if b[:4] != b'\x7fELF' or b[4] != 2:
        return []
    shoff, = struct.unpack_from('<Q', b, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', b, 0x3a)
    secs = [struct.unpack_from('<IIQQQQIIQQ', b, shoff + i * shentsize) for i in range(shnum)]
    stroff = secs[shstrndx][4]
    out = []
    for s in secs:
        if b[stroff + s[0]:b.index(b'\0', stroff + s[0])] == b'__versions':
            for e in range(s[4], s[4] + s[5], 64):
                crc, = struct.unpack_from('<Q', b, e)
                out.append((crc & 0xffffffff, b[e + 8:e + 64].split(b'\0')[0].decode()))
    return out

pairs, nmod = set(), 0
for d in sys.argv[1:]:
    for root, _, files in os.walk(d):
        for f in files:
            if f.endswith('.ko'):
                nmod += 1
                pairs.update(versions(os.path.join(root, f)))
for crc, sym in sorted(pairs):
    print(f'0x{crc:08x}\t{sym}')
print(f'{nmod} modules, {len(pairs)} (crc, symbol) pairs', file=sys.stderr)
PYEOF
