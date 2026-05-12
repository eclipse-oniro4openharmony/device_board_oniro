# Native Boot Plan: OpenHarmony on Volla X23 / Tablet (No Host OS)

Roadmap for transitioning from the current containerized architecture (OHOS-inside-LXC-on-Ubuntu-Touch) to a native boot where OpenHarmony is PID 1 and a stripped Android userspace runs as a guest namespace inside it, providing the HIDL HAL services that libhybris bridges to.

> **Scope.** This plan covers Volla X23 (codename `vidofnir`, `hardware=x23`) and Volla Tablet (`mimir`, MT8781). The two share the Helio G99 SoC and almost all userspace; per-device deltas (kernel patch series, partition geometry, Android-13 hooks for `mimir`) are called out inline.

---

## Current vs Target Architecture

```
Current:                                   Target (native):
┌──────────────────────────────────┐       ┌──────────────────────────────────┐
│   Ubuntu Touch (Halium 12)       │       │  OpenHarmony (PID 1)             │
│   systemd, NM, ofono, adb, lxc   │       │  init_early → init → samgr →     │
│                                  │       │  hilogd → hdcd → render_service  │
│  ┌─────────┐    ┌─────────────┐  │       │                                  │
│  │ Android │    │ OpenHarmony │  │       │  ┌────────────────────────────┐  │
│  │  LXC    │ ↔  │     LXC     │  │       │  │ Android namespace (guest)  │  │
│  └─────────┘    └─────────────┘  │       │  │ hwservicemanager,          │  │
│        ↕  hwbinder (shared IPC)  │       │  │ servicemanager, vndsm,     │  │
└──────────────────────────────────┘       │  │ composer@2.1, alloc@4.0    │  │
                                           │  └────────────────────────────┘  │
                                           │        ↕ hwbinder (PID 1 IPC ns) │
                                           └──────────────────────────────────┘
```

Key inversions:
- **OHOS is PID 1**, owns the kernel, ueventd, fstab, /dev/__parameters__.
- **Android is the guest** in a child namespace tree (no Ubuntu Touch in between).
- **HDC replaces ADB** for USB; OHOS hdcd uses ConfigFS gadget (`init.usb.configfs.cfg`).
- **Phase 10 native WiFi** and **Phase 13B native ALSA audio** stay native — no Android HAL needed for those.
- **Backlight + power-key** logic from Phases 8.15 / 11 already runs out of `composer_host` and OHOS power manager — no Ubuntu Touch dependency.

---

## Reality Check — What Already Works

A surprising amount of the heavy lifting is already done for `hybris_generic`. Before planning new work, account for these:

| Already done | Where | Implication for native boot |
|---|---|---|
| OHOS ramdisk builds | `out/hybris_generic/packages/phone/images/ramdisk.img` (~2.8 MB gzip cpio with `init`→`bin/init_early`, full toybox, `unshare`/`nsenter`/`pivot_root`/`switch_root`) | Phase N1 only needs to repack into Halium `boot.img`, not author a new ramdisk. |
| `enable_ramdisk: true` | `vendor/oniro/hybris_generic/config.json:10` | Already on; no product config flip needed. |
| OHOS `system.img` (2 GB ext4) + `vendor.img` (256 MB ext4) + `userdata.img` (1.4 GB) | Same dir, every full build | Can flash directly; only the partition mapping is open. |
| Kernel cmdline already carries `hardware=x23`, `ohos.boot.sn=...` | `device/board/oniro/hybris_generic/kernel/x23/patch/linux-5.10/volla-vidofnir.patch:12` | OHOS init second-stage already finds `/vendor/etc/fstab.x23` via `${ohos.boot.hardware}`. |
| `selinux=false`, `seccomp=false` | `vendor/oniro/hybris_generic/config.json:12-13` | Native boot inherits this; no further action. |
| 29 `InContainerMode()` guards in init | `base/startup/init/` (count from `grep -rn`, not 27 as old plan claimed) | All 29 default-take the *native* path when the `container=` env is unset; inverting the boot mode auto-runs them. |
| Phase 10 WiFi via HDI WPA + `chip_interface_service` | `phase10_wifi_support.md` | N9.2 collapses to "load wlan/bt firmware + start `wpa_host` + `chip_interface_service`"; no new Android WiFi HAL needed. |
| Phase 13B native ALSA via `libasound`+`audio_host` | `phase13_audio_support.md` | N9.5 collapses to "ensure `/dev/snd/*` exists + start `audio_host`"; no Android Audio HAL needed. |
| Backlight via sysfs from `composer_host` (Phase 11 Fix 1) | `phase11_power_off_and_backlight_plan.md` | Works untouched on native boot; no LXC bind required. |
| Power-button → power manager (Phase 8.15) | `phase8_system_stability.md` §8.15 | The `systemd-logind HandlePowerKey=ignore` workaround disappears — no Ubuntu Touch logind to fight. Pure win. |
| `/storage/Users` sharefs workaround (Phase 12) | `phase12_sharefs_user_files.md` | LXC bind disappears; replace with the *proper* port of `fs/sharefs/` to the X23/mimir kernel — a Phase-2-style kernel patch — see N9.10. |

Net: **the residual native-boot work is Phase N1 (`boot.img` repack), N2 (validate first-stage on bare metal), N3 (write fstab), N4 (replace LXC with a 200-line launcher), N6 (binder device flip), N7 (HDC), N10 (recovery)**. Phases N5/N8 are mostly cfg + path adjustments, not new code. Phase N9 is now mostly firmware loading + the host-side daemons whose work Ubuntu Touch was hiding.

---

## Phase Overview & Dependencies

