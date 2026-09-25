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
#   screencast.sh --no-push          # reuse the agent already on the device
#                                    # (no NDK needed; builds + pushes only
#                                    # if it is missing)
#   screencast.sh --stop
#   screencast.sh --pack DIR         # write DIR/oniro-screencast.tar.gz: this
#                                    # script + a prebuilt agent, usable
#                                    # without the tree or the NDK
#
# A screencast_agent.so next to this script is used as-is (that is how the
# packed archive works); otherwise the agent is built from the tree.
#
# ~30 fps on the Plinius at any scale (bound by the device screenshot, not
# the link).  The URL also takes ?scale=0.1..0.9 per viewer.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$HERE/../../../../../.." 2>/dev/null && pwd)"
SRC="$HERE/../device/screencast/screencast_agent.cpp"
API_INC="$TREE/test/testfwk/arkxtest/uitest/addon"
LIB=screencast_agent.so
DEV_DIR=/data/local/tmp

REMOTE=""
PORT=9000
SCALE=0.5
WIFI=0
STOP=0
NO_PUSH=0
PACK=""
while [ $# -gt 0 ]; do
    case "$1" in
        --remote) REMOTE="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --scale) SCALE="$2"; shift 2 ;;
        --wifi) WIFI=1; shift ;;
        --stop) STOP=1; shift ;;
        --no-push) NO_PUSH=1; shift ;;
        --pack) PACK="$2"; shift 2 ;;
        -h|--help) sed -n 4,32p "$0"; exit 0 ;;
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

# Sets AGENT to the library to push: the prebuilt next to this script if
# there is one, else a build from the tree (only when the source is newer
# than the cached library).
build_agent() {
    if [ -z "$PACK" ] && [ -f "$HERE/$LIB" ]; then
        AGENT="$HERE/$LIB"
        return
    fi
    [ -f "$SRC" ] || { echo "no prebuilt $LIB next to this script and no source tree" >&2; exit 1; }
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
    AGENT="$CACHE/$LIB"
}

if [ -n "$PACK" ]; then
    build_agent
    STAGE="$(mktemp -d)"
    trap 'rm -rf "$STAGE"' EXIT
    mkdir "$STAGE/oniro-screencast"
    cp "$0" "$STAGE/oniro-screencast/screencast.sh"
    cp "$AGENT" "$STAGE/oniro-screencast/$LIB"
    cat > "$STAGE/oniro-screencast/README.txt" <<README
Oniro screencast — mirror an OpenHarmony phone's screen into a browser.

Needs: bash, hdc on PATH (from the OpenHarmony SDK/command-line tools), the
phone connected (hdc list targets shows it).  No build tools needed.

  ./screencast.sh               start; open http://127.0.0.1:9000/
                                (double-click the page for fullscreen)
  ./screencast.sh --scale 0.7   sharper picture (default 0.5)
  ./screencast.sh --wifi        serve on the phone's WiFi address instead
                                (anyone on that network can watch)
  ./screencast.sh --stop        stop
  ./screencast.sh --help        all options

The agent runs inside the phone's uitest daemon; its log is
/data/local/tmp/screencast.log on the device.
Packed $(date +%Y-%m-%d) from $(git -C "$HERE" describe --always --dirty 2>/dev/null || echo unknown).
README
    mkdir -p "$PACK"
    tar -C "$STAGE" -czf "$PACK/oniro-screencast.tar.gz" oniro-screencast
    echo "wrote $PACK/oniro-screencast.tar.gz"
    exit 0
fi

# 1. Stop any running agent first: it has the old library mapped.
stop_agent

# 2. Build + push, unless --no-push and the device already has the agent.
if [ "$NO_PUSH" = 1 ] && hdc shell "[ -f $DEV_DIR/$LIB ] && echo present" | grep -q present; then
    echo "using the agent already on the device ($DEV_DIR/$LIB)"
else
    [ "$NO_PUSH" = 1 ] && echo "no agent on the device yet; building and pushing it"
    build_agent
    hdc file send "$AGENT" "$DEV_DIR/$LIB" >/dev/null
fi

# 3. Start the agent.
hdc shell "chmod 755 $DEV_DIR/$LIB && uitest start-daemon singleness --extension-name $LIB $PORT $SCALE $([ "$WIFI" = 1 ] && echo wifi)" >/dev/null
for _ in 1 2 3 4 5; do
    hdc shell "netstat -tln" 2>/dev/null | grep -q ":$PORT " && break
    sleep 1
done
hdc shell "netstat -tln" | grep -q ":$PORT " ||
    { echo "agent not listening; see $DEV_DIR/screencast.log on the device" >&2; exit 1; }

# 4. Reach it.
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
# (bash /dev/tcp probe instead of ss: also works on macOS)
if [ -n "$REMOTE" ] && ! (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
    ssh -f -N -L $PORT:127.0.0.1:$PORT "$REMOTE"
fi
echo "open  http://127.0.0.1:$PORT/     (double-click for fullscreen; stop: $0 --stop)"
