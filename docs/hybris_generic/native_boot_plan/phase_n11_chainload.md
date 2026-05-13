# Phase N11 — Chain-Load OHOS via Halium Ramdisk

**Status:** ✅ **DONE (2026-05-11)** — boots reliably to OHOS userspace.
USB hdc, see [`phase_n7_hdc_usb.md`](phase_n7_hdc_usb.md).
Reproduction recipe: [`README.md`](README.md).

After Phase N10's direct-boot.img-replacement attempt looped in the
bootloader, we reframed: instead of replacing Halium's boot.img wholesale,
**reuse Halium's ramdisk** (which already has the dm-mapper tools we need)
and only swap out `/init` to chain-load OHOS.

---

## The shape that worked

OHOS `init_early` can't load Halium-style dynamic partitions on its own.
The X23's `super` partition contains `system_a` and `vendor_a` as logical
extents inside a 9.66 GB block partition; `/dev/mapper/system_a` is
created at boot by Halium's first-stage init using
`/sbin/parse-android-dynparts` (parses LP metadata, emits `dmsetup create`
commands) — a script and tooling that already exist in the Halium boot.img
ramdisk.

Replace `/init` with a shell script that:
1. Mounts /proc /sys /dev.
2. `modprobe -a` the vendor kernel modules (block subsystem comes up only
   after these load).
3. Runs `parse-android-dynparts /dev/disk/by-partlabel/super | sh` →
   `/dev/mapper/system_a` and `/dev/mapper/vendor_a` appear.
4. Mounts them at `/root` and `/root/vendor`.
5. Pre-populates `/root/dev/disk/by-partlabel/*` symlinks so OHOS init
   doesn't block on ueventd.
6. `mount -o bind /proc /sys /dev` → `/root/{proc,sys,dev}`.
7. `exec env OHOS_NATIVE_BOOT=1 chroot /root /system/bin/init --second-stage`.

The kernel + boot.img header are byte-identical to live boot_a, so
vbmeta_a's chain-of-trust stays valid.

---

## Source artifacts (final)

| Path | Purpose |
|---|---|
| `device/board/oniro/hybris_generic/launcher/init-chainload.sh` | The chain-load `/init` (~150 LOC). |
| `device/board/oniro/hybris_generic/kernel/x23/build_boot_img_chainload.sh` | Builder.  Inputs: `out/hybris_generic/backups/boot_a.bak` + the init script.  Output: `out/hybris_generic/boot-chainload.img`. |
| `device/board/oniro/hybris_generic/kernel/x23/build_super_img.sh` | Builds `out/hybris_generic/super.img` (system_a + vendor_a in LP format) using `lpmake`. |
| `device/board/oniro/hybris_generic/utils/host/flash-native.sh` | Host-side: flash super (via fastbootd) + flash chainload (via LK fastboot). |
| `base/startup/init/services/init/standard/init_cmds.c` | `DoMkSandbox` skip under `OHOS_NATIVE_BOOT=1` — see below. |

---

## Hard-won lessons

### `DoMkSandbox` corrupts init's `fs_struct` in a chrooted-but-unshared NS

The init.cfg `init` trigger's first commands are `mksandbox system` and
`mksandbox chipset`.  `DoMkSandbox` does:

```c
unshare(CLONE_NEWNS);       // create new mount namespace
chdir(rootPath);            // CWD into sandbox path
pivot_root(rootPath, …);    // sandbox path becomes new root
umount2(".", MNT_DETACH);   // detach old root
setns(orig_ns_fd, …);       // back to caller's mount namespace
```

`setns()` returns init to the original mount namespace, but init's
`fs_struct->{root, pwd}` still references the (now-detached) sandbox
mount tree from NS1.  When init forks, the child inherits this dangling
CWD/root.  When the child execs, the kernel's path resolution from the
dangling CWD's mount confuses things and the exec fails silently.

Inside a normal native boot (init runs as PID 1 with `/` being the real
rootfs), the detach-and-re-enter works because pivot_root inside the new
NS doesn't touch the global mount tree.  In our chainload, init runs
**chrooted** into `/root`; the same sequence makes the chroot's `/` part
of the detached tree, breaking forks.

