#!/usr/bin/env bash
# waydroid-deploy.sh — provision /data/waydroid on a flashed device, from a
# freshly synced tree, with no manual steps.
#
# The image half of the Waydroid stack (compositor, session SA, supervisor,
# wayland/libffi, the init securebits patch) ships in system.img and needs only
# a normal build + flash.  Everything else lives under /data and is what this
# script puts there:
#
#   waydroidd, bin/tcprelay          built here with the OHOS SDK clang
#   images/{system,vendor}.img       sha256-pinned OTA images (waydroid-images.sh)
#   patches/{composer-service,libui.so}  regenerated (waydroid-patches.sh)
#   waydroid.prop, hosthals.xml      from utils/host/waydroid-data/
#   graft/                           gralloc bridge + net scripts + host AIDL libs
#   data.img                         created by waydroidd itself on first start
#
# Usage:
#   waydroid-deploy.sh              # everything (first run: ~7 min, 2 GB push)
#   waydroid-deploy.sh --no-images  # skip the image push (they rarely change)
#   waydroid-deploy.sh --no-verify  # don't wait for the container to boot
#
# Env: HDC (default `hdc`), OHOS_NATIVE_SDK (else ~/setup-ohos-sdk/linux/*/native).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE_ROOT="$(cd "$SCRIPT_DIR/../../../../../.." && pwd)"
BLOB_DIR="$TREE_ROOT/halium-blobs/waydroid"
DATA_DIR="$SCRIPT_DIR/waydroid-data"
SOC_WD="$TREE_ROOT/device/soc/oniro/hybris_generic/waydroid"
LAUNCHER="$TREE_ROOT/device/board/oniro/hybris_generic/launcher"
OUT_DIR="$TREE_ROOT/out/hybris_generic"
DEV=/data/waydroid
HDC="${HDC:-hdc}"

do_images=1; do_verify=1
for a in "$@"; do case "$a" in
    --no-images) do_images=0 ;;
    --no-verify) do_verify=0 ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
esac; done

die() { echo "ERROR: $*" >&2; exit 1; }
step() { echo; echo "==> $*"; }
dsh() { $HDC shell "$@"; }

find_sdk() {
    if [ -n "${OHOS_NATIVE_SDK:-}" ]; then echo "$OHOS_NATIVE_SDK"; return; fi
    for d in "$HOME"/setup-ohos-sdk/linux/*/native /opt/ohos-sdk/*/native; do
        [ -x "$d/llvm/bin/clang" ] && { echo "$d"; return; }
    done
    die "OHOS native SDK not found — set OHOS_NATIVE_SDK"
}

command -v "$HDC" >/dev/null || die "$HDC not found"
$HDC list targets 2>/dev/null | grep -q '[0-9A-Za-z]' || die "no hdc device"
SDK="$(find_sdk)"
CLANG="$SDK/llvm/bin/clang"

step "building waydroidd, tcprelay and the gralloc binder shim ($SDK)"
BUILD_DIR="$(mktemp -d)"; trap 'rm -rf "$BUILD_DIR"' EXIT
"$CLANG" --target=aarch64-linux-ohos --sysroot="$SDK/sysroot" -O2 -Wall -Wextra \
    -Wno-unused-parameter "$LAUNCHER/waydroidd.c" -o "$BUILD_DIR/waydroidd"
"$CLANG" --target=aarch64-linux-ohos --sysroot="$SDK/sysroot" -O2 -Wall -Wextra \
    "$LAUNCHER/tcprelay.c" -o "$BUILD_DIR/tcprelay"
# The shim is loaded by A13 container processes, so it is an Android object.
"$CLANG" --target=aarch64-linux-android -c \
    "$SOC_WD/gralloc_bridge/binderflags_shim.S" -o "$BUILD_DIR/shim.o"
"$SDK/llvm/bin/ld.lld" -shared -soname libbinderflags_shim.so \
    --allow-shlib-undefined -o "$BUILD_DIR/libbinderflags_shim.so" "$BUILD_DIR/shim.o"

step "fetching + verifying the pinned container images"
bash "$SCRIPT_DIR/waydroid-images.sh" fetch >/dev/null
step "regenerating the binary patches from those images"
bash "$SCRIPT_DIR/waydroid-patches.sh"

step "creating the on-device layout"
dsh "mkdir -p $DEV/images $DEV/patches $DEV/bin \
        $DEV/graft/lib $DEV/graft/lib64 $DEV/graft/etc/init $DEV/graft/etc/vintf/manifest" >/dev/null

if [ "$do_images" = 1 ]; then
    step "pushing container images (~2 GB, several minutes)"
    bash "$SCRIPT_DIR/waydroid-images.sh" push >/dev/null
