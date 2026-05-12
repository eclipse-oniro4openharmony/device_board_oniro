# Native Boot Plan — Execution Status (2026-05-11)

🎉 **Native boot + USB hdc are working end-to-end on Volla X23.**
The device boots OpenHarmony natively (no Ubuntu Touch host, no LXC),
USB enumerates as `12d1:5000 Phone X23`, and `hdc shell` over USB
returns a live shell into the running OHOS.

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
| N4 | `phase_n4_androidd.md` | 🔄 **Open (rewritten 2026-05-12).** C launcher to run Halium HAL services (hwservicemanager + composer@2.x + gralloc@4.0) in a child PID/mount/UTS namespace, sharing OHOS's `/dev/hwbinder`. Earlier draft was authored but never built or deployed; restart from scratch under the chainload model. |
| N5 | `phase_n5_android_image.md` | 🔄 **Open (rewritten 2026-05-12).** Source Halium 12 `system_a` + `vendor_a` from the public UBports installer bootstrap zip (`volla-vidofnir-12.0-ubports-installer-bootstrap-v3.zip`); add as `halium_system_a` + `halium_vendor_a` partitions in our `super.img`; mount RO at `/root/android/{system,vendor}` from the chainload. |
| N6 | `phase_n6_binder.md` | ✅ Source-side complete. Native uses default `/dev/binder` for OHOS samgr; `android-binder` provisioned by `androidd` via `BINDER_CTL_ADD`; `hwbinder`/`vndbinder` shared. Activates with N4 delivery. |
| **N7** | **`phase_n7_hdc_usb.md`** | ✅ **DONE.**  USB hdc gadget enumerates and `hdc shell` works.  Three fixes:  `cmode=3` (not 2) for MTK musb peripheral mode; `setparam const.security.developermode.state true` in `z_hdcd_autostart.cfg`; aarch64 cross-build of hdc client for the Pi (see HDC_AARCH64_HOST.md). |
| N8 | `phase_n8_graphics_native.md` | 🔄 **Open (rewritten 2026-05-12).** Wiring + gating only — depends on N4 + N5. Adds `cfg/z_composer_host_gate.cfg` (composer_host/allocator_host wait on `param:android.composer.ready=1`, set by `androidd`'s parent when hwservicemanager registers composer). Phases 5–8 + 11 inherit unchanged. |
| N9 | `phase_n9_firmware_peripherals.md` | 🔄 Partial.  WiFi/audio/sensors not yet up natively. |
| N10 | `phase_n10_flash_recovery.md` | ✅ `flash-native.sh` updated for the chainload flow (boot_a.bak → fastbootd → super → boot_a chainload). |
| **N11** | **`phase_n11_chainload.md`** | ✅ **DONE.**  Chainload boots reliably.  Two additional fixes vs the original N11 doc: `DoMkSandbox` corrupts init's `fs_struct` in a chrooted-but-unshared NS — patched to skip under `OHOS_NATIVE_BOOT=1`.  `cmode=3` for the USB gadget. |

## Source-tree state (final)

```
base/startup/init/
└── services/init/standard/init_cmds.c        # DoMkSandbox: skip when OHOS_NATIVE_BOOT=1

device/board/oniro/hybris_generic/
├── BUILD.gn                                  # (no native-boot specific additions)
├── cfg/z_hdcd_autostart.cfg                  # setparams + start hdcd
├── docs/x23-super.txt                        # partition reference
├── kernel/x23/
│   ├── build_boot_img_chainload.sh           # boot-chainload.img builder
│   ├── build_super_img.sh                    # super.img builder
│   ├── build_kernel.sh                       # (unchanged Halium kernel build)
│   └── deploy-kernel.sh                      # (unchanged)
├── launcher/
│   └── init-chainload.sh                     # the chain-load /init script
└── utils/host/flash-native.sh                # host-side flash automation

vendor/oniro/hybris_generic/etc/
├── BUILD.gn                                  # ohos_prebuilt_etc for the new cfgs
├── fstab/fstab.x23                           # OHOS-side mounts (second-stage)
├── init/
│   ├── init.x23.cfg                          # binderfs, /android dir tree, USB controller
│   └── init.x23.usb.cfg                      # cmode=3 + USB gadget configfs
├── param/hybris_native.para                  # HDC/dev-mode params (loaded only when
│                                             #   /sys_prod is mounted — not by chainload;
│                                             #   z_hdcd_autostart.cfg setparams cover it)
└── ueventd/ueventd.config                    # device-node permissions
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

## Open work

**Graphics revival (N4 + N5 + N8, rewritten 2026-05-12).**  The path
back to a working display under native boot is now a connected
sequence:

1. **N5 — Halium content sourcing.**  Fetch `volla-vidofnir-12.0-ubports-installer-bootstrap-v3.zip` (the public Volla
   UBports bootstrap), `lpunpack` it to extract Halium's `system_a` +
   `vendor_a` ext4 images, stash under `device/board/oniro/hybris_generic/halium-blobs/`.
   Extend `build_super_img.sh` to bake `halium_system_a` +
   `halium_vendor_a` as additional LP partitions in our super.img.
   Extend `init-chainload.sh` Stage 3 to mount them at
   `/root/android/system` and `/root/android/vendor`.
2. **N4 — `androidd` launcher.**  ~250-line C binary at
   `device/board/oniro/hybris_generic/launcher/androidd.c` +
   `androidd.cfg` + BUILD.gn.  `BINDER_CTL_ADD` for `android-binder`,
   `clone(CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS)` (IPC + net inherited
   so hwbinder + WiFi cross), bind `/dev/{mali0,dri,dma_heap}` +
   tmpfs `__properties__` + per-NS minimal dev nodes, pivot_root into
   `/android`, exec `/system/bin/init`.  Parent polls hwservicemanager
   for `IComposer/default` and setparams `android.composer.ready=1`.
3. **N8 — Composer gate.**  Single cfg `cfg/z_composer_host_gate.cfg`
   adding `start-mode: condition` + `condition: param:android.composer.ready=1`
   on `composer_host` and `allocator_host`.  Everything else
   (Phase 6 VDIs, Phase 8 stability, Phase 11 backlight) inherits
   unchanged — libhybris's own path-redirect map already handles
   `/vendor → /android/vendor` for Android-vendor library loads, so no
   source-level shuffling is needed.

**Phase N9 (peripherals beyond graphics).**  WiFi (Phase 10) and audio
(Phase 13B) are native and inherit cleanly — no Android HAL dependency.
Bluetooth and sensors still need their Android HALs running in
`androidd`'s namespace; defer until N4 is green.

**The original phase docs** (`phase_n*.md`) capture the design
thinking we went through and are kept as historical context.  Where
they describe an approach that was replaced (notably N1; N4/N5/N8 were
rewritten 2026-05-12), treat earlier revisions as design history, not
current state.
