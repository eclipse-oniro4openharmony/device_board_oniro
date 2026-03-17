# Native Boot Plan: OpenHarmony on Volla X23 (No Host OS)

Roadmap for transitioning from the current containerized architecture (OHOS-inside-LXC-on-Ubuntu-Touch) to a native boot where OpenHarmony is the primary OS and Android runs as a guest LXC container.

---

## Current Architecture

```
┌──────────────────────────────────────────────────┐
│               Ubuntu Touch (Host OS)             │
│  kernel, systemd, NetworkManager, ofono, adb ... │
│                                                  │
│  ┌──────────────────┐  ┌──────────────────────┐  │
│  │ Android LXC      │  │ OpenHarmony LXC      │  │
│  │ hwservicemanager  │  │ init → samgr →       │  │
│  │ servicemanager    │  │ render_service →     │  │
│  │ composer@2.1      │  │ foundation → ...     │  │
│  │ allocator@4.0     │  │                      │  │
│  └──────────────────┘  └──────────────────────┘  │
│         ↕  hwbinder (shared IPC namespace)  ↕    │
└──────────────────────────────────────────────────┘
```

## Target Architecture

```
┌──────────────────────────────────────────────────┐
│           OpenHarmony (Native Boot)              │
│  kernel → init_early → init → samgr →            │
│  render_service → foundation → hdcd → ...        │
│                                                  │
│  ┌──────────────────────────────────────────┐    │
│  │           Android LXC Container          │    │
│  │  minimal init → hwservicemanager →       │    │
│  │  servicemanager → composer@2.1 →         │    │
│  │  allocator@4.0 → vndservicemanager       │    │
│  └──────────────────────────────────────────┘    │
│         ↕  hwbinder (shared IPC namespace)  ↕    │
└──────────────────────────────────────────────────┘
```

Key differences:
- **No Ubuntu Touch layer** — OHOS init is PID 1, owns the kernel, mounts filesystems, manages devices
- **Android is the guest** — LXC runs inside OHOS (role reversal)
- **HDC replaces ADB** — USB debug via OpenHarmony's native `hdcd` daemon
- **OHOS owns hardware** — kernel modules, firmware loading, GPU, display, input all managed by OHOS

---

## Phase Overview