**Fix:** the chainload sets `OHOS_NATIVE_BOOT=1` in the env it execs
init with.  `DoMkSandbox` checks `getenv("OHOS_NATIVE_BOOT")` and returns
early.  Skipping sandbox isolation is acceptable for the chainload bring-up
path — sandboxing is a defence-in-depth, not a correctness boundary, in
this setup.

Diagnosis path that found this (preserved as design notes; markers
themselves no longer in the consolidated tree): instrumented
`DoTriggerExecute_` and `DoExec` with vendor_boot_a slot-marker writes.
Saw all triggers fire and forks complete via execve, but the children
couldn't `open("/dev/...")` for any I/O.  In-process commands (`mkdir`,
`write` in init.cfg) still worked because they operate on init's own
`task->fs`.  That fingerprint pointed to a per-process `fs_struct` issue,
which led to the mksandbox sequence.

### MUSB `cmode=3` (DEVICE), not `cmode=2` (HOST)

See [`phase_n7_hdc_usb.md`](phase_n7_hdc_usb.md).

### `mount -o bind`, not `mount -o move`

`mount -o move /dev /root/dev` returns success but the destination is
silently inaccessible from the chrooted child on this kernel.
`mount -o bind` works.  Verified empirically with a tiny static-linked
init that loops opening `/dev/sdc30` post-chroot — `move` fails 100%,
`bind` succeeds 100%.

### Bind-mount-over-RO-file iteration trick

For tight iteration on cfgs without rebuilding super: `mount -o bind
$new_file $path_inside_system_a` overrides a single file on the
read-only `system_a` mount in the running namespace.  Used heavily
during bring-up to test cfg changes without a 40-min rebuild +
re-flash cycle.  Not needed in the consolidated flow (everything's
baked into the image), but worth remembering for the next
regression-hunt.

### OHOS init's `mkdir /dir` cmd silently fails on RO `/`

`init.x23.cfg` pre-init lines like `mkdir /android` are no-ops under
native because the chainload's chrooted `/` is RO `system_a`.  Bake
the dirs into `system_a` via a brief remount-rw in the chainload's
Stage 3a (the cleanest place — `system_a` is still "ours" at that
point).  This is what currently provisions `/android/{system,vendor}`
mount-points.

### kmod's `modprobe` needs module *names*, not full paths

The Halium initramfs's modprobe is kmod (via the `/sbin/modprobe → /bin/kmod`
symlink).  kmod's dep resolution only kicks in when you pass a module
*name* and it can read `modules.dep`.  Passing a full `.ko` path skips
that — kmod loads only the named file, none of its dependencies — so
e.g. `ufs-mediatek-mod` loads but its deps `aee_aed`, `mrdump`,
`hardware_info`, `blocktag`, `rpmb`, `ufs-mediatek-dbg` don't, and the
block subsystem never comes up.  The chainload reads
`/lib/modules/modules.load` (an ordered list maintained by Halium) and
modprobes by name — same pattern as Halium's own
`scripts/halium:load_kernel_modules`.

### `modules.dep` is at `/lib/modules/modules.dep`, but kmod looks under `/lib/modules/$(uname -r)`

So we create a self-symlink first: `ln -sf /lib/modules /lib/modules/$(uname -r)`.
Now `/lib/modules/$(uname -r)/modules.dep` resolves to `/lib/modules/modules.dep`.

### `mountpoint -q` is unreliable in this initramfs

Busybox `mountpoint` misclassifies bind/move mounts and even normal
mounts inside the chainload's namespace, returning non-zero for paths
that `cat /proc/mounts` clearly shows as mounted.  Trust the `mount`
return code (`||`-test) instead.

### lpmake needs `--sparse` for `--image` to take effect

Without `--sparse`, lpmake's `--image=part=file.img` is silently
ignored — the resulting super.img has the LP metadata but the
partition data regions are zero-filled.  Symptom: parse-android-dynparts
on the device generates correct dmsetup tables, /dev/mapper/system_a
and /dev/mapper/vendor_a appear as block devices with correct sizes,
but reads return zero (or whatever was there before the flash).
`build_super_img.sh` uses `--sparse`.

### `/tmp` doesn't exist in the Halium initramfs

