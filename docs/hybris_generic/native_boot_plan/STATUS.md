# Native Boot Plan — Execution Status (2026-05-12)

🎉 **Native boot + USB hdc are working end-to-end on Volla X23.**
The device boots OpenHarmony natively (no Ubuntu Touch host, no LXC),
USB enumerates as `12d1:5000 Phone X23`, and `hdc shell` over USB
returns a live shell into the running OHOS.

🚧 **Graphics revival (Phases N4 + N5 + N8) — partial on-device validation
2026-05-12 PM.**  Halium HAL launcher (`androidd`), super-image Halium
blobs (`halium_system_a` from UBports system-image v12 + `halium_vendor_a`
from the bootstrap zip), composer-ready gate cfg, and Bug 8.18 sandbox
chmod port are all in tree and shipped to device.  Validated end-to-end:
  - Halium ext4 partitions mount cleanly at `/android/{system,vendor}` via
    the chainload (Stage 3b).
  - `androidd` runs as an OHOS init service with full caps.
  - `clone(CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS)` succeeds; child sets
    up its `/dev` tmpfs, binder binds, vendor bind, tmpfs `/data`, and
    pivot_root into `/android/system`.
  - `execv("/system/bin/init", ...)` reaches Halium's stage-2 init binary.

  **Still failing**: Halium init then SIGSEGVs immediately at startup
  (confirmed via `chroot /android/system /system/bin/init second_stage`
  from hdc shell — exits with "Signal 11" before producing any stdout/
  stderr).  The composer-ready watchdog therefore times out at 300 s.

  Bugs hit and fixed along the way (durable lessons added to "Hard-won
  lessons"):
  - `nodev` on the binderfs mount silently denies open(2) of
    `/dev/binderfs/binder-control` even for uid 0; removed from
    `init.x23.cfg`.
  - `/data` on native is on RO `system_a` (no userdata partition mounted
    in our fstab) — chainload can't pre-create `/android/data`, and the
    launcher must back `/android/system/data` with a tmpfs.
  - `pivot_root`'s put-old dir must be on a writable filesystem;
    `/android/system` is RO ext4, so put-old → `/data/old_root` (the
    tmpfs we just mounted).
  - Halium's android-rootfs.img is a *full Android root* (`/init`,
    `/bin -> /system/bin`, `/system/bin/init`), so the launcher must
    pivot into `/android/system/`, not `/android/`.
  - The bootstrap zip's super.img has `system_a` allocated but zeroed;
    real Halium system content comes from UBports system-image's
    `device-*.tar.xz` → `android-rootfs.img`.

Reproduction recipe: [`REPRODUCTION.md`](REPRODUCTION.md).
aarch64 Linux host setup (for Pi-as-USB-rig): [`HDC_AARCH64_HOST.md`](HDC_AARCH64_HOST.md).

---

## Final architecture

We arrived at a chain-load design instead of the original direct-replace
flash:

1. `boot_a` gets our `boot-chainload.img` — a Halium boot.img with `/init`
   replaced by `device/board/oniro/hybris_generic/launcher/init-chainload.sh`.
2. `super` gets a fresh LP-formatted image built by `build_super_img.sh`,
   containing only `system_a` + `vendor_a` from the OHOS build output.
3. The chain-load `/init` modprobes the vendor kernel modules, mounts
   `system_a` and `vendor_a` via `parse-android-dynparts` + `dmsetup`,
   and `exec env OHOS_NATIVE_BOOT=1 chroot /root /system/bin/init
   --second-stage`.

This is conceptually Phase N11 of the original plan, which superseded
the direct boot-image replacement (Phase N1) after the latter looped in
the bootloader.

## Phase index (final)

