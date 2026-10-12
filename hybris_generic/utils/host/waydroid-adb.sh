#!/usr/bin/env bash
# waydroid-adb.sh — wire workstation adb (and scrcpy) to the Waydroid
# container on the device (device/soc/oniro/hybris_generic/waydroid/README.md).
#
# Chain: adb → [ssh -L] → Pi hdc fport → device tcprelay (127.0.0.1:15555,
# spawned by waydroidd) → veth → container adbd (192.168.240.2:5555).
#
# Usage:
#   waydroid-adb.sh                 # phone on this host's USB
#   waydroid-adb.sh --remote usb-host  # phone on the relay box (hdc server there)
#
# Then:  adb -s $TARGET shell / install ...;  scrcpy -s $TARGET
# (scrcpy needs a desktop session; from a headless workstation run this
# script + scrcpy on your desk machine instead — only ssh + adb needed.)

set -e

REMOTE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --remote) REMOTE="$2"; shift 2 ;;
        *) echo "unknown arg $1" >&2; exit 2 ;;
    esac
done

DEV_PORT=15555   # tcprelay listener on the device (see waydroidd.c)
LOCAL_PORT=15556 # port adb connects to on this machine

# 1. hdc fport: listens on the machine running the hdc SERVER (the Pi
#    when --remote), forwarding to device-loopback where tcprelay sits.
hdc fport tcp:$DEV_PORT tcp:$DEV_PORT 2>/dev/null | grep -q OK ||
    hdc fport ls | grep -q "tcp:$DEV_PORT" ||
    { echo "hdc fport failed — is the device connected?" >&2; exit 1; }

# 2. If the hdc server is remote, tunnel its listener here.
if [ -n "$REMOTE" ]; then
    if ! pgrep -f "ssh.*-L $LOCAL_PORT:127.0.0.1:$DEV_PORT" >/dev/null; then
        ssh -f -N -L $LOCAL_PORT:127.0.0.1:$DEV_PORT "$REMOTE"
    fi
    TARGET=127.0.0.1:$LOCAL_PORT
else
    TARGET=127.0.0.1:$DEV_PORT
fi

# 3. Connect (first attempt through a cold fport can time out — retry).
for _ in 1 2 3; do
    adb connect "$TARGET" >/dev/null
    sleep 2
    if [ "$(adb -s "$TARGET" get-state 2>/dev/null)" = "device" ]; then
        echo "connected: $TARGET"
        adb -s "$TARGET" shell getprop ro.build.version.release |
            sed 's/^/  Android /'
        echo "try:  adb -s $TARGET shell   |   scrcpy -s $TARGET"
        exit 0
    fi
    adb disconnect "$TARGET" >/dev/null 2>&1 || true
done
echo "could not reach container adbd — is waydroidd running and booted?" >&2
exit 1