| Phase | Title | Dependencies | Already-shipped fraction |
|-------|-------|-------------|----|
| N0 | [Pre-flight: Chroot + A-slot Smoke Test](#phase-n0--pre-flight-smoke-test) | (none) | new |
| N1 | [Boot Image & Partition Layout](#phase-n1--boot-image--partition-layout) | Phase 2 (kernel) | ~70% (images + ramdisk build) |
| N2 | [Init System: Native Mode Bring-Up](#phase-n2--init-system-native-mode-bring-up) | N1 | ~80% (`InContainerMode` already gates correctly) |
| N3 | [Filesystem & fstab](#phase-n3--filesystem--fstab) | N1, N2 | 0% (no fstab.x23 yet) |
| N4 | [Android Guest: Namespace Launcher](#phase-n4--android-guest-namespace-launcher) | N2, N3 | new (replaces full-LXC port) |
| N5 | [Android Container Image](#phase-n5--android-container-image) | N4 | ~50% (Halium rootfs already on device) |
| N6 | [Binder Device Management](#phase-n6--binder-device-management) | N4, N5 | ~60% (Python script already creates devices; just port to C and flip roles) |
| N7 | [HDC over USB](#phase-n7--hdc-over-usb) | N2 | ~50% (configfs cfg present, FFS function dir + UDC binding pending) |
| N8 | [Graphics & Display (Native)](#phase-n8--graphics--display-native) | N5, N6 | ~95% (VDIs + Phase 8 stability all carry over; only path bind tweak) |
| N9 | [Firmware, Peripherals & Connectivity](#phase-n9--firmware-peripherals--connectivity) | N3 | ~30% (WiFi/audio done in P10/P13B; modem, sensors, BT remain) |
| N10 | [Flash Tooling, Recovery & Dual-Boot](#phase-n10--flash-tooling-recovery--dual-boot) | N1; **runs in parallel** | 0% |

```
N0 (chroot)──validate──┐
                       ↓
N1 (images) → N2 (init) → N3 (fstab) → N4 (launcher) → N5 (android) → N8 (graphics)
                  │            │              │              │
                  │            └→ N7 (hdc)    └→ N6 (binder)─┘
                  └→ N9.1 (firmware)
N10 (flash + recovery + A/B dev) — runs alongside everything; gates first reboot.
```

---

## Phase N0 — Pre-flight Smoke Test

### Goal
Validate the Android-as-guest *userspace* and the cross-namespace HIDL contract **before** touching the boot chain. This is a 1-day investment that prevents the most likely class of late failures (HAL services don't come up under OHOS-as-host-namespace) by reproducing them while we still have full Ubuntu Touch debug access.

### Approach
On the *current* containerized device, run a third LXC container alongside the existing two: same OHOS rootfs, but with `lxc.namespace.share.ipc = host` and `lxc.namespace.keep` empty — i.e. OHOS in its own PID/mount/UTS namespace but sharing the host (Ubuntu Touch) IPC namespace. Then start the Android container as it is today and confirm the OHOS render_service still gets a frame on the display. This emulates "OHOS owns the parent IPC namespace" in everything but PID 1 ownership.

### Tasks
- **N0.1** — Clone `/var/lib/lxc/openharmony` to `/var/lib/lxc/oh-native-test`; rewrite `lxc.namespace.share.ipc` to `host`; start; verify render_service produces a frame.
- **N0.2** — Strip the Android container's `init.rc` to just the 5 services we care about (see N5.3) and confirm OHOS still boots. This is the same diet Android needs in native boot.
- **N0.3** — Replicate the `unshare(2)` launcher of N4.2 from a host shell: `unshare --mount --pid --uts --fork --mount-proc -- /bin/bash -c 'pivot_root /var/lib/lxc/android/rootfs old; exec /init'`. If it works (HIDL services come up, we reach `Boot completed`) we know N4 is feasible.
- **N0.4** — Time the cold-start path: container down → `lxc-start android` → `hwservicemanager` registered → composer accepting Compose-IPC. This bounds the OHOS-side `wait-for-android` gate in N4.5.

### Exit criterion
Frame on display from N0.1 + a 60-second `unshare`-launched run from N0.3 with no SIGSEGV in the Android side. If either fails, debug here — not after we've reflashed the device.

### Risk if skipped
The dominant risk in N4–N6 is "we built a launcher but cross-namespace hwbinder is broken in some subtle way and we have no Ubuntu Touch shell to debug it." N0 retires that risk for ~$0.

---

## Phase N1 — Boot Image & Partition Layout

### Goal
Produce a flashable `boot.img` whose ramdisk is the OHOS one (already built — see Reality Check) and a partition mapping that lets the OHOS ext4 system + vendor + userdata images coexist with the Android rootfs on the device's `super`.

### Background
- Current `device/board/oniro/hybris_generic/kernel/x23/build_kernel.sh` packs the **Halium** ramdisk into `boot.img`. The OHOS ramdisk is built but not packed.
- The kernel cmdline (`volla-vidofnir.patch:12`) carries `hardware=x23 ohos.boot.sn=...`. We will append `ohos.required_mount.*` entries (Phase N3) so first-stage mounts succeed without an embedded fstab.
- Volla X23 is unlocked and already accepts `dd`-from-Halium-shell flashes of `boot.img` and `vendor_boot.img` (see `kernel/x23/deploy-kernel.sh:50-52`); fastboot is unverified — N10 starts there.

### Tasks

**N1.1 — Map the actual `super` partition.** From a running Halium shell:
```bash
sudo lpdump | tee super-layout.txt          # logical partitions inside super
sudo blkid                                  # filesystem types
ls -la /dev/disk/by-partlabel/              # all partlabels
sudo lptools dump-metadata 2>/dev/null      # if available
```
Document under `device/board/oniro/hybris_generic/docs/x23-super.txt`. Sizes are needed for N1.4. Repeat on `mimir` → `mimir-super.txt`.

**N1.2 — Settle the install target.** Three viable layouts, pick one:

| Layout | Pros | Cons |
|---|---|---|
| **A. OHOS over Android `system_b` + `vendor_b`** (preferred) | A/B safety: `_a` keeps Halium/UT bootable as a recovery slot; no super resize; matches N10.2 dual-boot story. | Halves system+vendor budget (Volla X23 typical: ~3 GB system_a + ~600 MB vendor_a per slot). 2 GB OHOS system + 256 MB vendor fits. |
| **B. Replace `super` entirely with a single ext4 covering OHOS + Android** | Simplest fstab (no dynamic partitions). | Loses A/B; one bad flash bricks. Forfeits N10.2. |
| **C. Resize super; carve new logical partition `oh_system`** | Keeps both OSes on `_a`. | `lpmake` rebuild requires fastboot to flash `super.img`; risk of losing factory metadata. |

Default to **A**. If N10 verifies fastboot works and BROM access is documented, **C** becomes attractive for "ship this device with OHOS *and* Halium recovery."

**N1.3 — Repack `boot.img` with OHOS ramdisk.** Add a sibling script `kernel/x23/build_boot_img_ohos.sh` (do not edit `build_kernel.sh` — keep the Halium build path intact for fallback):
```bash
# Inputs: $KERNEL_TREE/build-dir/tmp/partitions/boot.img        (Halium boot.img)
#         out/hybris_generic/packages/phone/images/ramdisk.img  (OHOS cpio.gz)
# Output: out/hybris_generic/boot-ohos.img
mkbootimg \
  --kernel  <(unpack_bootimg --boot-img $HALIUM_BOOT --extract kernel) \
  --ramdisk out/hybris_generic/packages/phone/images/ramdisk.img \
  --cmdline "$(unpack_bootimg --boot-img $HALIUM_BOOT --get cmdline) \
              ohos.required_mount.system=/dev/block/mapper/system_b@/usr@ext4@ro,barrier=1@wait,required \
              ohos.required_mount.vendor=/dev/block/mapper/vendor_b@/vendor@ext4@ro,barrier=1@wait,required \
              ohos.required_mount.userdata=/dev/block/by-name/userdata@/data@ext4@nosuid,nodev,noatime,discard@wait,check" \
  --header_version 4 --output out/hybris_generic/boot-ohos.img
```
- The cmdline append is the **complete** required-mount list parsed by `LoadFstabFromCommandLine` (`base/startup/init/interfaces/innerkits/fs_manager/fstab.c:553`). Field separator is `@`.
- Keep `dtbo.img` and `vendor_boot.img` untouched — they carry DT and vendor first-stage which is non-OHOS and harmless.

**N1.4 — Image budget.** Measured today on `out/hybris_generic/packages/phone/images/`:
| Image | Size | Target slot (layout A) | Slot capacity (X23 typical) |
|---|---|---|---|
| `system.img` | 2.0 GB | `system_b` | ~3.0 GB |
| `vendor.img` | 256 MB | `vendor_b` | ~600 MB |
| `userdata.img` | 1.4 GB | `userdata` | shared, formatted on first boot |
| `chip_prod.img` | 50 MB | logical inside `_b` (carve) | n/a |
| `sys_prod.img` | 50 MB | logical inside `_b` (carve) | n/a |
| `ramdisk.img` | 2.8 MB | inside `boot.img` | n/a |

`chip_prod` + `sys_prod` are not strictly required for boot; defer if the carve is awkward. Re-measure on `mimir`; the tablet likely has more room.

**N1.5 — Android rootfs placement.** Given (A), the Android rootfs is *not* in OHOS `system.img`. Two sources:
- **A.1** (recommended): leave the Android rootfs where it lives today (`/dev/block/by-partlabel/system_a` and `vendor_a`) and have OHOS read-only mount them at `/android/system` and `/android/vendor`. Zero copy, zero space cost, automatic versioning.
- **A.2** (heavier): pack the Halium `system_a` content into a squashfs at `/android.sfs` inside OHOS `system.img`, loop-mount at boot. Useful only if we move Halium off `_a` entirely.

A.1 is also what the existing LXC config does (it bind-mounts `/system/`, `/vendor/`, `/odm/`, `/apex/` as Android paths inside the OHOS container). The same idea works one level out.

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| `lpdump` not installed on Volla X23 Halium | Medium | Build `lptools` for aarch64 from AOSP source, or read `/dev/block/by-name/super` header by hand (lpmake header layout is documented). |
| MediaTek verified-boot rejects unsigned `boot.img` | High if vbmeta is enforced | The bootloader is unlocked for UT — `fastboot getvar unlocked` confirms. If vbmeta is still checked: pass `androidboot.verifiedbootstate=orange` and `androidboot.veritymode=disabled`. |
| `vendor_boot` first-stage init conflicts | Low | OHOS uses the kernel cmdline directly; we don't execute `vendor_boot`'s `/init` because the kernel uses our boot.img ramdisk. If the vendor ramdisk is concatenated, strip it via `mkbootimg --vendor_ramdisk none`. |
| Cmdline length cap (1024 or 2048 chars depending on kernel) | Low-Medium | If `ohos.required_mount.*` overflows, fall back to a `/vendor/etc/fstab.required` shipped in the OHOS ramdisk and read by `LoadFstabFromFile` (already supported, `fstab.c:506`). |

---

## Phase N2 — Init System: Native Mode Bring-Up

### Goal
Confirm OHOS init (`/bin/init_early` first stage → `/bin/init --second-stage`) boots cleanly as PID 1, doing the work the 29 `InContainerMode()` call sites currently skip.

### Background
The container patches in Phase 1 added `if (InContainerMode()) return;` guards. `InContainerMode()` (`init_utils.c:607`) reads the `container=` environment variable; with no env, returns 0 → native path.

### The 29 sites, by category

```
$ grep -rn "InContainerMode" base/startup/init/ | wc -l
29
```

| Category | Files | Count | Native behaviour |
|---|---|---|---|
| First-stage device + fs setup | `services/init/standard/{init.c,device.c}` | 4 | runs `MountBasicFs`, `CreateDeviceNode`, etc. |
| Service start (sandbox, caps) | `services/init/standard/init_service.c`, `init_common_cmds.c`, `init_cmds.c` | 13 | runs `mksandbox`, applies seccomp, capset |
| Cgroup setup | `services/init/init_cgroup.c` | 2 | mounts `/sys/fs/cgroup` hierarchies |
| SELinux | `services/modules/selinux/selinux_adp.c` | 4 | currently no-op (`build_selinux=false`) regardless |
| ueventd | `ueventd/ueventd_main.c` | 1 | starts ueventd loop |
| Reboot | `services/modules/reboot/reboot.c` | 1 | calls `sync()` + `reboot(LINUX_REBOOT_CMD_RESTART)` |
| `main.c` PID checks + 2-stage transition | `services/init/main.c` | 2 | guards "must be PID 1" / "must be first stage" |
| `libbegetutil` exposed symbol | `interfaces/innerkits/libbegetutil.versionscript` | 1 | export only; no behaviour |
| Updater mode probe | (see `init_firststage.c::SystemPrepare`) | (separate, `InUpdaterMode()`) | skips fstab mount in updater |

`build_selinux=false` already collapses the 4 SELinux sites; the remaining 25 are the live concern.

### Tasks

**N2.1 — Audit each native branch on hardware.** This is best done from N0 *first*, then re-run on bare metal. For each site, confirm:
- the first time it runs natively, no path is broken by stale Halium artefacts (`/dev/block/...` device-mapper names, leftover `/tmp/` mount points).
- error paths log via `EarlyLogInit()` → `/dev/kmsg` (printable via `dmesg` after pstore N10.5 lands).

**N2.2 — `MountBasicFs` on Volla X23.** Maps to `device.c:31-77` and runs unchanged on bare metal. Two device-specific concerns:
- `/dev/pts` requires `CONFIG_DEVPTS_MULTIPLE_INSTANCES=y` (already set per Phase 2 kernel).
- `/sys/fs/selinux` mount fails benignly when SELinux is compiled out — `MountBasicFs` already tolerates `EINVAL`.

**N2.3 — `MountRequiredPartitions` (`init_firststage.c:98-133`).** Reads either `LoadFstabFromCommandLine` (cmdline `ohos.required_mount.*`, preferred — see N1.3) or `LoadRequiredFstab` (`/vendor/etc/fstab.required` or `/etc/fstab.updater`). Mounts everything tagged `wait,required`, then if `/usr` is among them, calls `SwitchRoot("/usr")` (`fstab_mount.c:723,735,938`).

> **Critical correction to the previous draft of this plan**: OHOS mounts the *root* filesystem at `/usr`, **not at `/`** as AOSP would. After `SwitchRoot("/usr")`, `/usr` becomes the new `/`. Native fstab must reflect this. See N3.1.

**N2.4 — Second-stage transition.** `SystemPrepare` ends with `execv("/bin/init", ["init", "--second-stage", buf])` (`init_firststage.c:194`). After SwitchRoot, `/bin/init` resolves to OHOS `/usr/bin/init` (now `/bin/init`). Second-stage parses `/system/etc/init/*.cfg`, runs `pre-init` job (`init.cfg:7`), starts param service, ueventd, watchdog, then opens services per cfg.

**N2.5 — SELinux: stays compiled out for now.** `build_selinux=false` is the pragmatic choice; matches container build. A later phase (post-Milestone 4) can enable `build_selinux=true` and start the long policy-port tail. **Do not** flip this during the bring-up.

**N2.6 — ueventd rules.** Native boot needs ueventd to make `/dev/{mali0,dri/*,dma_heap/*,input/*,binder*,access_token_id,snd/*}` and set perms. Add `device/board/oniro/hybris_generic/cfg/ueventd.x23.rc` (parallel to AOSP `ueventd.rc`):
```
/dev/mali0                0666   graphics  graphics
/dev/dri/card0            0666   graphics  graphics
/dev/dri/renderD128       0666   graphics  graphics
/dev/dma_heap/system      0666   graphics  graphics
/dev/dma_heap/mtk_*       0666   graphics  graphics
/dev/input/event*         0660   root      android_input
/dev/access_token_id      0666   access_token  access_token
/dev/snd/*                0660   audio     audio
/dev/rfkill               0660   wifi      wifi
```
Install via `BUILD.gn` to `/vendor/etc/ueventd.x23.rc`; OHOS ueventd reads `/system/etc/ueventd.cfg` + `/vendor/etc/ueventd.${ohos.boot.hardware}.rc`.

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| Kernel panics in early init with no console | High | N10.5 (pstore/ramoops) + N0.4 (chroot smoke) precede the first reflash. |
| Cgroup v2 vs v1 mismatch in `init_cgroup.c` | Medium | Volla X23 5.10 kernel is hybrid; `init_cgroup.c` mounts both. Verify the hybris kernel didn't disable `CONFIG_CGROUP_*` — Phase 2 added them. |
| ueventd cold-start race with required-partition uevents | Medium | `init_firststage.c:85-95` already retriggers uevents for required devices before mount; works as-is. |
| Root mount point mismatch (was wrong in old plan) | Critical | Mount at `/usr`, not `/`. Confirmed by reading `fstab_mount.c:723`. |

---

## Phase N3 — Filesystem & fstab

### Goal
Define the OHOS fstab in OHOS-native syntax and align the on-disk layout with what `init_firststage.c` and `init.cfg` expect.

### Tasks

**N3.1 — fstab format and location.**

Two delivery paths, both supported by OHOS init; ship both for resilience:

1. **Cmdline** (preferred, simpler): `ohos.required_mount.<name>=<dev>@<mnt>@<type>@<flags>@<fs_mgr>` — see N1.3. Parsed by `LoadFstabFromCommandLine` (`fstab.c:553-568`).
2. **File**: `device/board/oniro/hybris_generic/cfg/fstab.x23` installed to `/vendor/etc/fstab.x23`. Parsed by `mount_fstab_sp /vendor/etc/fstab.${ohos.boot.hardware}` (`init.cfg:16`) for *non-required* second-stage mounts.

```
# /vendor/etc/fstab.x23 — OHOS-native flags only.
# Allowed fs_mgr flags: wait, check, required, nofail, hvb, projquota, casefold, compression, dedup, formattable
# (NOT first_stage_mount, NOT slotselect — those are AOSP, not OHOS)
#<src>                                          <mnt>          <type>  <mnt_flags>                                                         <fs_mgr>
/dev/block/mapper/system_b                      /usr           ext4    ro,barrier=1                                                        wait,required
/dev/block/mapper/vendor_b                      /vendor        ext4    ro,barrier=1                                                        wait,required
/dev/block/by-name/userdata                     /data          ext4    nosuid,nodev,noatime,discard,fscrypt=2:aes-256-cts:aes-256-xts      wait,check
/dev/block/by-name/misc                         /misc          none    none                                                                wait,nofail
# Android rootfs (read-only) — see N1.5 layout A.1
/dev/block/mapper/system_a                      /android/system  ext4  ro,barrier=1                                                        wait,nofail
/dev/block/mapper/vendor_a                      /android/vendor  ext4  ro,barrier=1                                                        wait,nofail
```

Notes:
- `/usr` is the OHOS root after SwitchRoot. AOSP-style `/system` is **not** the mount point.
- `slot_suffix` substitution is not done by OHOS init; the slot must be encoded into the device path (Layout A pins to `_b`). If we ever want OHOS to do A/B selection we have to add it; not in scope here.
- `fileencryption=software,quota` (in the previous draft) was AOSP fs_mgr; the OHOS equivalent is the `fscrypt=...` mount option above plus the `quota` mount option directly, not an fs_mgr flag.
- `/misc` carries OEM unlock + bootloader command messages. Optional but common.

**N3.2 — Root layout after SwitchRoot.**

```
/  (was /usr in ramdisk)            ohos system
├── bin -> /system/bin              symlink farm
├── system/                         OHOS bin/lib64/etc
├── vendor/                         OHOS vendor partition (its own ext4)
├── data/                           userdata partition
├── android/
│   ├── system/                     bind from Halium system_a, ro
│   ├── vendor/                     bind from Halium vendor_a, ro
│   ├── odm/                        symlink or bind, ro
│   └── apex/                       rbind from /android/vendor/apex
├── config/                         configfs (mounted in init.cfg pre-init)
├── dev/
│   ├── binderfs/                   binderfs (Phase N6)
│   ├── __parameters__/             OHOS param service shmem (NOT __properties__)
│   ├── snd/                        ALSA — ueventd
│   └── ...
└── proc, sys, tmp, mnt, storage    standard
```

> **Correction to old plan §N3.3.** OHOS uses `/dev/__parameters__` (param service), **not** `/dev/__properties__` (which is the Android name). The OHOS param service mounts its own shmem internally; init does not need an explicit tmpfs mount. The `__properties__` path is needed only inside the *Android* container — that mount belongs in N5, not N3.

**N3.3 — Tmpfs / pseudo-fs.** Already mounted by `init.cfg` pre-init (`mount configfs none /config`) or by the kernel cmdline (`proc`, `sys` from `MountBasicFs`). What we must add (new `init.x23.cfg` under `/vendor/etc/`):
```json
{
    "jobs": [{
        "name": "pre-init",
        "cmds": [
            "mkdir /dev/binderfs 0755 root root",
            "mount binder binder /dev/binderfs",
            "symlink /dev/binderfs/binder    /dev/binder",
            "symlink /dev/binderfs/hwbinder  /dev/hwbinder",
            "symlink /dev/binderfs/vndbinder /dev/vndbinder",
            "mkdir /android 0755 root root",
            "mkdir /android/system 0755 root root",
            "mkdir /android/vendor 0755 root root",
            "mkdir /android/odm 0755 root root",
            "mkdir /android/apex 0755 root root"
        ]
    }]
}
```
This is `import`-ed by `init.cfg:5` because `${ohos.boot.hardware}=x23`.

**N3.4 — Encryption.** Volla X23 stock Halium uses Android FBE (file-based encryption). When OHOS first mounts `/data`, the existing keys are unrecognised and the partition will be reformatted on first boot. This is acceptable — it's a one-time cost — but **document it**: the device is wiped on the OHOS-flash transition. Same applies on rollback to Halium. For developer iteration N10.6 wraps this.

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| Dynamic-partition device path stability | Medium | `/dev/block/mapper/system_b` is created by the kernel's `dm-linear` from the super partition metadata; ueventd waits in `init_firststage.c:85`. Sometimes appears late — `wait,required` already loops. |
| `mount_fstab_sp` of `fstab.x23` runs *after* required mounts | Low | This is the design; non-required mounts (e.g. `/misc`) come up second-stage, that's fine. |
| First boot wipes user data | Documented | Bake into N10.6 flow. Not a bug. |

---

## Phase N4 — Android Guest: Namespace Launcher

### Goal
Run the five Android HIDL services inside a child namespace of OHOS PID 1, with the smallest possible runtime footprint.

### Background
The previous draft assumed a full LXC userspace port — `lxc-start`, `liblxc.z.so`, `--disable-seccomp`, the works. That is feasible (Alpine and postmarketOS ship LXC against musl) but ~5–10 days of port work, debug-cycle pain on a non-glibc target, and a sizeable attack surface for what is effectively a static set of 5 services that we know how to start and never need to dynamically reconfigure.

A simpler tool already ships in the OHOS ramdisk: **`unshare`, `nsenter`, `pivot_root`, `chroot`** are in `out/hybris_generic/packages/phone/ramdisk/bin/`. A 200-line C launcher (`androidd`) replaces LXC for our use case.

### Tasks

**N4.1 — Decision: launcher vs LXC.** Adopt the launcher; keep the LXC port as a documented fallback in case namespace-shared hwbinder turns out to need cgroup tricks that LXC handles for us. Concrete signals to switch:
- if cgroup-v2 unified hierarchy + cgroup namespaces are required for hwbinder (spoiler: not by anything we've seen), revisit.
- if we want OOM-isolation between OHOS host and Android guest, LXC's cgroup config is more ergonomic; can be added later by wrapping `androidd` invocation in an `lxc-start` after the launcher proves the model.

**N4.2 — `androidd` skeleton.** Add `device/board/oniro/hybris_generic/launcher/androidd.c`:

```c
// Launches the Android HIDL HAL stack as PID 1 of a child namespace tree.
// Mirrors what the current LXC config does, but in ~200 LOC of C.

int main(void) {
    // 1. Pre-flight on host side: binderfs, /dev/snd, gpu nodes are all in place
    //    by the time we run (we're started from init.cfg via 'service' entry,
    //    after pre-init).

    // 2. Create binder device for Android (Phase N6.3 ioctl).
    create_binderfs_device("android-binder");

    // 3. fork; child enters namespaces and execs Android init.
    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS;
    // IPC namespace: SHARE with parent (OHOS PID 1) so hwbinder messages cross.
    // Net: SHARE (we want WiFi/RIL traffic from within the same netns).
    pid_t pid = clone_with(flags, child);
    waitpid(pid, NULL, 0);  // block until Android exits
}

static int child(void *arg) {
    // 4. Mount tmpfs over /android, then bind in rootfs reads.
    mount("tmpfs", "/android", "tmpfs", 0, "size=64M,mode=755");
    bind_recursive("/dev/block/mapper/system_a:ro", "/android/system");
    bind_recursive("/dev/block/mapper/vendor_a:ro", "/android/vendor");
    // ...

    // 5. Mount the per-namespace dev (autodev=0 in current LXC config).
    mount("tmpfs", "/android/dev", "tmpfs", 0, "size=8M,mode=755");
    mknod("/android/dev/null", S_IFCHR | 0666, makedev(1, 3));
    // ...binder bind-mounts (Phase N6.4)
    bind_file("/dev/binderfs/android-binder",  "/android/dev/binder");
    bind_file("/dev/binderfs/hwbinder",        "/android/dev/hwbinder");
    bind_file("/dev/binderfs/vndbinder",       "/android/dev/vndbinder");

    // 6. Per-namespace property store (Android-only).
    mount("tmpfs", "/android/dev/__properties__", "tmpfs", 0, NULL);

    // 7. GPU + DMA-BUF + audio passthrough (rbind from host).
    bind_dir("/dev/mali0",    "/android/dev/mali0");
    bind_dir("/dev/dri",      "/android/dev/dri");      // recursive
    bind_dir("/dev/dma_heap", "/android/dev/dma_heap");

    // 8. pivot_root into /android, drop the old root.
    pivot_root("/android", "/android/old_root");
    chdir("/");
    umount2("/old_root", MNT_DETACH);
    rmdir("/old_root");

    // 9. exec Android init.
    setenv("ANDROID_DATA", "/data", 1);
    setenv("ANDROID_ROOT", "/system", 1);
    execl("/init", "init", NULL);
}
```

**N4.3 — Service registration.** Add `vendor/oniro/hybris_generic/etc/init/androidd.cfg`:
```json
{
    "services": [{
        "name": "androidd",
        "path": ["/system/bin/androidd"],
        "uid": "root",
        "gid": ["root"],
        "caps": ["SYS_ADMIN", "MKNOD", "NET_ADMIN", "DAC_OVERRIDE", "SYS_RESOURCE"],
        "start-mode": "boot",
        "importance": -10
    }],
    "jobs": [{
        "name": "post-fs-data",
        "cmds": [
            "start androidd"
        ]
    }]
}
```
Started after `post-fs-data` so `/data` is available; before `boot && param:bootevent.boot.completed=true` so the composer is up before render_service starts.

**N4.4 — OHOS-side gating.** `render_service` and `composer_host` must not start until the Android composer has registered with `hwservicemanager`. Pattern:
```
# /system/etc/init/composer_host.cfg (existing, augmented)
{
  "services": [{
    "name": "composer_host",
    "path": [...],
    "start-mode": "condition",
    "condition": "android.composer.ready=1"
  }]
}
```
Then in `androidd`, after fork, the parent (running in OHOS context) polls `hwservicemanager` for `android.hardware.graphics.composer@2.1::IComposer/default`; once registered, sets `android.composer.ready=1` via `SystemSetParameter`. Concrete poll: `hidl-gen --query` is not available; use `lshal` from inside the Android namespace via `nsenter -t $androidd_pid -m -p -- /system/bin/lshal | grep '@2.1::IComposer/default'`. Or, simpler: `GetService<IComposer>("default")` from a tiny OHOS-side helper that runs in a polling loop until non-null, then sets the param.

**N4.5 — cgroup constraints.** Optional: put the Android namespace under a memcg cgroup with a hard limit (~512 MB) so a runaway Android service can't kill OHOS. Configured in `androidd.cfg`:
```
"cpuCore": [0,1,2,3],
"cgroup": {
    "memory": {"limit": "536870912"},
    "cpuset": {"cpus": "0-3"}
}
```
OHOS init already supports the `cgroup` field per service (`init_cgroup.c`). Skip if it complicates bring-up; add later.

### Why not LXC?
| Concern | LXC port | `androidd` launcher |
|---|---|---|
| Effort | 5-10 days port + debug | 1 day |
| Runtime size | ~2 MB binary + libs | ~30 KB |
| Configurability | rich (we'd use ~5%) | exactly what we need |
| Failure modes | many — apparmor, seccomp, cgroup driver, pivot_root variant | clear, in our code |
| Future-proof | drift with LXC upstream | drift with our fork — we own it |

If a future device requires multiple parallel Android versions or dynamic container management, revisit.

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| Cross-namespace hwbinder fails | Critical | N0.1 retires this risk before any reflash. |
| Android `init` panics on missing `/proc/cmdline` content (e.g. `androidboot.hardware`) | Medium | Bind a synthetic `/proc/cmdline` with the keys Android wants; or `setprop` them in `androidd` before `execl`. |
| Mali driver doesn't tolerate two GL clients in different mount namespaces | Low (already works in current setup) | Same `/dev/mali0` is bind-mounted into both; the Mali kernel driver is multi-process; verified in Phase 5. |
| `pivot_root` requires the new root to be a *different mount* than the old | Medium | The `mount("tmpfs", "/android", ...)` step satisfies this. Verified in N0.3. |
| OHOS musl `clone(2)` ABI quirk | Low | OHOS musl is upstream-aligned for `clone` flags; if `CLONE_NEWUSER` later: separate problem. |

---

## Phase N5 — Android Container Image

### Goal
A read-only Android rootfs containing exactly the binaries needed for the 5 HAL services, sized to ~50–80 MB and bootable in <3 s.

### Background
Halium's Android stage is large (~600 MB) because it carries a phone UI it doesn't need. We want HAL services only.

### Tasks

**N5.1 — Source the rootfs.**
- **Option A** (recommended for bring-up): mount the existing Halium `system_a` + `vendor_a` directly. No surgery, fast iteration. The container we ship in production trims it.
- **Option B**: extract from the Volla X23 stock firmware (system + vendor + odm + apex) and trim. Larger up-front investment.
- **Option C**: rebuild a stripped Halium tree. Largest investment; only worthwhile if upstream Halium changes break us.

Adopt **A** until N4 is green, then switch to **B**.

**N5.2 — Trimmed init.rc.** Drop `init.rc` overlay at `/android/init.hal-only.rc`:
```
# Disable everything that isn't a HAL service.
import /init.environ.rc
import /init.usb.rc

on early-init
    # Reduced from upstream Halium init.rc — only what hwbinder + composer needs.

on init
    mkdir /dev/socket 0755 root root
    mkdir /mnt 0775 root system
    chmod 0666 /dev/binder
    chmod 0666 /dev/hwbinder
    chmod 0666 /dev/vndbinder

service hwservicemanager /system/bin/hwservicemanager
    class core
    user root
    group root readproc

service servicemanager /system/bin/servicemanager
    class core
    user root
    group root readproc

service vndservicemanager /vendor/bin/vndservicemanager /dev/vndbinder
    class core
    user root
    group root

service composer-2-1 /vendor/bin/hw/android.hardware.graphics.composer@2.1-service
    class hal
    user root
    group graphics
    capabilities SYS_NICE

service allocator-4-0 /vendor/bin/hw/android.hardware.graphics.allocator@4.0-service-mediatek
    class hal
    user root
    group graphics
```
Loaded by passing `INIT_USER_RC=/init.hal-only.rc` env to Android init.

**N5.3 — Pre-seeded properties.** Before `execl("/init", ...)` in `androidd`, set:
```
ro.hardware=mt6789
ro.hardware.egl=mali
ro.hardware.vulkan=mali
ro.board.platform=mt6789
ro.zygote=zygote64
ro.bionic.arch=arm64
debug.sf.no_hw_vsync=0
```
The X23 set is what `start-ohos.sh` already plants (`device/board/oniro/hybris_generic/utils/start-ohos.sh`). For `mimir`, additionally set the Android-13 `ro.product.first_api_level=33` so libunwindstack's `CallStack` hook trips correctly (Phase 9.2).

**N5.4 — IPC namespace orientation.**
The previous draft used `lxc.namespace.share.ipc = 1` — that's not valid LXC syntax (`share.<ns>` takes a PID, container name, or `host`, not `1`). With the launcher, the call site is just `clone(...)` *without* `CLONE_NEWIPC`, which inherits OHOS PID 1's IPC namespace — exactly what we want for hwbinder. No string config to get wrong.

> Current LXC config at `device/board/oniro/hybris_generic/utils/device/lxc/config:17` uses `lxc.namespace.share.ipc = android` (OHOS sharing Android's IPC namespace). The native flip is "Android sharing OHOS's" — the launcher gets this for free by *not* unsharing IPC.

**N5.5 — Property system (Android side).** Android `/dev/__properties__` is created by Android `init` itself when it starts the property service; the `mount tmpfs /dev/__properties__` in `androidd` (N4.2 step 6) just provides the mount point. **No host-side property bind from OHOS to Android** — they're independent param/property systems and stay separate.

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| Android `init` expects to load full `/system/etc/init/*.rc` | Medium | `init.hal-only.rc` overrides the default; for safety also `chmod 0000` the Android rcs we want suppressed. |
| `androidboot.selinux=permissive` is required | Medium | Bind a `/proc/cmdline` with this set inside the namespace, or pass via Android's "androidboot.*" property mapping in `init.environ.rc`. |
| HAL service crashes loop and consumes CPU | Medium | `oneshot` or restart-limit in `init.hal-only.rc`; expose oom-score so OHOS never gets killed before Android. |

---

## Phase N6 — Binder Device Management

### Goal
Provision separate `binder` context-manager devices for OHOS (host) and Android (guest), with `hwbinder` and `vndbinder` shared across both.

### Background
Today (`utils/device/lxc/config:33-38`):
- `/dev/binder` in container = `/dev/binderfs/ohos-binder` (OHOS gets its own)
- `/dev/hwbinder`, `/dev/vndbinder` are the host's shared devices.

Native flips this: OHOS as PID 1 owns the *default* devices, Android gets a dedicated one.

### Tasks

**N6.1 — Mount binderfs in N3.3.** Already in the cfg above.

**N6.2 — Allocation.** OHOS `samgr` registers the context manager on `/dev/binder` (the default) at second-stage start. The `androidd` launcher creates `android-binder` *before* fork and bind-mounts it as `/dev/binder` inside the Android namespace. Result:
- OHOS sees: `/dev/binder` (= `/dev/binderfs/binder`), `/dev/hwbinder`, `/dev/vndbinder`.
- Android sees: `/dev/binder` (= `/dev/binderfs/android-binder` bind), `/dev/hwbinder` (same kernel object as OHOS sees), `/dev/vndbinder` (same).

**N6.3 — C device-creation utility.** Replace the existing Python script (`/home/phablet/openharmony/create_ohos_binder.py`) with a function in `androidd`:
```c
static int create_binderfs_device(const char *name) {
    int fd = open("/dev/binderfs/binder-control", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    struct binderfs_device dev = {0};
    strncpy(dev.name, name, sizeof(dev.name) - 1);
    int rc = ioctl(fd, BINDER_CTL_ADD, &dev);
    close(fd);
    return rc;
}
```
Idempotent: if the device already exists, ioctl returns `EEXIST` — treat as success.

**N6.4 — Symlinks for OHOS.** Already in `init.x23.cfg` from N3.3.

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| `binderfs` not in kernel | Critical | `CONFIG_ANDROID_BINDERFS=y` is in `openharmony.config` — verify; this same kernel is what we ship today. |
| Context-manager registration race (OHOS vs Android) | Low | OHOS samgr starts at second-stage; `androidd` is `start-mode boot`, runs after pre-init but before samgr's full registration. Either order works because they target distinct binder devices. |
| Cross-binder garbage collection in shared hwbinder | Low | This is what hybris already exercises via the host. Native is the same kernel object. |

---

## Phase N7 — HDC over USB

### Goal
HDC over USB on first boot of native OHOS so we can debug without a serial console.

### Background
- `/system/etc/init/init.usb.cfg` (`base/startup/init/services/etc/init.usb.cfg`) imports `/vendor/etc/init.${ohos.boot.hardware}.usb.cfg`. We must ship `init.x23.usb.cfg`.
- USB DRD on MT6789 is `mtu3`; UDC name typically `musb-hdrc.0` or `mt_usb`. Confirm via `ls /sys/class/udc/` *from current Halium shell* before native boot.
- `developermode.state=on` is required for hdc to accept inbound connections (project memory `project_hdc_connection.md`). On first OHOS boot, this is unset — hdc binds the FFS endpoint but rejects the host. Two options: (a) set the param at first boot via `param:set`; (b) ship a default `developermode.state=on` for the dev/eng build only.

### Tasks

**N7.1 — `init.x23.usb.cfg`.** Mirror `drivers/peripheral/usb/cfg/init.usb.configfs.cfg` but with the X23 controller name:
```json
{
    "jobs": [{
        "name": "boot",
        "cmds": [
            "setparam sys.usb.controller musb-hdrc.0",
            "setparam sys.usb.config hdc",
            "setparam sys.usb.configfs 1"
        ]
    }]
}
```
Append to `vendor/oniro/hybris_generic/etc/init/`.

**N7.2 — Default to dev mode for the eng build.** In `vendor/oniro/hybris_generic/etc/param/`:
```
const.developermode.state=on
```
Strip this for any non-dev build.

**N7.3 — TCP fallback (always-on during bring-up).** `start-ohos.sh` already does this for the LXC build. Replicate as a native init job: if `persist.hdc.mode.tcp=enable`, start hdcd with `-l 0`. Requires N9 networking — chicken-and-egg if WiFi isn't up. Mitigation: USB RNDIS gadget for IP-over-USB, also configurable via `init.x23.usb.cfg` adding the `rndis_hdc` mode (already templated upstream).

**N7.4 — Host-side validation (already works for container).**
```bash
adb forward tcp:8712 tcp:8712 && hdc start -r && hdc tconn 127.0.0.1:8712
```
For native USB, drop the `adb forward` step; the device will appear directly to `hdc list targets`.

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| `mtu3` not loaded | Medium | It is built-in on the X23 Halium kernel; verify by `lsmod` + `ls /sys/class/udc/` pre-flash. |
| `developermode.state` unset → hdc rejects | Medium | N7.2 ships default `on` for eng; production builds must add the toggle UI. |
| USB-C role-switch defaults to host | Low-Medium | DT default on the X23 is device mode for the bottom port; if not, push `usb_role_switch` ucsi command at boot. |

---

## Phase N8 — Graphics & Display (Native)

### Goal
`render_service` lights pixels on the panel, reusing all of Phases 5–8 unchanged.

### Tasks

**N8.1 — Library path strategy.** The libhybris hardcoded paths today are:
- OHOS: `/system/lib64/libhybris_*`, `/system/lib64/libGLES_mali.so` (symlinks via `libEGL_impl.so` etc.)
- Android-side reads: `/vendor/lib64/egl/`, `/vendor/lib64/hw/...` — under the LXC container these are bind-mounted *from the host's* `/android/vendor/lib64/...`.

After native boot, `/vendor` is **OHOS vendor**, not Android vendor. We must not overlay Android's vendor libs onto OHOS's vendor partition (path collision, was the cause of the 2026-03-20 SPHAL revert documented in `lxc/config:71-79`).

Use a separate prefix for the Android-vendor libs, e.g. `/system/lib64/hybris_vendor/egl/` and `/system/lib64/hybris_vendor/hw/`, then patch `libhybris/utils/properties.c` to look there. Concrete diff is small (already a pattern: `HYBRIS_LD_LIBRARY_PATH`). This avoids touching `/vendor/lib64/`.

Alternative (minimally invasive): keep the bind-mount, but mount Android's libs at `/vendor/lib64/hw_android/` and `/vendor/lib64/egl_android/`, then adjust libhybris's hard-coded `dlopen` paths. About 5-10 sites in `third_party/libhybris/hybris/`.

**N8.2 — EGL/GLES symlinks** identical to current setup; nothing changes after the path remapping above.

**N8.3 — Env vars** carried by the existing `hybris_graphic_env.cfg` — no change.

**N8.4 — Composer readiness gate.** Implemented in N4.4.

**N8.5 — Device-node access** is via ueventd rules in N2.6. `/dev/mali0`, `/dev/dri/*`, `/dev/dma_heap/*` ownership matches the existing LXC bind permissions.

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| Path collision between OHOS `/vendor/lib64/egl/` and Android's | High | N8.1: relocate Android vendor libs to a non-`/vendor` prefix. |
| Phase 8.17 Mali NULL+0x1d8 crash recurs natively | Unknown | Whatever the root cause is, native boot doesn't change the EGL teardown sequence; expect to reproduce. Track in §8.17 of phase8 doc. |

---

## Phase N9 — Firmware, Peripherals & Connectivity

### Goal
The peripherals Ubuntu Touch was loading transparently come up under OHOS.

### What's already done

- **WiFi (Phase 10).** Native OHOS HDI WPA via `wpa_host` + `chip_interface_service`; `wpa_supplicant` from Halium is **not** used. Native boot inherits this — just need to ensure (a) WiFi firmware is reachable, (b) the host `wpa_supplicant` we currently mask in `start-ohos.sh` isn't there to fight (true natively), (c) `rfkill unblock all` runs at boot (was in `start-ohos.sh`; move to `init.x23.cfg`).
- **Audio (Phase 13B).** Native ALSA via `audio_host` + `libasound`. Native boot inherits this — just need `/dev/snd/*` perms (ueventd rule in N2.6) and `mt6789-mt6366` codec firmware.
- **Backlight (Phase 11 Fix 1).** sysfs writer in `composer_host`. Native boot inherits.
- **Power button (Phase 8.15).** Power manager already handles it. The `systemd-logind HandlePowerKey=ignore` workaround disappears (no logind to fight) — pure simplification.

### Tasks (still open)

**N9.1 — Firmware loading.** Inventory & install path:
| Component | Source path | Native install path | Notes |
|---|---|---|---|
| WiFi (MT7663 / connsys) | `/vendor/firmware/WIFI_RAM_CODE_*.bin`, `WIFI_MT*` | `/vendor/firmware/` | Already on Halium vendor partition; survives N1.5(A.1) |
| BT (MT7663) | `/vendor/firmware/BT_RAM_CODE_*.bin` | same | same |
| Mali GPU | built into kernel module | n/a | |
| Modem (CCCI) | `/vendor/firmware/md1*.img`, `md1_filter.bin` | same | Telephony out of scope |
| Audio codec (mt6366) | usually in-kernel | n/a | Phase 13B verified |

Set `firmware_class.path=/vendor/firmware` on the kernel cmdline if the kernel doesn't search there by default. Verify on Halium with `cat /sys/module/firmware_class/parameters/path`.

**N9.2 — WiFi.** Already done (Phase 10). Add to `init.x23.cfg`: `exec_start /system/bin/rfkill unblock all`. Start `wpa_host` and `chip_interface_service` via their existing service entries; no extra config.

**N9.3 — Modem / Telephony.** Out of scope for Milestone 4. Future work: port MTK CCCI userspace daemons (`ccci_mdinit`, `ccci_fsd`) into the Android container; expose RIL via OHOS telephony VDI. Need stock `nv` and `protect_*` partitions populated from Halium for IMEI provisioning.

**N9.4 — Bluetooth.** `android.hardware.bluetooth@1.0-service-mediatek` in the Android namespace, OHOS-side talks to it via `/dev/hwbinder`. Symmetric to graphics. Add as service #6 to N5.2's `init.hal-only.rc` once the BT VDI is written. Estimated 2–3 days of bring-up similar to WiFi but smaller surface.

**N9.5 — Audio.** Already done (Phase 13B). Audit: `/dev/snd/*` perms (N2.6), `audio_host` service starts, mixer paths probe. No new work expected.

**N9.6 — Sensors.** `android.hardware.sensors@2.1-service-mediatek` in Android namespace; OHOS sensorservice talks via hwbinder. Defer to post-Milestone 4.

**N9.7 — Power management.** OHOS power manager + kernel `cpufreq` + `/sys/class/power_supply/` work natively. Suspend via `/sys/power/state` works (Phase 11 Fix 2 already exercises the OHOS reboot path). Verify wakeup sources are sane; MTK SPMI typically requires no extra config.

**N9.8 — Camera.** Out of scope; Camera HAL bridge is its own multi-week project.

**N9.9 — `ofono`/RIL replacement.** Out of scope (telephony).

**N9.10 — `sharefs` kernel port.** Phase 12 currently uses an LXC-time bind workaround. Native boot has no LXC-time hook; the *proper* fix becomes load-bearing here. Port `fs/sharefs/` from OHOS linux-6.6 to the X23/mimir 5.10 kernel (similar in shape to the Phase-2 hilog/binder port). Estimated 1 week. Until then, the `androidd` launcher's `bind_dir` of `nosharefs/docs → sharefs/docs` is a 5-line equivalent of the LXC bind — works as a temporary stand-in.

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| Firmware path mismatch | Medium | Layout A.1 (Android vendor mounted at `/android/vendor`) means firmware *also* needs to be reachable at the kernel-expected path. Either symlink `/vendor/firmware → /android/vendor/firmware` post-mount or set `firmware_class.path` to both. |
| Missing `nv` calibration after wipe | High for telephony | Telephony out of scope for Milestone 4; if pursued, stock `nv` and `protect_*` partitions must be backed up and restored during Phase N10 flash. |

---

## Phase N10 — Flash Tooling, Recovery & Dual-Boot

### Goal
Make the experiment safe. **Runs in parallel with everything else; gates the first reflash.**

### Tasks

**N10.1 — Flash procedure.** `device/board/oniro/hybris_generic/utils/flash-native.sh`:
```bash
# Flashes OHOS to the inactive A/B slot.
# Requires: device in fastboot, adb authorised, host has OHOS images at out/.

set -euo pipefail
SLOT_INACTIVE=$(fastboot getvar current-slot 2>&1 | grep -oE "[ab]" | tr ab ba)  # opposite slot
fastboot flash boot_${SLOT_INACTIVE}        out/hybris_generic/boot-ohos.img
fastboot flash dtbo_${SLOT_INACTIVE}        out/hybris_generic/dtbo.img
fastboot flash vendor_boot_${SLOT_INACTIVE} out/hybris_generic/vendor_boot.img
fastboot flash system_${SLOT_INACTIVE}      out/hybris_generic/packages/phone/images/system.img
fastboot flash vendor_${SLOT_INACTIVE}      out/hybris_generic/packages/phone/images/vendor.img
# Don't wipe userdata — first OHOS boot will reformat it (one-way per N3.4).
fastboot set_active ${SLOT_INACTIVE}
fastboot reboot
echo "Booting OHOS on slot ${SLOT_INACTIVE}; Halium remains on the other slot."
```

If fastboot is unavailable on the X23 (some Halium devices restrict it), the immediate fallback is **dd-over-adb** — already used by `kernel/x23/deploy-kernel.sh:50-52` for boot.img today. Extend it to system/vendor (slow, but works without fastboot):
```bash
adb push system.img /tmp/
adb shell "echo 1234 | sudo -S dd if=/tmp/system.img of=/dev/block/by-name/system_b bs=8M"
```

**N10.2 — A/B dual-boot, the safety net.**
- **A slot**: Halium 12 / Ubuntu Touch (untouched).
- **B slot**: OHOS (the experiment).
- Boot the experiment with `fastboot set_active b`. Roll back with `set_active a` from fastboot or by holding volume-down through BROM if the bootloader is hung.
- This means **never run the flash script with the device's *active* slot as the target**.

**N10.3 — Recovery image.** OHOS has an updater target (`base/update/updater/`). `out/hybris_generic/packages/phone/images/updater.img` is already 21 MB and built. Investigate whether it can be flashed to `recovery_b` and entered via `reboot recovery`. If not, the Halium A-slot is the recovery — boot it and re-flash.

**N10.4 — UART debug.** Volla X23 has test pads for UART per the mainline schematic; document the location and pinout in a separate guide if/when a developer pries open a unit. Until then, pstore is the only post-mortem channel.

**N10.5 — pstore / ramoops.**
- Add to kernel cfg (Phase 2 build): `CONFIG_PSTORE=y`, `CONFIG_PSTORE_RAM=y`, `CONFIG_PSTORE_CONSOLE=y`, `CONFIG_PSTORE_PMSG=y`.
- Reserve a ramoops region via DT overlay (`device/board/oniro/hybris_generic/kernel/x23/patch/linux-5.10/ramoops.patch`):
```
ramoops {
    compatible = "ramoops";
    reg = <0x0 0xfffe0000 0x0 0x20000>;  // pick an unused region; verify on the X23 memory map
    record-size  = <0x4000>;
    console-size = <0x4000>;
    pmsg-size    = <0x4000>;
};
```
- After a panic, logs at `/sys/fs/pstore/console-ramoops-0` next boot.
- **Do this in Phase 2 next kernel rebuild — so it's on-device by the time N0/N1 ship.**

**N10.6 — Dev iteration loop (fast path).** While bringing up natively, the slow loop is:
1. Build OHOS (`./build.sh`)
2. Flash all of it (~3 min over USB)
3. Reboot (~1 min)
4. Lose userdata each time fstab changes encryption format

For tight iteration:
- Flash `boot.img` only when changing kernel/cmdline/ramdisk (`fastboot flash boot_b boot-ohos.img`). ~10 s.
- For OHOS service iteration during native bring-up: cross-mount via `hdc` + `mount -o remount,rw /` over the running ext4 system, push deltas, kill service. Same loop you use today on the LXC build — just over hdc instead of `adb shell lxc-attach`.
- Keep `userdata` formatted across runs; only reformat when fscrypt config changes. Encode in the script.

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| Bootloop on the inactive slot | Medium | Active slot stays Halium; volume-down recovery to fastboot still works. |
| Fastboot disabled by Halium | Medium | dd-over-adb (N10.1 fallback). |
| Brick during early N1 flash | Low | A-slot Halium intact; mtkclient BROM as final fallback. Keep `mtkclient` checked out in a dev directory; document the BROM key combo (Volla X23: hold both volume keys + USB plug). |
| pstore region overlaps used memory | Medium | Pick the address from MTK's memory map (typically the last 64 KB before reserved); verify with `/proc/iomem` from Halium. |

---

## Implementation Order & Milestones

### Milestone 0: Smoke test (Phase N0)
Reproduce the OHOS-as-host topology from the existing Halium-LXC build, no new images flashed. Validates the launcher model.

### Milestone 1: Boot to hdc shell (N1 + N2 + N3 + N7 + N10)
OHOS boots, mounts `/`, `/vendor`, `/data`; `hdcd` over USB; HDC connects from the host. **No graphics, no Android.** This is the "boot chain works" milestone — much smaller than it looks once N0 is green and N10 has a rollback path.

### Milestone 2: Android namespace running (N4 + N5 + N6)
`androidd` brings up the 5 HAL services in a child namespace. `lshal` from `nsenter` shows them registered with `hwservicemanager`. **No display yet** (no render_service started).

### Milestone 3: Display (N8)
render_service connects to Android HWC2; bootanimation plays; launcher visible. Inherits 100% of Phase 6 + Phase 8 stability fixes.

### Milestone 4: Daily-driver prototype (N9 partial)
WiFi (Phase 10), audio (Phase 13B), input (Phase 7), backlight + power (Phases 8.15 + 11) — all already work; just need the perms cfg in place. Bluetooth and sensors as stretch goals.

---

## Risk Summary

| Risk | Severity | Likelihood | Phase | Notes |
|------|----------|------------|-------|-------|
| Cross-namespace hwbinder breaks under OHOS-as-host topology | Critical | Low (Halium proves it works) | N0/N4 | Retired by N0 before any reflash. |
| Super partition slot too small for OHOS images | High | Low | N1 | 2 GB system + 256 MB vendor fits typical X23 `_b` budget (~3 GB). Measure first. |
| First-stage init failure with no kmsg | High | Medium | N2 | pstore/ramoops (N10.5) + N0 chroot smoke. |
| Path collision between OHOS `/vendor` and Android vendor libs | High | High if not addressed | N8 | N8.1 — relocate Android-vendor libs out of `/vendor/lib64/`. |
| `developermode.state` blocks first hdc connect | Medium | High | N7 | Default-on for eng builds. |
| Bricked device during dev iteration | Critical | Low | N10 | A/B slot strategy — Halium on A as recovery. |
| Telephony provisioning lost on userdata wipe | High for phones, irrelevant for tablet | Medium | N9.3 | Out of scope for Milestone 4; back up `nv` and `protect_*` if pursued later. |
| Phase 8.17 Mali crash regresses | Medium | Unknown | N8 | Same code path; expect to reproduce, root-cause is still open in §8.17. |
| `fs/sharefs/` LXC-bind disappears on native | Medium | High | N9.10 | Either ship the `androidd` bind equivalent (5 lines) or do the proper kernel port. |

---

## Open Decisions (pre-N0)

1. **Layout: A vs B vs C** in N1.2. Default A (per-slot dual boot) unless `lpdump` shows insufficient room.
2. **Launcher: `androidd` C binary vs LXC port** in N4. Default `androidd`; LXC port is a documented escape hatch.
3. **Android-vendor libs path: `/vendor/lib64/hw_android/` (bind) vs `/system/lib64/hybris_vendor/`** in N8.1. Default the latter (no `/vendor` overlap), but it touches more libhybris source files. Decide after N0.
4. **Tablet (mimir): full parallel track or follow-on after X23?** Default: bring up X23 to Milestone 3, then port mimir as a single PR set (kernel cmdline + Android-13 hooks already in Phase 9). Avoid two-target debugging during the high-risk N1–N6 stretch.

---

## Per-Device Deltas (X23 vs mimir)

| Item | X23 (`vidofnir`, MT6789) | Tablet (`mimir`, MT8781) |
|---|---|---|
| Kernel patches | `kernel/x23/patch/` | `kernel/mimir/patch/` |
| `hardware=` | `x23` | `mimir` |
| Android base | 12 (Halium) | 13 (with Phase 9 libhybris hooks) |
| Speaker path (Phase 13B) | DL1 → I2S3 → AW883xx | Ext_Speaker_Amp |
| `mksandbox` quirks | none | none |
| Display res | 720×1560 | 1600×2560 |
| Super geometry | TBD via `lpdump` | TBD via `lpdump` |
| Halium IPC namespace fix (Phase 9.3) | n/a | `start-ohos.sh` switches `lxc.namespace.share.ipc` dynamically — replicate in `androidd` ENV check on `${ohos.boot.hardware}` |

The right framing: bring up X23 first (smaller surface, more Phase artefacts). Mimir port is a small PR if Phase 9's hook set still applies — it should.
