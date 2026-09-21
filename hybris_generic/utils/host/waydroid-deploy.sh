#!/usr/bin/env bash
# waydroid-deploy.sh — provision Android app support on a flashed device.
#
# All code and configuration of the Waydroid stack ships in system.img, built
# when the product sets hybris_generic_soc_feature_waydroid (it does for
# hybris_generic): compositor, waydroidd, supervisor, the patch table, the prop
# and hosthals files, the gralloc graft, the "Android Apps" front-end.  What a
# device still needs is DATA, and that is all this script provides:
#
#   /data/waydroid/images/{system,vendor}.img   the two sha256-pinned upstream
#                                               images, pushed unmodified
#   persist.waydroid.enabled=1                  the runtime switch
#   persist.waydroid.autostart=0|1              start policy (below)
#
# Start policy.  By default the stack is ON DEMAND: nothing runs until "Android
# Apps" is opened (first open ~20 s), and it is torn down again after
# persist.waydroid.idle_stop_s (default 1800) in the background.  --autostart
# brings the container up at boot and keeps it, for an instant first open at
# the price of its RAM.
#
# data.img is created by waydroidd on first start; the binary patches are
# derived by waydroidd on every start; the graft is assembled by the supervisor
# on every start.  Nothing is compiled or patched on this host.
#
# Usage:
#   waydroid-deploy.sh              # everything (first run: ~7 min, 2 GB push)
#   waydroid-deploy.sh --no-images  # images are already on the device
#   waydroid-deploy.sh --no-verify  # don't open the app and wait for Android
#   waydroid-deploy.sh --autostart  # start at boot instead of on demand
#   waydroid-deploy.sh --disable    # switch it off again (keeps the data)
#
# Env: HDC (default `hdc`).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEV=/data/waydroid
HDC="${HDC:-hdc}"

do_images=1; do_verify=1; do_disable=0; autostart=0
for a in "$@"; do case "$a" in
    --no-images) do_images=0 ;;
    --no-verify) do_verify=0 ;;
    --disable) do_disable=1 ;;
    --autostart) autostart=1 ;;
    -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
esac; done

die() { echo "ERROR: $*" >&2; exit 1; }
step() { echo; echo "==> $*"; }
dsh() { $HDC shell "$@"; }

command -v "$HDC" >/dev/null || die "$HDC not found"
$HDC list targets 2>/dev/null | grep -q '[0-9A-Za-z]' || die "no hdc device"

if [ "$do_disable" = 1 ]; then
    step "disabling waydroid"
    dsh "param set persist.waydroid.enabled 0" >/dev/null
    echo "the supervisor tears the container down and stops within ~15 s;"
    echo "images and data.img stay in $DEV (rm -rf it to reclaim the space)"
    exit 0
fi

dsh "[ -x /system/bin/waydroidd ] && [ -f /system/etc/waydroid/patches.conf ] && echo present" |
    grep -q present || die "this image has no Android app support — build the
product with hybris_generic_soc_feature_waydroid = true (vendor config.json) and flash it"

step "fetching + verifying the pinned container images"
bash "$SCRIPT_DIR/waydroid-images.sh" fetch >/dev/null
step "checking patches.conf against them"
bash "$SCRIPT_DIR/waydroid-patches.sh"

if [ "$do_images" = 1 ]; then
    step "pushing container images (~2 GB, several minutes)"
    bash "$SCRIPT_DIR/waydroid-images.sh" push >/dev/null
fi

# Deployments made before the stack moved into the image left root binaries and
# patched blobs here.  Remove them: the prop/hosthals copies would silently
# override the image's, and code does not belong on /data.
step "removing pre-image-era leftovers from $DEV"
dsh "rm -rf $DEV/waydroidd $DEV/patches $DEV/waydroid.prop $DEV/hosthals.xml \
            $DEV/bin/tcprelay $DEV/bin/waydroidd; rmdir $DEV/bin 2>/dev/null; true" >/dev/null

step "installing the Android Apps front-end"
# Preinstalled (removable) on first boot of a fresh /data; on a device whose
# /data predates this image BMS does not rescan, so install it by hand.
# -r: also replaces an older copy — the front-end and the compositor speak a
# protocol (the SHOW heartbeat) and must come from the same image.
dsh "bm install -r -p /system/app/org.oniroproject.androidapps/AndroidApps.hap" | tail -1

step "enabling waydroid ($([ $autostart = 1 ] && echo autostart || echo on demand))"
dsh "param set persist.waydroid.autostart $autostart
     param set persist.waydroid.enabled 1
     CPID=\$(cat $DEV/container.pid 2>/dev/null); [ -n \"\$CPID\" ] && kill -9 \$CPID 2>/dev/null
     killall -9 waydroidd waydroid_compositor 2>/dev/null; true" >/dev/null

[ "$do_verify" = 0 ] && { echo; echo "deployed (verification skipped)"; exit 0; }

step "opening Android Apps and waiting for Android to boot in the container"
# Opening the app is the on-demand trigger (and harmless under --autostart).
dsh "power-shell wakeup >/dev/null 2>&1
     aa start -b org.oniroproject.androidapps -a EntryAbility" | tail -1
booted=0
for i in $(seq 1 36); do
    sleep 5
    CPID=$($HDC shell "cat $DEV/container.pid 2>/dev/null" 2>/dev/null | tr -dc 0-9)
    [ -z "$CPID" ] && continue
    $HDC shell "timeout 8 nsenter -t $CPID -m -p -- /system/bin/getprop sys.boot_completed \
                > /data/local/tmp/wd_bc 2>&1; cat /data/local/tmp/wd_bc" 2>/dev/null |
        grep -q '^1' && { echo "container $CPID: Android booted after ~$((i * 5))s"; booted=1; break; }
done
$HDC shell "cat $DEV/supervisor.log" 2>/dev/null | tail -5
[ $booted = 1 ] || die "Android did not boot — see $DEV/supervisor.log and $DEV/waydroidd.log"
echo
echo "done.  Android Apps is open on the device."