else
    step "pushing prop/hosthals/patches (images skipped)"
    for f in waydroid.prop hosthals.xml; do $HDC file send "$DATA_DIR/$f" "$DEV/$f" >/dev/null; done
    for f in composer-service libui.so; do $HDC file send "$BLOB_DIR/patches/$f" "$DEV/patches/$f" >/dev/null; done
    dsh "chmod 755 $DEV/patches/composer-service" >/dev/null
fi

step "pushing launcher, relay and graft"
$HDC file send "$BUILD_DIR/waydroidd" "$DEV/waydroidd" >/dev/null
$HDC file send "$BUILD_DIR/tcprelay" "$DEV/bin/tcprelay" >/dev/null
$HDC file send "$BUILD_DIR/libbinderflags_shim.so" "$DEV/graft/libbinderflags_shim.so" >/dev/null
$HDC file send "$SOC_WD/gralloc_bridge/start-allocator.sh" "$DEV/graft/start-allocator.sh" >/dev/null
$HDC file send "$SOC_WD/net/waydroid-net.sh" "$DEV/graft/waydroid-net.sh" >/dev/null
$HDC file send "$SOC_WD/gralloc_bridge/waydroid-gralloc.rc" "$DEV/graft/etc/init/waydroid-gralloc.rc" >/dev/null
$HDC file send "$SOC_WD/net/waydroid-net.rc" "$DEV/graft/etc/init/waydroid-net.rc" >/dev/null
# The container's VINTF reads ODM manifest fragments from this exact name.
$HDC file send "$SOC_WD/gralloc_bridge/graphics-allocator-aidl.xml" \
    "$DEV/graft/etc/vintf/manifest/waydroid-allocator-aidl.xml" >/dev/null

step "grafting the host AIDL graphics libs into the container's /odm"
# The A13 image lacks what the A14 Mali blobs need; common-V5 is renamed to the
# V4 soname the container links against (NDK AIDL libs keep their type symbols).
dsh "set -e
chmod 755 $DEV/waydroidd $DEV/bin/tcprelay $DEV/graft/start-allocator.sh $DEV/graft/waydroid-net.sh
for a in lib64 lib; do
  cp /android/system/\$a/android.hardware.graphics.common-V5-ndk.so \
     $DEV/graft/\$a/android.hardware.graphics.common-V4-ndk.so
  cp /android/system/\$a/android.hardware.graphics.allocator-V2-ndk.so $DEV/graft/\$a/
done" >/dev/null || die "graft failed — is the Halium container up (/android/system mounted)?"

step "installing the Android Apps front-end"
HAP=""
for c in "$OUT_DIR/packages/phone/system/app/org.oniroproject.androidapps/AndroidApps.hap" \
         "$OUT_DIR/oniro_soc_products/hybris_generic_soc/AndroidApps.hap"; do
    [ -f "$c" ] && { HAP="$c"; break; }
done
if dsh "bm dump -n org.oniroproject.androidapps" 2>/dev/null | grep -q androidapps; then
    echo "already installed"
elif [ -n "$HAP" ]; then
    $HDC file send "$HAP" /data/local/tmp/AndroidApps.hap >/dev/null
    dsh "bm install -p /data/local/tmp/AndroidApps.hap" | tail -1
else
    # The image ships it in /system/app but does not preinstall it.
    dsh "bm install -p /system/app/org.oniroproject.androidapps/AndroidApps.hap" | tail -1
fi

step "enabling waydroid and restarting the container generation"
dsh "param set persist.waydroid.enabled 1
     CPID=\$(cat $DEV/container.pid 2>/dev/null); [ -n \"\$CPID\" ] && kill -9 \$CPID 2>/dev/null
     killall -9 waydroidd waydroid_compositor 2>/dev/null; true" >/dev/null
echo "supervisor will build a fresh generation within ~10 s"

[ "$do_verify" = 0 ] && { echo; echo "deployed (verification skipped)"; exit 0; }

step "waiting for Android to boot inside the container"
for i in $(seq 1 60); do
    sleep 5
    CPID=$($HDC shell "cat $DEV/container.pid 2>/dev/null" 2>/dev/null | tr -dc 0-9)
    [ -z "$CPID" ] && continue
    $HDC shell "timeout 8 nsenter -t $CPID -m -p -- /system/bin/getprop sys.boot_completed \
                > /data/local/tmp/wd_bc 2>&1; cat /data/local/tmp/wd_bc" 2>/dev/null |
        grep -q '^1' && { echo "container $CPID: Android booted after ~$((i * 5))s"; break; }
done
$HDC shell "cat $DEV/supervisor.log" 2>/dev/null | tail -4
echo
echo "done.  Open \"Android Apps\" on the device (or: aa start -b org.oniroproject.androidapps -a EntryAbility)"