The script needs `/tmp` for `parse-android-dynparts > /tmp/dyn.sh`.
mkdir it explicitly alongside `/dev`/`/sys`/`/proc`/`/root` at the top.

### `/bin/init` doesn't exist in OHOS

The Halium ramdisk's `switch_root` invocation in some examples uses
`/bin/init`; OHOS has only `/init` (symlink → `/system/bin/init`) and
`/system/bin/init`.  Use `/system/bin/init` directly in `exec chroot`.

### `busybox switch_root` is fragile; `exec chroot` works

`switch_root` deletes the old root and gets fussy about what's still
mounted.  `exec chroot /root /system/bin/init --second-stage` preserves
PID 1 (kernel keeps PID 1 across exec) and just changes the path anchor.

### Kernel only decompresses LZ4 *legacy* frames

The Volla X23's Halium kernel uses the LZ4 legacy frame format (LZ4
v0.1–0.9, magic `02 21 4c 18`).  Modern `lz4` (≥1.4) defaults to a
different frame format which the kernel **silently** fails to decompress —
the device stays at the LK splash forever with no error.

`build_boot_img_chainload.sh` uses `lz4 -9 -l -c` to force legacy frames.

### `os_version` / `os_patch_level` should match the live header

Whether MTK LK validates these against vbmeta is unclear, but matching
the originals (`12.0.0` / `2025-09`) is safe.  Builder passes both.

### vendor_boot ramdisks contain TWO concatenated LZ4 frames

Frame 1 is the main rootfs; frame 2 is `/scripts/halium` + the recovery
ramdisk.  `cpio -idm` reads only one archive (stops at first
`TRAILER!!!`), so the builder splits the LZ4 stream at every legacy frame
magic and extracts each frame's cpio into the same staging dir.

### Halium boot.img has fastbootd; our chainload doesn't

To flash `super` (which requires fastbootd's dynamic-partition support):
flash a Halium boot.img first → `fastboot reboot fastboot` → flash super
→ `fastboot reboot bootloader` → flash chainload.  Encoded in
`flash-native.sh`.

### force `root:root` ownership in the repacked cpio

cpio extraction by a non-root user (us, on the build host) leaves files
owned by the build user.  The kernel initramfs extractor preserves the
recorded UID/GID; if `/init` isn't root-owned, suid helpers like
`/bin/mount` (which we need for devtmpfs) lose privilege and silently
fail.  GNU cpio's `--owner=+0:+0` rewrites every entry at archive-creation
time.  Builder uses it.

---

## Recovery (if the device gets stuck)

**Device boots Halium splash and stays there.**  The chain-load `/init`
panicked or failed before exec-chroot.  Reflash `boot_a.bak` → `boot_a`
via `fastboot flash boot_a /tmp/boot_a.bak` and the device returns to
Halium normally.  To diagnose, see the marker channel in older revs of
`init-chainload.sh` — it writes 128-byte slot records into vendor_boot_a
which can be fetched back via fastbootd; not enabled in the consolidated
script to keep it clean.

---

## What we tried that didn't work

- **`mtkclient`** for partition writes was unreliable on this device.  Its
  BROM (`0e8d:0003`) cycles every ~6–9 s instead of waiting indefinitely;
  mtkclient races and frequently ends up with stale Download-Agent state
  that needs a 15-s power-button hold to clear.  **Use fastboot for
  writes**, mtkclient only for unbricking.
- **`mtkclient/Library/DA/xmlflash/xml_lib.py::writeflash` reads the whole
  partition into RAM.**  9.66 GB super.img OOM'd the Pi's 7.6 GB.  Patched
  to a `FileBytesView` shim that does file `read()` on slice access; RSS
  stayed under 400 MB.  Upstreamable to bkerler/mtkclient if anyone
  wants to PR.
- **A custom C dm-loader (`init_dm.c`)** for first-stage dynamic-partition
  setup, replaced by Halium's `parse-android-dynparts`.  Removed from
  the consolidated tree.
- **Direct boot.img replacement (Phase N1)** — LK rejected the OHOS-only
  boot.img (failed at signature / header check).  Reusing Halium kernel
  bytes-as-is fixed this.