| Phase | Doc | Status |
|---|---|---|
| N0 | `phase_n0_preflight_smoke_test.md` | ✅ historical — risk retired by chainload approach |
| N1 | `phase_n1_boot_image.md` | ❌ Superseded by N11.  `boot-ohos.img` direct flash failed (LK rejected). |
| N2 | `phase_n2_init_native.md` | ✅ One additional fix landed in `init_cmds.c::DoMkSandbox` — skip when `OHOS_NATIVE_BOOT=1`.  Otherwise unchanged. |
| N3 | `phase_n3_fstab.md` | ✅ `fstab.x23` + `init.x23.cfg` deployed via vendor.img.  Chainload handles `/proc /sys /dev` bind, OHOS init handles second-stage mounts. |
| N4 | `phase_n4_androidd.md` | ✅ Source-side complete (2026-05-12 PM). `launcher/androidd.c` (~370 LOC, libc only) does `BINDER_CTL_ADD` for `android-binder`, `clone(NEWPID|NEWNS|NEWUTS)`, builds per-NS `/dev` tree (tmpfs + binder binds + Mali/DRI/DMA-BUF passthrough), `pivot_root` into `/android`, exec `/system/bin/init`. Parent setns'es into Halium NS every 5 s for ≤5 min, runs `lshal --neat | grep IComposer/default`; on match flips OHOS param `android.composer.ready=1` via `/system/bin/param`. |
| N5 | `phase_n5_android_image.md` | ✅ Source-side complete (2026-05-12 PM). **Plan adjusted from original draft**: the bootstrap zip's `system_a` is allocated-but-zero, so we source `halium_system_a.img` (441 MB ext4) from UBports system-image stable v12's `device-<sha>.tar.xz` (the embedded `android-rootfs.img`), and `halium_vendor_a.img` (930 MB ext4) from the bootstrap zip's super. Ships `utils/host/{pull-halium-blobs.sh,lpunpack.py}` (stdlib Python LP extractor, no AOSP dep). Both partitions baked into `super.img` by `build_super_img.sh` when `halium-blobs/` is populated (graphics-disabled builds skip them). Chainload Stage 3b mounts both at `/root/android/{system,vendor}` RO. Stage 3a does the Bug 8.18 sandbox `chmod 0644` via remount-rw → chmod → remount-ro on system_a. |
| N6 | `phase_n6_binder.md` | ✅ Source-side complete. Native uses default `/dev/binder` for OHOS samgr; `android-binder` provisioned by `androidd` via `BINDER_CTL_ADD`; `hwbinder`/`vndbinder` shared. Activates with N4 delivery. |
| **N7** | **`phase_n7_hdc_usb.md`** | ✅ **DONE.**  USB hdc gadget enumerates and `hdc shell` works.  Three fixes:  `cmode=3` (not 2) for MTK musb peripheral mode; `setparam const.security.developermode.state true` in `z_hdcd_autostart.cfg`; aarch64 cross-build of hdc client for the Pi (see HDC_AARCH64_HOST.md). |
| N8 | `phase_n8_graphics_native.md` | ✅ Source-side complete (2026-05-12 PM). `cfg/z_composer_host_gate.cfg` overlays composer_host/allocator_host with `start-mode: condition` + `condition: param:android.composer.ready=1`. Phases 5–8 + 11 inherit unchanged (libhybris's own `/vendor → /android/vendor` path map handles native boot's lib lookups without any source edits). Bug 8.18 sandbox chmod port deferred to the chainload (init.x23.cfg can't chmod `/system` post-boot — it's RO). |
| N9 | `phase_n9_firmware_peripherals.md` | 🔄 Partial.  WiFi/audio/sensors not yet up natively. |
| N10 | `phase_n10_flash_recovery.md` | ✅ `flash-native.sh` updated for the chainload flow (boot_a.bak → fastbootd → super → boot_a chainload). |
| **N11** | **`phase_n11_chainload.md`** | ✅ **DONE.**  Chainload boots reliably.  Two additional fixes vs the original N11 doc: `DoMkSandbox` corrupts init's `fs_struct` in a chrooted-but-unshared NS — patched to skip under `OHOS_NATIVE_BOOT=1`.  `cmode=3` for the USB gadget. |

## Source-tree state (final, post-graphics-revival 2026-05-12)

```
base/startup/init/
└── services/init/standard/init_cmds.c        # DoMkSandbox: skip when OHOS_NATIVE_BOOT=1

device/board/oniro/hybris_generic/
├── BUILD.gn                                  # group includes launcher:androidd_group +
│                                             #   cfg:hybris_composer_gate_group
├── .gitignore                                # excludes halium-blobs/  (NEW 05-12)
├── cfg/
│   ├── z_composer_host_gate.cfg              # NEW 05-12: gates composer_host on
│   │                                             android.composer.ready=1
│   ├── z_hdcd_autostart.cfg                  # setparams + start hdcd
│   └── z_*.cfg                               # input caps, container fixes, hdf env
├── halium-blobs/                             # NOT committed — populated by
│   ├── halium_system_a.img                   #   pull-halium-blobs.sh (~1.5 GB total)
│   └── halium_vendor_a.img
├── kernel/x23/
│   ├── build_boot_img_chainload.sh
│   ├── build_super_img.sh                    # NOW conditionally adds halium_* parts
│   ├── build_kernel.sh
│   └── deploy-kernel.sh
├── launcher/
│   ├── init-chainload.sh                     # +Stage 3a (sandbox chmod) +Stage 3b
│   │                                         #   (halium_{system,vendor}_a bind)
│   ├── androidd.c                            # NEW 05-12: Halium HAL launcher (~370 LOC)
│   ├── androidd.cfg                          # NEW 05-12: init service def (post-fs-data)
│   └── BUILD.gn                              # NEW 05-12
└── utils/host/
    ├── flash-native.sh
    ├── pull-halium-blobs.sh                  # NEW 05-12: fetch Halium ext4 images
    └── lpunpack.py                           # NEW 05-12: stdlib LP super extractor

vendor/oniro/hybris_generic/etc/
├── BUILD.gn
├── fstab/fstab.x23
├── init/
│   ├── init.x23.cfg                          # pre-existing /android/{system,vendor,...}
│   │                                         #   mkdir is what androidd + chainload bind into
│   └── init.x23.usb.cfg
├── param/hybris_native.para
└── ueventd/ueventd.config
```