| Phase | Title | Dependencies |
|-------|-------|-------------|
| N1 | [Boot Image & Partition Layout](#phase-n1--boot-image--partition-layout) | Phase 2 (kernel) |
| N2 | [Init System: Native Mode Bring-Up](#phase-n2--init-system-native-mode-bring-up) | N1 |
| N3 | [Filesystem & fstab](#phase-n3--filesystem--fstab) | N1, N2 |
| N4 | [LXC Inside OpenHarmony](#phase-n4--lxc-inside-openharmony) | N2, N3 |
| N5 | [Android Container (Guest)](#phase-n5--android-container-guest) | N4 |
| N6 | [Binder Device Management](#phase-n6--binder-device-management) | N4, N5 |
| N7 | [HDC over USB](#phase-n7--hdc-over-usb) | N2, N3 |
| N8 | [Graphics & Display (Native)](#phase-n8--graphics--display-native) | N5, N6 |
| N9 | [Firmware, Peripherals & Connectivity](#phase-n9--firmware-peripherals--connectivity) | N3 |
| N10 | [Flash Tooling & Recovery](#phase-n10--flash-tooling--recovery) | N1 |

```
N1 (images) ──→ N2 (init) ──→ N3 (fstab) ──→ N4 (lxc) ──→ N5 (android) ──→ N8 (graphics)
                    │              │              │              │
                    │              │              └──→ N6 (binder) ──→ N8
                    │              │
                    └──→ N7 (hdc) │
                                  └──→ N9 (firmware)
N10 (recovery) — parallel, critical safety net
```

---

## Phase N1 — Boot Image & Partition Layout

### Goal
Produce flashable images (`boot.img`, `system.img`, `vendor.img`, `userdata.img`) for the Volla X23's A/B partition scheme.

### Background
The Volla X23 uses an A/B partition layout with MediaTek's dynamic "super" partition containing logical partitions (`system_a/b`, `vendor_a/b`, `product_a/b`). The existing kernel build (`device/board/oniro/hybris_generic/kernel/x23/build_kernel.sh`) already produces `boot.img`, `dtbo.img`, and `vendor_boot.img`. The OHOS build system (`build/ohos/images/`) can generate ext4 system and vendor images.

### Tasks

**N1.1 — Map the Volla X23 partition table**
- Dump the full partition layout from the device: `ls -la /dev/disk/by-partlabel/`
- Identify which partitions can be repurposed for OHOS (the Android `system` and `vendor` logical partitions inside `super`)
- Document the super partition layout using `lptools` or `lpdump`
- Record partition sizes to set image size limits

**N1.2 — Create the OHOS ramdisk for Volla X23**
- Extend the standard OHOS ramdisk (`build/ohos/images/mkimage/ramdisk_image_conf.txt`) with Volla-X23-specific content:
  - `fstab.volla_x23` (see Phase N3)
  - Kernel modules needed at early boot (if any)
- Ensure `/init` → `/bin/init_early` symlink is present
- Pack as CPIO (`mkcpioimage.py`), verify size fits the 15 MB debug limit during development

**N1.3 — Produce boot.img with OHOS ramdisk**
- The current `build_kernel.sh` packs a Halium ramdisk into `boot.img`. Modify it (or create a parallel script) to pack the OHOS ramdisk instead.
- The kernel cmdline must be set for OHOS: remove any `systempart=`, `androidboot.` parameters that conflict; ensure `init=/init` (default) is present.
- `dtbo.img` and `vendor_boot.img` remain unchanged — they carry device tree and vendor ramdisk from the original Halium build.

**N1.4 — Generate system.img and vendor.img**
- Configure the OHOS build to output ext4 images for the `hybris_generic` product:
  - `system.img` — OHOS `/system` + `/` root directories (uses `system_image_conf.txt`, currently 2 GB)
  - `vendor.img` — OHOS `/vendor` contents (uses `vendor_image_conf.txt`, currently 256 MB)
- Size images to fit within the Volla X23's super partition logical volumes
- Include `userdata.img` (empty ext4 or f2fs, formatted on first boot)

**N1.5 — Integrate Android rootfs into the system image**
- The Android HAL container needs its rootfs accessible from OHOS. Two options:
  - **Option A (recommended):** Pack the Android `system` and `vendor` trees as a read-only squashfs image at `/android.img` inside the OHOS system partition, loop-mounted at boot
  - **Option B:** Include the Android trees directly under `/android/system/`, `/android/vendor/`, `/android/odm/` in the OHOS system image (simpler but wastes space and complicates updates)

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| **Super partition too small** | Dynamic partitions may not have room for OHOS + Android | Measure existing sizes; consider removing product/product_services partitions; resize logical volumes with `lptools` |
| **MediaTek preloader / verified boot** | Custom images may fail signature checks | Bootloader is already unlocked for Ubuntu Touch; verify with `fastboot getvar unlocked` |
| **vendor_boot ramdisk conflicts** | Halium vendor_boot may inject Android-specific init stages | May need to reflash vendor_boot with a minimal or empty ramdisk |

---

## Phase N2 — Init System: Native Mode Bring-Up

### Goal
Make the OHOS init (`init_early` → `init`) boot successfully as PID 1 on bare metal, performing all the operations currently skipped by `InContainerMode()`.

### Background
Phase 1 of the containerized work patched 27 call sites in `base/startup/init/` to skip operations incompatible with LXC: `MountBasicFs`, `CreateDeviceNode`, `chmod`/`chown`, SELinux, `insmod`, `settimeofday`, and the entire first-stage `SystemPrepare()`. For native boot, all of these must execute.

### Tasks

**N2.1 — Verify InContainerMode() returns 0 on native boot**
- `InContainerMode()` checks the `container` environment variable. On native boot, this is unset → returns 0. Confirm no other code path sets it.
- All 27 guarded blocks should execute their native paths. Audit each to ensure correctness on the Volla X23.

**N2.2 — First-stage init: MountBasicFs on Volla X23**
- `MountBasicFs()` (`init_firststage.c` → `device.c:31-77`) mounts `/dev` (tmpfs), `/proc`, `/sys`, `/sys/fs/selinux`, `/dev/pts`, `/mnt`, `/storage`
- These are standard Linux operations that should work on bare metal. Verify no MediaTek-specific kernel quirks block them.

**N2.3 — First-stage init: MountRequiredPartitions**
- After `MountBasicFs`, `SystemPrepare()` calls `MountRequiredPartitions()` to mount `/system`, `/vendor`, `/data` using the fstab
- This is the critical step that connects the ramdisk to the on-disk filesystem
- Must parse `fstab.volla_x23` (Phase N3) and mount the correct block devices

**N2.4 — Second-stage init: StartSecondStageInit**
- First-stage calls `exec()` to start second-stage init (`/system/bin/init --second-stage`)
- Second-stage loads service configs (`.cfg` files), starts `samgr`, `hilogd`, `render_service`, etc.
- The `InContainerMode()` guards in second-stage paths (sandbox, SELinux, capabilities) should automatically take the native path

**N2.5 — SELinux strategy**
- Current: `build_selinux=false` compiles out all SELinux enforcement
- For native boot, this is acceptable initially but eventually SELinux should be enabled for security
- **Phase N2 keeps SELinux disabled** (same as containerized); a future phase enables it

**N2.6 — ueventd / device node management**
- In the container, `lxc.autodev=1` creates `/dev`. Natively, OHOS `ueventd` must run.
- Verify `ueventd` is included in the ramdisk and starts from init cfg
- Add Volla X23-specific ueventd rules for: `/dev/mali0`, `/dev/dri/*`, `/dev/dma_heap/*`, `/dev/input/*`, `/dev/binder`, `/dev/hwbinder`, `/dev/vndbinder`, `/dev/access_token_id`

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| **Kernel panics during early init** | No serial console output on Volla X23 | Use `pstore` / `last_kmsg` / `ramoops` to capture logs from failed boots; add `console=ttyMSM0,115200` if UART is available |
| **MountRequiredPartitions fails** | Wrong block device paths or filesystem types | Pre-validate by running `blkid` on the device; ensure fstab matches actual partition UUIDs or by-partlabel paths |
| **ueventd rules missing** | Device nodes not created, services fail | Start with permissive rules (`/dev/* 0666`), tighten later |

---

## Phase N3 — Filesystem & fstab

### Goal
Define the complete filesystem layout and mount table for OHOS running natively on the Volla X23.

### Tasks

**N3.1 — Create fstab.volla_x23**
- Location: `device/board/oniro/hybris_generic/cfg/fstab.volla_x23`
- Map block devices to mount points:

```
# <device>                              <mount>      <type>  <flags>                          <fs_mgr>
/dev/block/by-partlabel/system${slot}   /            ext4    ro,barrier=1                     wait,first_stage_mount
/dev/block/by-partlabel/vendor${slot}   /vendor      ext4    ro,barrier=1                     wait,first_stage_mount
/dev/block/by-partlabel/userdata        /data        ext4    nosuid,nodev,noatime,discard     wait,check,fileencryption=software,quota
```

- Note: If using dynamic partitions inside `super`, the paths will be `/dev/block/mapper/system_a` etc. (device-mapper logical volumes). First-stage init handles `dm-verity` / `dm-linear` via `fs_mgr`.
- Include `/dev/__properties__` tmpfs mount for the OHOS property system

**N3.2 — Root filesystem layout**
- The OHOS system image becomes the root filesystem (`/`):
  - `/system/bin/`, `/system/lib64/`, `/system/etc/` — standard OHOS paths
  - `/vendor/` — OHOS vendor partition (separate image)
  - `/data/` — user data partition
  - `/android/` — mount point for Android rootfs (Phase N5)
  - `/config/` — configfs mount point (for USB gadget)
  - `/dev/binderfs/` — binderfs mount point

**N3.3 — Tmpfs and pseudo-filesystem mounts**
- Add to init cfg (post-`MountBasicFs`):
  - `mount binderfs binderfs /dev/binderfs` — for binder device management
  - `mount configfs none /config nodev noexec nosuid` — for USB gadget (HDC)
  - `mount tmpfs tmpfs /dev/__properties__` — OHOS property storage

**N3.4 — Android rootfs mount**
- If using squashfs (Option A from N1.5):
  ```
  mount -t squashfs -o ro,loop /android.img /android
  ```
- If using directory (Option B):
  - Already present in system image at `/android/system/`, `/android/vendor/`

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| **Dynamic partition device-mapper paths** | Standard `by-partlabel` paths don't work for logical volumes | Use `dmsetup` or rely on OHOS `fs_mgr` to parse the super partition metadata |
| **File encryption (FBE)** | `/data` partition may have existing Android FBE metadata | Format `/data` fresh on first OHOS boot; user loses Android data (expected) |
| **Slot suffix resolution** | A/B slot selection usually handled by bootloader properties | Parse `androidboot.slot_suffix` from kernel cmdline in init |

---

## Phase N4 — LXC Inside OpenHarmony

### Goal
Build and integrate LXC userspace tools into the OHOS system image so OHOS can manage an Android guest container.

### Background
Currently LXC is provided by the Ubuntu Touch host. For native boot, OHOS must carry its own LXC tools. LXC is a relatively simple C project with minimal dependencies (primarily `libcap`, `libseccomp`, and `libselinux` — the latter two can be disabled).

### Tasks

**N4.1 — Port LXC to the OHOS build system**
- Add `third_party/lxc/` with LXC source (version 5.x or 6.x)
- Create `BUILD.gn` targeting:
  - `lxc-start`, `lxc-stop`, `lxc-attach`, `lxc-info`, `lxc-create`, `lxc-destroy` — binaries
  - `liblxc.z.so` — shared library
- Configure build: `--disable-selinux --disable-seccomp --disable-apparmor` (for initial bring-up)
- Link against OHOS musl libc, `libcap` (already in OHOS tree)
- Install to `/system/bin/` and `/system/lib64/`

**N4.2 — LXC runtime directories**
- Create at boot (via init cfg):
  - `/var/lib/lxc/` — container storage
  - `/var/run/lxc/` — runtime state
  - `/var/log/lxc/` — logs
- These should be on a writable filesystem (tmpfs or `/data/lxc/`)

**N4.3 — Kernel requirements for hosting containers**
- Verify the Volla X23 kernel config has:
  - `CONFIG_NAMESPACES=y`, `CONFIG_USER_NS=y`, `CONFIG_PID_NS=y`, `CONFIG_NET_NS=y`, `CONFIG_UTS_NS=y`, `CONFIG_IPC_NS=y`
  - `CONFIG_CGROUPS=y`, `CONFIG_CGROUP_DEVICE=y`
  - `CONFIG_VETH=y` (for container networking, if needed)
  - `CONFIG_BRIDGE=y` (if network bridging is used)
- Most should already be enabled for the Halium/OHOS kernel; verify and add any missing options to `openharmony.config`

**N4.4 — LXC init service**
- Create an OHOS init service cfg (`/system/etc/init/android_container.cfg`) that:
  1. Creates binder devices (Phase N6)
  2. Mounts the Android rootfs
  3. Starts the Android container: `lxc-start -n android -F`
  4. Waits for `hwservicemanager` to register
  5. Starts the Android HWC2 composer service
- This service must start **before** `render_service` and `composer_host`

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| **LXC build against musl libc** | LXC assumes glibc features (e.g., `fexecve`, `signalfd`) | OHOS musl is extended; patch as needed — Halium/postmarketOS have musl LXC builds as reference |
| **Capability restrictions** | OHOS init may drop capabilities before LXC needs them | Ensure the `android_container` service runs with `CAP_SYS_ADMIN`, `CAP_NET_ADMIN`, `CAP_MKNOD` |
| **cgroup v2 only** | If the kernel uses cgroup2 exclusively, LXC must use the cgroupv2 driver | LXC 5.x+ supports cgroup2 natively |

---

## Phase N5 — Android Container (Guest)

### Goal
Configure and run a minimal Android userspace as an LXC guest inside OHOS, providing the HAL services needed by libhybris.

### Background
The current Android container (`/var/lib/lxc/android/`) runs a Halium 12 Android userspace with:
- `hwservicemanager` — HIDL service registry
- `servicemanager` — binder context manager
- `vndservicemanager` — vendor binder context manager
- `android.hardware.graphics.composer@2.1-service` — HWC2
- `android.hardware.graphics.allocator@4.0-service-mediatek` — gralloc allocator

### Tasks

**N5.1 — Extract the Android rootfs**
- From the existing Ubuntu Touch installation, archive the Android container filesystem:
  ```
  tar -czf android-rootfs.tar.gz -C /var/lib/lxc/android/rootfs .
  ```
- Alternatively, extract from the stock Volla X23 firmware (system.img + vendor.img)
- Package as a squashfs image or directory tree within the OHOS system image

**N5.2 — Create the Android container LXC config**
- Location: `/data/lxc/android/config` (or `/system/etc/lxc/android/config`)
- Key settings:
  ```
  lxc.rootfs.path = /data/android/rootfs    # or loop-mounted squashfs
  lxc.init.cmd = /init
  lxc.uts.name = android

  # Share IPC namespace with OHOS host for hwbinder communication
  lxc.namespace.share.ipc = 1
  # The PID 1 process of the host (OHOS init)

  # Autodev: let Android create its own /dev (as in current setup)
  lxc.autodev = 0

  # Android property system
  lxc.mount.entry = tmpfs dev/__properties__ tmpfs rw,nosuid,nodev,noexec 0 0

  # Bind-mount binder devices from host binderfs
  lxc.mount.entry = /dev/binderfs/binder    dev/binder    bind bind,create=file 0 0
  lxc.mount.entry = /dev/binderfs/hwbinder  dev/hwbinder  bind bind,create=file 0 0
  lxc.mount.entry = /dev/binderfs/vndbinder dev/vndbinder bind bind,create=file 0 0

  # GPU access for composer/allocator
  lxc.mount.entry = /dev/mali0    dev/mali0    bind bind,create=file 0 0
  lxc.mount.entry = /dev/dri      dev/dri      rbind bind,create=dir 0 0
  lxc.mount.entry = /dev/dma_heap dev/dma_heap bind bind,create=dir 0 0
  ```

**N5.3 — Minimize the Android init**
- The Android container does NOT need a full Android boot. Strip to:
  - `init` (Android's init, for property service and service management)
  - `hwservicemanager`
  - `servicemanager`, `vndservicemanager`
  - `android.hardware.graphics.composer@2.1-service`
  - `android.hardware.graphics.allocator@4.0-service-mediatek`
  - Optionally: `android.hardware.sensors@2.1-service-mediatek` (for future Phase N9)
- Disable everything else in `init.rc` to speed up container boot

**N5.4 — Shared IPC namespace configuration**
- The critical requirement: OHOS and Android must share the IPC namespace so hwbinder HIDL calls work cross-container
- Current approach: `lxc.namespace.keep = ipc` on the OHOS container (inherits host IPC)
- New approach: `lxc.namespace.share.ipc` on the Android container (inherits OHOS host IPC)
- This means both see the same `/dev/hwbinder` and `hwservicemanager` registrations are globally visible

**N5.5 — Android property system isolation**
- Android services read properties from `/dev/__properties__`. In the containerized setup, this is bind-mounted from the host.
- For native boot, Android must have its own property area:
  - Let Android init create `/dev/__properties__` inside the container (via tmpfs mount in LXC config)
  - Pre-seed with required properties: `ro.hardware.egl=mali`, `ro.board.platform=mt6789`, `ro.hardware.vulkan=mali`, etc.
- OHOS has its own property system (parameterized via `SystemWriteParam`/`SystemReadParam`) — no conflict

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| **Android init expects full hardware** | Android init tries to mount partitions, load firmware, configure networking | Use a minimal `init.rc` that only starts HAL services; set `androidboot.hardware=mt6789` in container env |
| **SELinux in Android container** | Android enforcing mode blocks operations without proper contexts | Pass `selinux=0` or `androidboot.selinux=permissive` in the container's kernel cmdline (faked via `/proc/cmdline` bind mount or init.rc override) |
| **hwservicemanager registration timing** | OHOS services start before Android HALs register | The OHOS `android_container` init service (N4.4) must gate `render_service` startup on HAL availability |

---

## Phase N6 — Binder Device Management

### Goal
Create and manage isolated binder devices for both OHOS and Android on the same host, preventing context manager collisions.

### Background
The current setup creates `ohos-binder`, `ohos-hwbinder`, `ohos-vndbinder` via a Python script using `BINDER_CTL_ADD` ioctl on `/dev/binderfs/binder-control`. For native boot, this logic moves into OHOS init.

### Tasks

**N6.1 — Mount binderfs from OHOS init**
- Add to first-stage or early second-stage init:
  ```
  mkdir /dev/binderfs
  mount -t binder binder /dev/binderfs
  ```
- This creates `/dev/binderfs/binder-control` and the default `binder`, `hwbinder`, `vndbinder` devices

**N6.2 — Assign binder devices to OHOS and Android**
- **OHOS uses the default devices**: `/dev/binderfs/binder` (symlinked to `/dev/binder`), `/dev/binderfs/hwbinder`, `/dev/binderfs/vndbinder`
  - OHOS `samgr` registers as context manager on `/dev/binder` (no collision since Android isn't using it yet)
- **Android gets the same hwbinder/vndbinder** (shared IPC namespace) but its own `/dev/binder`:
  - Create `/dev/binderfs/android-binder` via `BINDER_CTL_ADD`
  - Bind-mount into Android container as `/dev/binder`
  - Android's `servicemanager` registers on this device
- **Reversal from current setup**: Currently OHOS gets dedicated devices; now Android does. The principle is the same — whoever boots second gets the dedicated device.

**N6.3 — Implement binder device creation in C (replace Python script)**
- Port `create-ohos-binder-devices.py` to a small C utility or directly into an init plugin:
  ```c
  int fd = open("/dev/binderfs/binder-control", O_RDWR);
  struct binderfs_device dev = { .name = "android-binder" };
  ioctl(fd, BINDER_CTL_ADD, &dev);
  ```
- Run before the Android container starts

**N6.4 — Symlink management**
- Create convenience symlinks in init:
  - `/dev/binder` → `/dev/binderfs/binder`
  - `/dev/hwbinder` → `/dev/binderfs/hwbinder`
  - `/dev/vndbinder` → `/dev/binderfs/vndbinder`

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| **binderfs not in kernel config** | If `CONFIG_ANDROID_BINDERFS` is not enabled | Already enabled for current setup; verify in `openharmony.config` |
| **Context manager race** | OHOS samgr and Android servicemanager both try to register on `/dev/binder` | Sequence guarantees: OHOS samgr starts first on default `/dev/binder`; Android container starts later with its own device |

---

## Phase N7 — HDC over USB

### Goal
Enable `hdcd` (HarmonyOS Device Connector daemon) for USB-based device communication, replacing ADB.

### Background
The HDC source is at `developtools/hdc/`. USB connectivity requires ConfigFS gadget setup and FunctionFS endpoints. The init system already has USB gadget configuration in `init.usb.configfs.cfg` and `init.usb.cfg`.

### Tasks

**N7.1 — Verify USB controller on Volla X23**
- Identify the USB device controller: check `/sys/class/udc/` on the device
- MT6789 typically uses `musb-hdrc` or `mtu3` (MediaTek USB 3.0 DRD controller)
- Set `sys.usb.controller` parameter to the correct controller name

**N7.2 — ConfigFS gadget setup**
- The existing `init.usb.configfs.cfg` creates the USB gadget at `/config/usb_gadget/g1/`
- Verify or add HDC function directory creation:
  ```
  mkdir /config/usb_gadget/g1/functions/ffs.hdc
  ```
- Set VID/PID: `idVendor=0x12D1` (or Oniro-specific), `idProduct=0x5000` (HDC mode)
- Create FunctionFS mount:
  ```
  mkdir /dev/usb-ffs/hdc
  mount -t functionfs hdc /dev/usb-ffs/hdc
  ```

**N7.3 — hdcd service configuration**
- Ensure `hdcd.cfg` is present in `/system/etc/init/`:
  ```json
  {
      "services": [{
          "name": "hdcd",
          "path": ["/system/bin/hdcd"],
          "uid": "root",
          "gid": ["root", "shell"],
          "caps": ["SYS_PTRACE", "KILL", "DAC_OVERRIDE", "NET_ADMIN"],
          "start-mode": "condition",
          "importance": -20
      }]
  }
  ```
- Trigger on `sys.usb.ffs.ready.hdc=1`

**N7.4 — USB mode selection at boot**
- Set default USB mode to HDC in init:
  ```
  setparam sys.usb.configfs 1
  setparam sys.usb.config hdc
  ```
- This triggers the `init.usb.configfs.cfg` job that writes to the gadget and starts `hdcd`

**N7.5 — Verify host-side tooling**
- Ensure `hdc` host binary is built and can connect to the device:
  ```
  hdc list targets    # Should show the device
  hdc shell           # Should get a shell
  hdc file send/recv  # File transfer
  ```

**N7.6 — Network HDC (fallback during development)**
- During early bring-up when USB may not work, enable HDC over TCP:
  - `setparam persist.hdc.mode.tcp enable`
  - `setparam persist.hdc.port 8710`
- Requires network connectivity (see Phase N9) or a USB RNDIS gadget for IP-over-USB

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| **USB controller driver not loaded** | MediaTek USB DRD driver may need specific DT configuration | Check if `mtu3` module is built; add to kernel config if needed |
| **FunctionFS not available** | `CONFIG_USB_FUNCTIONFS` may be missing | Add to kernel config; it is standard for Android kernels |
| **No debug access during early boot** | If HDC doesn't come up, there's no way to debug | Phase N10 (recovery) must be in place; also consider UART/serial as emergency debug path |
| **ConfigFS gadget permissions** | Init must have permission to write to `/config/usb_gadget/` | Runs as root (PID 1); no issue |

---

## Phase N8 — Graphics & Display (Native)

### Goal
Bring up the display stack on native boot, reusing the existing libhybris VDI implementations.

### Background
The display stack (Phase 6, Phase 8) is already functional in the containerized setup. The key components are:
- `libdisplay_composer_vdi_impl.z.so` — wraps `libhybris-hwc2` for display composition
- `libdisplay_buffer_vdi_impl.z.so` — wraps `libhybris-gralloc` for buffer allocation
- EGL impl symlinks pointing to libhybris EGL → `libGLES_mali.so`
- All the Phase 8 stability fixes (sticky CLIENT layers, pre-validate override, etc.)

### Tasks

**N8.1 — Library path adjustment**
- In the containerized setup, Android libraries live under `/android/system/`, `/android/vendor/` because the host's `/system/` and `/vendor/` are Android's
- For native boot, `/system/` and `/vendor/` are OHOS. Android libraries are at `/android/system/`, `/android/vendor/` (mounted from the Android rootfs)
- The `HYBRIS_LD_LIBRARY_PATH` and libhybris Q linker path remapping should continue to work as-is, since they already use `/android/` prefixed paths

**N8.2 — EGL/GLES symlink setup**
- Ensure the following symlinks exist in the OHOS system image:
  - `/system/lib64/libEGL_impl.so` → `libEGL.z.so` (libhybris EGL)
  - `/system/lib64/libGLESv2_impl.so` → `libGLESv2.z.so`
  - `/system/lib64/libGLESv1_CM_impl.so` → `libGLESv1_CM.z.so`
- `/vendor/lib64/egl/` and `/vendor/lib64/hw/` must be bind-mounted or symlinked from the Android rootfs:
  - In native boot, use mount entries in init cfg instead of LXC mount entries

**N8.3 — Environment variables for render_service**
- Same as containerized: `HYBRIS_EGLPLATFORM=ohos`, `LIBEGL`, `LIBGLESV2`, etc.
- Already configured via `hybris_graphic_env.cfg` — this carries over unchanged

**N8.4 — Android HWC2 service dependency**
- `render_service` → `composer_host` → VDI → libhybris-hwc2 → hwbinder → Android `composer@2.1-service`
- The init service ordering (Phase N4.4) must ensure Android container is fully booted and HWC2 registered before `render_service` starts
- Add a readiness check: poll `hwservicemanager` for `android.hardware.graphics.composer@2.1::IComposer/default` registration

**N8.5 — DRI/GPU device access**
- On native boot, `/dev/mali0`, `/dev/dri/*`, `/dev/dma_heap/*` are created by ueventd
- No bind mounts needed — direct access
- Verify permissions: `render_service` runs as `graphic` (UID 1003); ensure ueventd rules grant access

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| **GPU driver initialization order** | Mali driver must be loaded before render_service | Ensure kernel module is built-in or loaded by ueventd early |
| **Android libraries not at expected paths** | libhybris hardcodes some paths like `/vendor/lib64/egl/` | Maintain bind mounts from `/android/vendor/lib64/egl/` to `/vendor/lib64/egl/` (same as containerized approach) |
| **Composition bugs from Phase 8** | All existing Phase 8 fixes must be present | These are in the VDI source code, not container-specific — they carry over |

---

## Phase N9 — Firmware, Peripherals & Connectivity

### Goal
Ensure all hardware peripherals work under native OHOS boot.

### Background
In the containerized setup, Ubuntu Touch manages firmware loading, modem, WiFi, Bluetooth, audio, sensors, and power management. Removing Ubuntu Touch means OHOS or the Android container must handle these.

### Tasks

**N9.1 — Firmware loading**
- The Linux kernel loads firmware from `/lib/firmware/` (or `/vendor/firmware/`)
- Identify all firmware blobs used by the Volla X23:
  - WiFi: MediaTek MT7663 or similar (`/vendor/firmware/`)
  - Bluetooth: MediaTek BT firmware
  - GPU: Mali firmware (usually built into the driver)
  - Modem: MediaTek CCCI firmware
- Package firmware into the OHOS system image at `/vendor/firmware/` or `/lib/firmware/`
- Set the kernel firmware search path: `firmware_class.path=/vendor/firmware`

**N9.2 — WiFi**
- Options:
  - **Option A:** Run WiFi HAL in Android container, use `wpa_supplicant` from Android
  - **Option B:** Use OHOS softbus/WiFi management with the kernel driver directly
- Initially, Option A is simpler since the Android WiFi HAL (`android.hardware.wifi@1.0-service-lazy-mediatek`) already works
- Requires: WiFi kernel module loaded, firmware available, `wpa_supplicant` running in Android container

**N9.3 — Modem / Telephony**
- The MediaTek CCCI (Cross Core Communication Interface) driver manages modem communication
- RIL (Radio Interface Layer) runs in Android — `rild` or `vendor.mediatek.hardware.mtkradioex@1.0-service`
- For initial bring-up, telephony is not required; skip and add later
- Long term: run the telephony HAL in the Android container, bridge to OHOS via a telephony VDI

**N9.4 — Bluetooth**
- Similar to WiFi: run BT HAL in Android container or use kernel HCI directly
- `android.hardware.bluetooth@1.0-service-mediatek` in Android container is the path of least resistance

**N9.5 — Audio**
- `android.hardware.audio.service.mediatek` provides AudioHAL
- Could run in Android container and bridge via libhybris (similar to graphics)
- Or use OHOS distributed audio with ALSA directly — more work but avoids Android dependency

**N9.6 — Sensors**
- `android.hardware.sensors@2.1-service-mediatek` in Android container
- Bridge to OHOS multimodal input or sensor framework

**N9.7 — Power management**
- CPU frequency scaling: kernel handles via `cpufreq` governors
- Suspend/resume: OHOS power manager must interact with `/sys/power/state`
- Battery monitoring: `/sys/class/power_supply/`
- Charging: MediaTek charger driver (kernel)
- These mostly work through sysfs and don't depend on Ubuntu Touch

**N9.8 — Camera**
- Complex; MediaTek camera HAL is deeply integrated with Android
- `android.hardware.camera.provider@2.6-service-mediatek`
- Run in Android container; bridge to OHOS camera framework via VDI (future work)

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| **Firmware path differences** | Kernel expects firmware at paths from the Android build | Symlink or copy to expected paths; set `firmware_class.path` |
| **Android HAL service proliferation** | Running many Android HALs increases container complexity | Start minimal (graphics only), add peripherals one at a time |
| **Modem initialization sequence** | CCCI driver may need specific init timing or properties | Defer telephony; focus on WiFi and display first |
| **Suspend/resume stability** | Power management regressions when switching from Ubuntu Touch | Test incrementally; keep `wake_lock` held during development |

---

## Phase N10 — Flash Tooling & Recovery

### Goal
Create a safe flash procedure and recovery mechanism so a failed boot doesn't brick the device.

### Critical: This phase runs in parallel with all others.

### Tasks

**N10.1 — Establish the flash procedure**
- Use `fastboot` (or `mtkclient` for MediaTek) to flash images:
  ```bash
  fastboot flash boot boot.img
  fastboot flash dtbo dtbo.img
  fastboot flash vendor_boot vendor_boot.img
  # For dynamic partitions inside super:
  fastboot flash system system.img
  fastboot flash vendor vendor.img
  fastboot -w   # wipe userdata (first flash only)
  fastboot reboot
  ```
- If MediaTek secure download mode is needed, use `mtkclient` (Python tool for MediaTek BROM/preloader)

**N10.2 — A/B slot strategy**
- Flash OHOS to the **inactive slot** (e.g., `_b` if Ubuntu Touch is on `_a`)
- This preserves Ubuntu Touch on the other slot as a fallback
- Switch active slot: `fastboot set_active b`
- If OHOS fails to boot, switch back: hold volume-down during boot → fastboot mode → `fastboot set_active a`

**N10.3 — Recovery image**
- Build an OHOS updater/recovery image or reuse the stock Android recovery
- The recovery image should support:
  - `fastboot` mode (for reflashing)
  - ADB sideload (for pushing new images)
  - Factory reset (wipe `/data`)

**N10.4 — Serial/UART debug (if available)**
- Investigate if the Volla X23 exposes a UART debug port (usually via test pads on the PCB or via the USB port in a special mode)
- MediaTek devices sometimes expose UART over the headphone jack or USB with special cables
- Having serial console access is invaluable for debugging early boot failures

**N10.5 — pstore / ramoops for crash logs**
- Ensure `CONFIG_PSTORE=y` and `CONFIG_PSTORE_RAM=y` in the kernel
- Configure a reserved memory region for ramoops in the device tree
- After a failed boot, logs are available at `/sys/fs/pstore/` on the next boot

**N10.6 — Create a unified flash script**
- `device/board/oniro/hybris_generic/utils/flash-native.sh`:
  - Builds all images (or uses pre-built)
  - Flashes to inactive slot
  - Sets active slot
  - Reboots
  - Validates boot (waits for HDC connection)

### Obstacles & Mitigations
| Obstacle | Risk | Mitigation |
|----------|------|------------|
| **Bootloop / brick** | Bad boot.img prevents any access | A/B slot strategy (N10.2) is the primary safety net; always flash to inactive slot |
| **MediaTek download mode** | If bootloader is corrupted, need BROM access | Document the BROM key combo for the Volla X23; keep `mtkclient` ready |
| **No fastboot on Volla X23** | Some Halium devices don't expose fastboot properly | Test `fastboot devices` before starting; if unavailable, use `mtkclient` or `dd` from recovery |

---

## Implementation Order & Milestones

### Milestone 1: Minimal Boot to Shell (N1 + N2 + N3 + N7 + N10)
- OHOS boots to init, mounts filesystems, starts hilogd
- HDC works over USB — can get a shell
- No graphics, no Android container
- **This proves the boot chain works end-to-end**

### Milestone 2: Android Container Running (N4 + N5 + N6)
- LXC starts Android container inside OHOS
- `hwservicemanager` and `servicemanager` are running
- Binder IPC works cross-container
- **This proves the role reversal works**

### Milestone 3: Display Working (N8)
- `render_service` connects to Android HWC2 via libhybris
- Boot animation plays on physical display
- Launcher visible
- **This proves the full graphics stack works natively**

### Milestone 4: Connectivity (N9)
- WiFi operational
- Bluetooth operational
- Audio playback works
- **The device is usable as a daily driver prototype**

---

## Risk Summary

| Risk | Severity | Likelihood | Phase | Notes |
|------|----------|------------|-------|-------|
| Super partition too small for OHOS + Android | High | Medium | N1 | May need to strip Android rootfs or use a different partitioning scheme |
| First-stage init failure with no debug access | High | Medium | N2 | Mitigated by A/B slot strategy and pstore |
| LXC build against musl fails | Medium | Medium | N4 | Well-trodden path (Alpine Linux, postmarketOS) |
| Android HAL timing races | Medium | High | N5/N8 | Already encountered in containerized setup; init ordering solves it |
| USB gadget driver issues | Medium | Low | N7 | Standard on Android 12 kernels |
| Missing firmware for WiFi/BT/modem | High | Medium | N9 | Must extract from stock ROM; may have licensing implications |
| Device brick during development | Critical | Low | N10 | A/B slots + BROM access prevent permanent brick |

---

## Open Questions

1. **Super partition layout**: What are the exact sizes of the logical partitions inside `super`? Can they be resized without reflashing the entire super partition?
2. **UART access**: Does the Volla X23 expose a serial debug port? This would significantly de-risk early boot development.
3. **Vendor boot ramdisk**: Does the MediaTek vendor_boot contain first-stage init binaries that conflict with OHOS? May need an empty vendor_boot.
4. **Android rootfs size**: How large is the minimal Android rootfs (HAL services only)? This determines whether it fits alongside OHOS in the super partition.
5. **DM-verity**: Is dm-verity enforced on the system/vendor partitions? If so, images must include verity metadata or verity must be disabled in the kernel cmdline.
