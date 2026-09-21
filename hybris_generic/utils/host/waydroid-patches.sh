#!/usr/bin/env bash
# waydroid-patches.sh — regenerate the two binary patches the Waydroid
# container needs, from the sha256-pinned images (plan: android_app_compat_plan.md
# §W3, gralloc bridge: device/soc/oniro/hybris_generic/waydroid/gralloc_bridge/).
#
# They used to exist only as hand-patched blobs in an untracked cache, so a
# deleted tree lost them.  Everything here is derived from images.manifest:
# extract with debugfs, rewrite a handful of instruction words, verify the
# result against a pinned sha256.  Both originals and both results are pinned,
# so a changed image fails loudly instead of producing a silently wrong blob.
#
#   waydroid-patches.sh            # regenerate (idempotent)
#   waydroid-patches.sh --verify   # only check existing files against the pins
#
# ── composer-service ────────────────────────────────────────────────────────
# = /bin/hw/android.hardware.graphics.composer@2.1-service from the pinned
# vendor.img with ONE instruction changed:
#
#     0x7138: 52800080  mov w0, #4   ->   52800020  mov w0, #1
#
# i.e. main()'s configureRpcThreadpool(4, true) becomes (1, true).
#
# Why: the waydroid hwc module (hwcomposer.waydroid.so, hwc_open) spawns
# hwc_binder_thread, which calls configureRpcThreadpool(1, true) before
# registering the vendor.waydroid.{display,window,clipboard} HALs.  If that
# thread runs after main() has started the hwbinder pool (max 4), libhidlbase
# aborts: "Binder threadpool cannot be shrunk after starting".  A pure startup
# race that this container loses every boot; with both callers asking for 1 the
# shrink condition can never hold.  waydroidd bind-mounts the result over the
# vendor image copy.  NOTE: it must be mode 0755 — bound over an executable.
#
# Re-derive if the pinned vendor image changes:
#   llvm-objdump -d composer-service | grep -B2 configureRpcThreadpool
#
# ── libui.so ────────────────────────────────────────────────────────────────
# = /system/lib64/libui.so from the pinned system.img with
# Gralloc4Mapper::createDescriptor patched to CLEAR the buffer usage to its low
# 24 bits instead of rejecting phantom high bits.  Three instructions at the
# usage-validation check (0x31b64):
#
#     ands x3,x8,x11 ; b.eq OK ; adrp x1,..  (reject: "invalid usage bits", -22)
#   ->
#     and x11,x11,#0xffffff   usage &= 0xffffff        92405d6b
#     str x11,[x0,#32]        write the clean usage    f900100b
#     b   0x31b8c             continue the OK path     14000008
#
# Why: the container is A13 (lineage-20) but the host Mali gralloc is A14 (MTK
# mt6878).  A13 libui builds a BufferDescriptorInfo whose usage carries phantom
# bits that the A14 Mali mapper rejects (attribute mask usage & 0xFFFE08282400)
# and that A13 libui's own validUsageBits rejects too.  SurfaceFlinger only ever
# requests 0x1b00 (HW_TEXTURE|HW_RENDER|HW_COMPOSER|HW_FB), none of them bad, so
# clearing usage to 0xffffff yields exactly the descriptor the A14 mapper wants.
#
# Re-derive if the pinned system image changes:
#   grep -abo 'invalid usage bits' libui.so  -> the ands/b.eq just above the
#   __android_log_print reject in Gralloc4Mapper::createDescriptor.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE_ROOT="$(cd "$SCRIPT_DIR/../../../../../.." && pwd)"
BLOB_DIR="$TREE_ROOT/halium-blobs/waydroid"
OUT_DIR="$BLOB_DIR/patches"

# image : path inside it : output : sha256(extracted) : sha256(patched)
SPECS=(
"vendor.img|/bin/hw/android.hardware.graphics.composer@2.1-service|composer-service|eaf399d0bd3c507db9d6d1106b70f611485291a09941a5a0eb6e70ced3077534|201d930fd1a7338dcfec25e2863e7e73288e4f8d809a433ca5d868301ee036ee"
"system.img|/system/lib64/libui.so|libui.so|ac348042db0e873f8b6d443ec545e961b14665e9e262da9f6a433a0b6da67c07|dd6a1a868c39ae75ab7e33ebe9b858bc4e3833fa932a9541824dc554f5dd4512"
)
# output : offset : expected-original-word : replacement-word  (little-endian)
PATCHES=(
"composer-service|0x7138|0x52800080|0x52800020"
"libui.so|0x31b64|0xea0b0103|0x92405d6b"
"libui.so|0x31b68|0x54000120|0xf900100b"
"libui.so|0x31b6c|0x90ffff61|0x14000008"
)

die() { echo "ERROR: $*" >&2; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

verify_only=0
[ "${1:-}" = "--verify" ] && verify_only=1

for spec in "${SPECS[@]}"; do
    IFS='|' read -r img inner out _ want_patched <<< "$spec"
    target="$OUT_DIR/$out"
    if [ "$verify_only" = 1 ]; then
        [ -f "$target" ] || die "missing $target"
        [ "$(sha "$target")" = "$want_patched" ] || die "$out does not match its pin"
        echo "ok   $out"
    fi
done
[ "$verify_only" = 1 ] && { echo "both patches match their pins"; exit 0; }

command -v debugfs >/dev/null || die "debugfs not found (apt install e2fsprogs)"
mkdir -p "$OUT_DIR"

for spec in "${SPECS[@]}"; do
    IFS='|' read -r img inner out want_orig want_patched <<< "$spec"
    [ -f "$BLOB_DIR/$img" ] || die "no $BLOB_DIR/$img — run waydroid-images.sh fetch first"
    target="$OUT_DIR/$out"

    if [ -f "$target" ] && [ "$(sha "$target")" = "$want_patched" ]; then
        echo "ok   $out (already patched)"
        continue
    fi

    echo "extracting $inner from $img ..."
    rm -f "$target"
    debugfs -R "dump $inner $target" "$BLOB_DIR/$img" 2>/dev/null
    [ -s "$target" ] || die "debugfs could not extract $inner from $img"
    got="$(sha "$target")"
    [ "$got" = "$want_orig" ] || die "$out: extracted copy is not the pinned one
  expected $want_orig
  got      $got
The image changed — re-derive the patch (see the header of this script)."

    for p in "${PATCHES[@]}"; do
        IFS='|' read -r pout off oldw neww <<< "$p"
        [ "$pout" = "$out" ] || continue
        python3 - "$target" "$off" "$oldw" "$neww" <<'PY'
import struct, sys
path, off, oldw, neww = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), int(sys.argv[4], 16)
d = bytearray(open(path, 'rb').read())
cur = struct.unpack_from('<I', d, off)[0]
if cur != oldw:
    raise SystemExit(f'{path}: at {off:#x} expected {oldw:#010x}, found {cur:#010x}')
struct.pack_into('<I', d, off, neww)
open(path, 'wb').write(d)
print(f'  {off:#x}: {oldw:#010x} -> {neww:#010x}')
PY
    done

    got="$(sha "$target")"
    [ "$got" = "$want_patched" ] || die "$out: patched result does not match its pin
  expected $want_patched
  got      $got"
    # bound over an executable inside the container
    [ "$out" = "composer-service" ] && chmod 755 "$target"
    echo "ok   $out"
done

echo "patches regenerated in $OUT_DIR"
