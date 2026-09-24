#!/usr/bin/env bash
# Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
# SPDX-License-Identifier: Apache-2.0
#
# screencast.sh — mirror the device screen into a browser on this machine
# (live demos on a projector).  Builds the uitest extension
# utils/device/screencast/screencast_agent.cpp with the SDK NDK, pushes it,
# starts it under `uitest start-daemon singleness`, and forwards its HTTP
# port.  Open the printed URL; double-click the page for fullscreen.
#
# Usage:
#   screencast.sh                    # phone on this host's USB
#   screencast.sh --remote frankpi   # hdc server on the relay box
#   screencast.sh --scale 0.7        # sharper (default 0.5 = 540x1200)
#   screencast.sh --wifi             # no fport: serve on the phone's WiFi
#                                    # address (anyone on that network can
#                                    # watch; default is USB/loopback only)
#   screencast.sh --stop
#
# ~30 fps on the Plinius at any scale (bound by the device screenshot, not
# the link).  The URL also takes ?scale=0.1..0.9 per viewer.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$HERE/../../../../../.." && pwd)"
SRC="$HERE/../device/screencast/screencast_agent.cpp"
API_INC="$TREE/test/testfwk/arkxtest/uitest/addon"
LIB=screencast_agent.so
DEV_DIR=/data/local/tmp

REMOTE=""
PORT=9000
SCALE=0.5
WIFI=0
STOP=0
while [ $# -gt 0 ]; do
    case "$1" in
        --remote) REMOTE="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --scale) SCALE="$2"; shift 2 ;;
        --wifi) WIFI=1; shift ;;
        --stop) STOP=1; shift ;;
        -h|--help) sed -n 4,22p "$0"; exit 0 ;;
        *) echo "unknown arg $1" >&2; exit 2 ;;
    esac
done

stop_agent() {
    hdc shell "pkill -f 'singleness --extension-name $LIB'" >/dev/null 2>&1 || true
}

if [ "$STOP" = 1 ]; then
    stop_agent
    hdc fport rm tcp:$PORT tcp:$PORT >/dev/null 2>&1 || true
    pkill -f "ssh -f -N -L $PORT:127.0.0.1:$PORT" 2>/dev/null || true
    echo "screencast stopped"
    exit 0
fi

# 1. Build (only when the source is newer than the cached library).
if [ -z "$OHOS_NDK" ]; then
    for d in "$HOME"/setup-ohos-sdk/linux/*/native "$HOME"/command-line-tools/sdk/default/openharmony/native; do
        [ -x "$d/llvm/bin/aarch64-unknown-linux-ohos-clang++" ] && OHOS_NDK="$d"
    done
fi
[ -n "$OHOS_NDK" ] || { echo "no OpenHarmony NDK found; set OHOS_NDK=<sdk>/native" >&2; exit 1; }
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/oniro-screencast"
mkdir -p "$CACHE"
if [ ! -f "$CACHE/$LIB" ] || [ "$SRC" -nt "$CACHE/$LIB" ]; then
    "$OHOS_NDK/llvm/bin/aarch64-unknown-linux-ohos-clang++" --sysroot="$OHOS_NDK/sysroot" \
        -std=c++17 -O2 -Wall -fPIC -shared -fno-exceptions -fno-rtti -nostdlib++ \
        -I"$API_INC" -o "$CACHE/$LIB" "$SRC"
fi

# 2. Push and (re)start the agent.
stop_agent # before the push: the running daemon has the old library mapped
hdc file send "$CACHE/$LIB" "$DEV_DIR/$LIB" >/dev/null
hdc shell "chmod 755 $DEV_DIR/$LIB && uitest start-daemon singleness --extension-name $LIB $PORT $SCALE $([ "$WIFI" = 1 ] && echo wifi)" >/dev/null
for _ in 1 2 3 4 5; do
    hdc shell "netstat -tln" 2>/dev/null | grep -q ":$PORT " && break
    sleep 1
done
hdc shell "netstat -tln" | grep -q ":$PORT " ||
    { echo "agent not listening; see $DEV_DIR/screencast.log on the device" >&2; exit 1; }

# 3. Reach it.
if [ "$WIFI" = 1 ]; then
    IP=$(hdc shell "ifconfig wlan0" | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p' | tr -d '\r')
    [ -n "$IP" ] || { echo "phone has no WiFi address" >&2; exit 1; }
    echo "open  http://$IP:$PORT/"
    exit 0
fi
# hdc fport listens on the machine running the hdc SERVER (the relay box
# when --remote); tunnel it here in that case.
hdc fport tcp:$PORT tcp:$PORT 2>/dev/null | grep -q OK ||
    hdc fport ls | grep -q "tcp:$PORT" ||
    { echo "hdc fport failed — is the device connected?" >&2; exit 1; }
if [ -n "$REMOTE" ] && ! ss -tln | grep -q "127.0.0.1:$PORT "; then
    ssh -f -N -L $PORT:127.0.0.1:$PORT "$REMOTE"
fi
echo "open  http://127.0.0.1:$PORT/     (double-click for fullscreen; stop: $0 --stop)"
