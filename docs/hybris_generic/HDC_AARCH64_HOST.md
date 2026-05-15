# Running `hdc` on an aarch64 Linux host (Pi, etc.)

The stock OHOS SDK only ships `hdc` for x86_64 Linux, Windows, and macOS.
For an aarch64 Linux host (e.g. a Raspberry Pi acting as the USB test
rig), we build hdc as an aarch64 device-side target — which gives us a
musl-linked aarch64 binary — and run it on the Pi by providing the musl
loader plus the OHOS shared libraries it links against.

## Build

```
sudo docker exec -u root -w /home/openharmony/workdir <container> \
    ./build.sh --product-name hybris_generic --ccache \
    --build-target hdc --fast-rebuild
```

Output: `out/hybris_generic/developtools/hdc/hdc`
(ELF aarch64, dynamically linked, interpreter `/lib/ld-musl-aarch64.so.1`).

## Bundle for the Pi

The binary depends on ~10 OHOS shared libraries (transitively ~3000 .so
files, ~1.1 GB).  Easiest is to copy them all out of the build tree, plus
the musl loader:

```
HOST=frankpi    # ssh alias for the Pi

# Copy hdc binary
ssh "$HOST" 'sudo mkdir -p /opt/hdc/lib && sudo chown -R $(id -u):$(id -g) /opt/hdc'
scp out/hybris_generic/developtools/hdc/hdc "$HOST:/opt/hdc/"

# Copy musl loader to its expected path
scp out/hybris_generic/common/common/libc/ld-musl-aarch64.so.1 \
    "$HOST:/tmp/ld-musl-aarch64.so.1"
ssh "$HOST" 'sudo install -m 0755 /tmp/ld-musl-aarch64.so.1 /lib/ld-musl-aarch64.so.1'

# Bulk-copy every aarch64-target .so out of the build tree
( cd out/hybris_generic && find . -name '*.so' \
    -not -path '*/lib.unstripped/*' \
    -not -path '*/innerkits/*' \
    -not -path '*/exe.unstripped/*' \
    -not -path '*/clang_x64/*' ) \
    | while read f; do
        name=$(basename "$f")
        [ -f "lib_bundle/$name" ] && continue
        mkdir -p lib_bundle
        cp "out/hybris_generic/$f" "lib_bundle/$name"
    done

# IMPORTANT: pick the c_utils libutils.z.so (not ets_utils — same filename,
# different symbols).  hdc needs OHOS::SplitStr, LoadStringFromFile, RefBase.
cp out/hybris_generic/commonlibrary/c_utils/libutils.z.so lib_bundle/libutils.z.so

rsync -a lib_bundle/ "$HOST:/opt/hdc/lib/"
```

## Wrapper script + UDS dir

```
ssh "$HOST" 'sudo tee /usr/local/bin/hdc > /dev/null' << 'WRAPEOF'
#!/bin/bash
# Stale-pidfile cleanup: server crashed-but-not-cleaned will fool the
# client into thinking it's still up.
for pidfile in /root/.HDCServer.pid /home/frankpi/.HDCServer.pid; do
    [ -f "$pidfile" ] || continue
    pid=$(cat "$pidfile" 2>/dev/null)
    if [ -n "$pid" ] && [ ! -d "/proc/$pid" ]; then
        rm -f "$pidfile"
    fi
done
# Filter libhilog noise (OHOS hilog daemon not on Pi)
exec env LD_LIBRARY_PATH=/opt/hdc/lib:$LD_LIBRARY_PATH /opt/hdc/hdc "$@" \
    2> >(grep -vE "HiLogAdapter_init|HiLogBase: Can.t connect" >&2)
WRAPEOF
ssh "$HOST" 'sudo chmod +x /usr/local/bin/hdc'

# hdc server bind()s its UDS socket inside /data/hdc/hdc_debug/.  This is
# an OHOS-flavoured path the upstream code hardcodes; create it on Pi.
ssh "$HOST" 'sudo mkdir -p /data/hdc/hdc_debug && sudo chmod 777 /data/hdc/hdc_debug'
```

## Use

```
ssh "$HOST" 'sudo hdc list targets'
# 0a20230726

ssh "$HOST" 'sudo hdc shell "uname -a"'
# Linux localhost 5.10.209-ga4ec076d798b ... aarch64 Toybox
```

`sudo` is required to access `/dev/bus/usb/001/<n>` and write the UDS
socket / PID file in `/root/`.

## Gotchas

- **`libutils.z.so` clash.** The OHOS source tree has two libs with this
  filename (one from `commonlibrary/c_utils`, one from
  `commonlibrary/ets_utils`).  A bulk-copy by find can pick the wrong
  one; hdc then fails at runtime with `Error relocating ... OHOS::SplitStr
  symbol not found`.  Explicitly copy `c_utils/libutils.z.so` last so it
  wins.
- **`HiLogAdapter_init: Can't connect to server. Errno: 2`.**  libhilog
  is statically wired to talk to OHOS's hilog daemon socket
  (`/dev/socket/hilogInput`), which doesn't exist on the Pi.  Errno 2 is
  ENOENT.  The error is harmless — logging just goes nowhere — and the
  wrapper greps it out of stderr.
- **Stale PID file on server crash.**  If the hdc server is killed
  before it can clean up `/root/.HDCServer.pid`, the next client
  invocation thinks the server is running and hangs in `ConnectUds`
  retries.  The wrapper validates and removes stale PID files.
- **`/tmp` is tmpfs.** Do **not** install into `/tmp` on the Pi — Debian's
  default mounts /tmp as tmpfs, so everything is wiped on reboot.
  `/opt/hdc/` is on root fs and persists.