## Hard-won lessons (durable)

1. **MTK musb `cmode` enum** (kernel `drivers/misc/mediatek/usb20/musb.h`):
   `0=NONE, 1=NORMAL/auto, 2=HOST, 3=DEVICE`.  Setting `cmode=2` for
   "peripheral" leaves the controller in HOST mode and the host PC sees
   nothing on the bus.  Cost: ~weeks.
2. **`hdcd` exits with `developerMode != "true"`**, then init restarts it
   in a loop.  `hybris_native.para` sets the param but lands in
   `/sys_prod/etc/param/`, which the chainload doesn't mount.  Fix:
   `setparam` in `z_hdcd_autostart.cfg` before `start hdcd`.
3. **`DoMkSandbox` corrupts init's `fs_struct`** in a chrooted-but-unshared
   namespace.  `unshare(CLONE_NEWNS) → chdir(rootPath) → pivot_root →
   umount2 MNT_DETACH → setns(orig_ns)` leaves init's CWD inode pointing
   into a detached mount tree.  Every fork+exec from init thereafter
   fails silently (children can't open `/dev` paths).  Fix: skip
   `DoMkSandbox` when `OHOS_NATIVE_BOOT=1`.  Cost: ~days.
4. **`mount -o bind /proc /sys /dev` → chroot**, not `mount -o move`.
   On this kernel `move` silently leaves the destination inaccessible
   from the chrooted child.
5. **Bind-mount-over-RO-file** in `system_a` works for iterating on cfgs
   without rebuilding super.  Used during bring-up; no longer needed in
   the consolidated flow (everything's baked in the image).
6. **Halium boot.img has fastbootd inside; our chainload doesn't.** When
   flashing super, flash a Halium boot.img first to reach fastbootd,
   then flash chainload last.
7. **`nodev` on a binderfs mount silently breaks all device-node opens**,
   even for uid 0 with all caps.  Returns `EACCES` (not the more
   honest `EPERM`/`EACCES` distinction you'd see for a missing cap).
   The kernel binderfs control device is a real char-device (major 1,
   minor variable) and `nodev` blocks it.  Drop `nodev` from the mount
   options for `binderfs` (we keep `noexec,nosuid` — those are fine).
   Cost: hours of "but I am root" diagnosing.
8. **Halium-12 android-rootfs.img is a *full* Android root**, not a
   `/system`-content tar.  Inside it: `/init` symlink → `/system/bin/init`,
   `/bin -> /system/bin` (absolute symlink), real binaries at
   `/system/bin/*`, plus empty `/dev`, `/proc`, `/sys`, `/data`, `/vendor`
   for the runtime to mount things on.  Implications:
   - Mount it at `/android/system/` (we do).  Halium's real `/system`
     content is then at `/android/system/system/`.
   - From OHOS (no pivot), `ls /android/system/bin/init` follows the
     `/bin -> /system/bin` symlink → resolves to **OHOS's**
     `/system/bin/init`, not halium's.  Use `/android/system/system/bin/`
     for pre-flight checks.
   - `androidd` must `pivot_root` into `/android/system/`, not
     `/android/`.  After pivot, `/system/bin/init` resolves correctly.
9. **Halium vendor (`vendor_a` inside the bootstrap super.img) is
   self-contained** and works as `halium_vendor_a`.  But Halium *system*
   (`system_a` inside the same super.img) is **allocated but zeroed** —
   UBports installs the real system content as a file
   (`/var/lib/lxc/android/android-rootfs.img`) into UT's userdata at
   `systemimage:install` time.  We source it from the system-image
   `device-*.tar.xz` instead.
10. **`pivot_root`'s put-old dir needs to be on a writable filesystem,**
    because the kernel materialises a mount-point dentry there.
    `/android/system/old_root` is on the RO halium ext4 → EROFS.  Use
    `/data/old_root` inside the per-NS tmpfs we mount at
    `/android/system/data` for this.
11. **`/data` is RO on native (no userdata partition mounted)** —
    `fstab.x23` only mounts misc + persist, and the kernel cmdline's
    `ohos.required_mount.*` doesn't cover userdata.  Anything that
    needs writable `/data` (Halium init's per-NS data, OHOS services'
    state) has to be backed by a tmpfs or by bringing userdata into
    fstab.  Not a blocker for HAL bring-up; will be one for any
    persistent OHOS app state.
12. **OHOS init's `mkdir /dir` cmd silently fails on RO `/`**, so cfg
    pre-init lines like `mkdir /android` are no-ops under native.
    Bake the dirs into `system_a` via a brief remount-rw in the
    chainload's Stage 3a (the cleanest place — `system_a` is still
    "ours" at that point).

## Open work

**Graphics revival (N4 + N5 + N8) — current blocker (2026-05-12 PM).**

The launcher is wired and reaches `execv("/system/bin/init", ...)` of
Halium's stage-2 init inside the new PID/mount/UTS namespace.  The
**immediate failure** is that Halium init `SIGSEGV`s at startup before
producing any output.  Independent confirmation: from `hdc shell`,
`chroot /android/system /system/bin/init second_stage` exits with
"Signal 11", no stdout/stderr.

Candidates for the SEGV (ordered by likelihood):

1. **Missing `/init.environ.rc`** — Android init parses this very
   early.  The Halium image *has* `/android/system/init.environ.rc`,
   but after our pivot we need that path resolvable.  Verify with
   `ls /android/system/init.environ.rc`.
2. **SELinux policy load** — Even when `androidboot.selinux=permissive`,
   Halium init still tries to load `/sepolicy` (or `/file_contexts.bin`)
   and may segfault if the binary policy isn't compatible with the
   running kernel.  Halium 12's policy was compiled for the Halium
   5.10 kernel — which IS our kernel, so this might actually work.
3. **Bionic linkerconfig** — Halium 12 expects `/linkerconfig/ld.config.txt`.
   The android-rootfs.img has `/linkerconfig/` as an empty dir; the
   contents are normally generated by `linkerconfig` at first boot,
   read by the dynamic linker.  Without it, libraries may not load.
4. **Property service init** — needs `/dev/__properties__` tmpfs (we
   have that) and `/proc/self/exe` resolvable (should be).
5. **`/dev/hwbinder`/`/dev/binder` context register failure** — the
   binder context manager call assumes single-registration; we use
   `android-binder` for Android's `/dev/binder` so this should be
   fine.

**Next-session debugging path** (in order of cost / diagnostic
value):

- **a.** strace-equivalent: build a tiny C wrapper that exec's init
  and uses `ptrace(PTRACE_TRACEME)` to catch the first signal.  Or
  cross-compile `strace` for aarch64-musl and ship it via `hdc file
  send`.  Either gives us the syscall that returned the address that
  caused the segfault.
- **b.** Stripped binary — Halium's init binary IS stripped; we won't
  get useful symbols from a core dump.  Pull `linker64`'s mmap log
  via `LD_DEBUG=all` (bionic env var) to see if early loads work.
- **c.** Walk Halium init's source from AOSP (
  `system/core/init/main.cpp::SecondStageMain`) and check each
  pre-`InitLogging` step — most likely candidates are
  `MountKernelFileSystems` and `LoadKernelModules`.
- **d.** Bind-mount **OHOS's** `init` over Halium's `/system/bin/init`
  as a sanity check that the bionic environment can load anything at
  all (it won't, because OHOS uses musl — but the SEGV signature would
  shift, isolating where the problem is).

**Phase N9 (peripherals beyond graphics).**  WiFi (Phase 10) and audio
(Phase 13B) are native and inherit cleanly — no Android HAL dependency.
Bluetooth and sensors still need their Android HALs running in
`androidd`'s namespace; defer until N4's exec-init SEGV is resolved.

**The original phase docs** (`phase_n*.md`) capture the design
thinking we went through and are kept as historical context.  Where
they describe an approach that was replaced (notably N1; N4/N5/N8 were
rewritten 2026-05-12), treat earlier revisions as design history, not
current state.
