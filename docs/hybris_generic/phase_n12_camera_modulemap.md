# Phase N12.2 — Camera Kernel Module Map (X23, MT6789, ISP6s)

Companion to [phase_n12_camera.md](phase_n12_camera.md) §N12.2 and
[phase_n12_camera_x23_inventory.md](phase_n12_camera_x23_inventory.md).

Defines the dependency-resolved load order for the MTK camera/ISP/sensor
kernel stack under native boot.  Derived from
`kernel/linux/volla-vidofnir/build-dir/tmp/system/lib/modules/5.10.209/modules.dep`
on a freshly-built kernel (2026-05-20), filtered through `modinfo` so
the listed deps are the *direct* link-time dependencies that the
in-kernel module loader resolves at `insmod` time.

## Pre-condition: modules already loaded by other blocks

These are already `insmod`'d by the pre-init lines from N8.11 (GPU),
N9.5 (audio), N9.2 (WiFi/connsys), or are kernel built-ins.  Camera
load must come *after* these blocks, but adds no new `insmod` lines
for them.

```
aee_aed             mrdump                emi             emi-mpu
mtk-smi             mtk-smi-dbg           mtk-icc-core    iommu_debug
iommu_secure        mtk_iommu             mtk-cmdq-drv-ext cmdq_helper_inf
vcp_status          mmprofile             irq-dbg         device-apc-common
system_heap         clk-common            clk-mt6789-cam
videodev (builtin)  v4l2-mem2mem (builtin) videobuf2-v4l2 (builtin)
```

## Empirical load behaviour (2026-05-20)

Tested by manually `hdc file send`ing each `.ko` to `/data/local/tmp/`
and `insmod`ing one-at-a-time on a running native-boot device.  Result:

- **Safe (loads cleanly):** TEE chain (`gz_trusty_mod`, `gz_ipc_mod`,
  `mcDrvModule` already present from `rpmb_mtk`, `gz_tz_system`,
  `iommu_gz`, `trusted_mem`, `mtk_sec_heap`); ISP core +
  imgsensor framework (`archcounter_timesync`, `v4l2-fwnode`,
  `imgsensor-glue`, `mtk_ccu`, `imgsensor`, `imgsensor_isp6s`,
  `cam_qos`, `camera_isp`, `camera_mem`); helpers (`camera_eeprom`,
  `camera_af_media`, `mtk_jpeg`).  → 18 modules.  After loading:
  - `/dev/video0` appears (`mtk-jpeg-enc`)
  - `/dev/camera_eeprom{0,1,2}` from `camera_eeprom.ko` (the 3
    eeprom i2c devices DO bind to the eeprom driver)
  - `/dev/camera-isp`, `/dev/camera-mem` from `camera_isp.ko` /
    `camera_mem.ko`
  - 3 new platform drivers register: `image_sensor`, `seninf`,
    `seninf_n3d` (from `imgsensor_isp6s.ko`); `seninf` + `seninf_n3d`
    bind to `1a004000.seninf_top` / `1a004000.seninf_n3d_top`
  - **But no `/dev/kd_camera_hw`, no `/dev/v4l-subdev*`, no
    `/dev/media*`** — the camera-sensor i2c devices
    (`mediatek,camera_main` @ 8-001a, `mediatek,camera_main_two`
    @ 4-0010, `mediatek,camera_sub` @ 4-001a) DO NOT bind to
    `imgsensor.ko` (which only matches `mediatek,imgsensor`).
    `image_sensor` platform driver also doesn't auto-bind to
    `soc:kd_camera_hw1@1a004000` even though that node has
    `mediatek,imgsensor` compatible and `waiting_for_supplier=0`.
    Manual `echo soc:... > /sys/bus/platform/drivers/image_sensor/bind`
    is accepted but produces no probe activity (no dmesg, no
    `/dev/kd_camera_hw`).  See "Sensor binding blocker" below.
- **Triggers kernel watchdog reboot (~30 s after insmod):**
  `camera_dip_isp6s.ko`.  insmod returns "rc=0" but the calling
  shell + hdcd die a few seconds later; pstore ramoops is empty
  (no panic — the watchdog rebooted while CPUs were spinning).
  Almost certainly the DIP block's `probe()` touches a register
  while the camera power-domain is OFF, locking the SoC.  On a stock
  Halium boot, modprobe loads it later — *after* `cameraserver` /
  the HAL has issued a `pm_runtime_get` on the camera platform
  device that turns the domain on.  Native boot has no equivalent
  to wake the domain before probe.
- **Not yet tested (suspected same class as DIP):**
  `camera_dpe_isp60`, `camera_mfb_isp6s`, `camera_rsc_isp60`,
  `camera_wpe_isp6s`, `camera_pda`.  All follow the same
  pattern as DIP (hardware sub-block ISP6s probe).

### Sensor binding blocker — RESOLVED 2026-05-20

After all 18 safe modules are loaded, `dumpsys media.camera` (Android
side) initially reports `Camera Provider HAL internal/0-X (v2.6, remote)
static info: 0 devices`.  **Root cause + fix in two lines:**

> The MTK camera HAL needs `/mnt/vendor/nvcfg` (GPT partition `nvcfg`,
> `/dev/block/by-name/nvcfg`) mounted to find its 3A / sensor
> calibration NV files.  Under native boot, Halium's first-stage init
> never runs the fstab mount.  `androidd`'s parent watchdog now mounts
> it from outside the Halium NS — `dumpsys media.camera` reports
> 3 devices (S5KGM1ST rear, OV16A1Q front, GC08A3WIDE rear-wide).

The fix: see `mount_vendor_nv()` in
`device/board/oniro/hybris_generic/launcher/androidd.c`.  It runs
before the existing composer-ready watchdog poll, retries the
mount up to 16 × 500 ms while Halium init is mounting its tmpfs on
`/mnt`, and exits silently if the partition isn't present.

Detailed investigation history kept below for future reference.

#### Updated findings — full investigation history (2026-05-20)

#### Updated findings (2026-05-20)

The i2c sensor devices **do** bind — but to the legacy MTK chardev
shim, not to the v4l2 framework:

| i2c device | DT node | Driver bound to it |
|---|---|---|
| `i2c-4/4-0010` | `camera_main_two` | `kd_camera_hw_bus3` |
| `i2c-4/4-001a` | `camera_sub` | `kd_camera_hw_bus2` |
| `i2c-8/8-001a` | `camera_main` | `kd_camera_hw` |
| `soc:kd_camera_hw1@1a004000` (platform) | `mediatek,imgsensor` | `image_sensor` |

So the kernel side of sensor bind is fine.  The chardev nodes appear
as expected: `/dev/kd_camera_hw`, `/dev/seninf`, `/dev/seninf_n3d`,
`/dev/camera-isp`, `/dev/camera-mem`, `/dev/camera_eeprom{0,1,2}`,
`/dev/video0` (mtk-jpeg-enc).

`imgsensor.ko` (the new v4l2 framework) — separately — is dormant.
No i2c device has compatible `mediatek,imgsensor`, so the v4l2 driver
binds nothing.  This is the by-design path on Halium 12 + MT6789:
the legacy `imgsensor_isp6s.ko` + `kd_camera_hw*` shim is the
canonical sensor path on this SoC.  The v4l2 driver is harmless
overhead — leave it loaded; revisit if a future cleanup removes it.

#### Probe-time printks — informational, not root cause

Kernel printks captured at module-load time (via `logcat -d` inside
the Halium namespace, immediately after `insmod imgsensor_isp6s.ko`):

```
image_sensor soc:kd_camera_hw1@1a004000: there is not valid maps for state default
probe of soc:kd_camera_hw1@1a004000 returned 1 after 3811 usecs
SeninfN3D[n3d_clk_init] cannot get 0 clock, skip
SeninfN3D[n3d_clk_init] cannot get 1 clock, skip
probe of 1a004000.seninf_top returned 1 after 742 usecs
probe of 1a004000.seninf_n3d_top returned 1 after 511 usecs
```

These look alarming but are **informational**, not errors:

- "*there is not valid maps for state default*" — `dev_info` from
  `drivers/pinctrl/devicetree.c:174`, fires for any node with no
  `pinctrl-0`/`pinctrl-names`.  Most MTK SoC nodes (including this
  one upstream) don't declare pinctrl; the pin muxing is handled
  by board-level overlays.
- "*cannot get N clock, skip*" — `LOG_PR_ERR` (but the function
  returns 0 — it just prints).  Indexes 0 (SCP_SYS_MDP) and 1
  (SCP_SYS_CAM) are not declared in MT6789's DT (the driver is
  generic and supports SoCs that have them).  The 4 clocks MT6789
  *does* declare (`CAMSYS_SENINF_CGPDN`, `CAMSYS_CAM_CGPDN`,
  `CAMSYS_CAMTG_CGPDN`, `CAMSYS_CAMTM_SEL`) all bind cleanly.
- "*probe returned 1*" — MTK driver convention for "attached but
  some optional sub-resource missing"; the driver continues.

The seninf, seninf_n3d, and image_sensor drivers all successfully
bind and create their chardev nodes — confirmed by the post-boot
state of `/sys/bus/{i2c,platform}/devices/`.

#### Strace of camerahalserver — what enumeration actually does

`strace -y -f -e trace=openat,ioctl,read,write,close` against a
manually-respawned camerahalserver (inside the Halium NS) caught
the full enumeration sequence on a fresh boot.  Highlights:

```
openat("/dev/seninf",                 O_RDWR) = 8
openat("/dev/kd_camera_hw",           O_RDWR) = 9
openat("/dev/camera_eeprom0",         O_RDWR) = 8
openat("/dev/camera_eeprom1",         O_RDWR) = 8
openat("/dev/camera_eeprom2",         O_RDWR) = 8
ioctl(9</dev/kd_camera_hw>, _IOC(WR, 0x69, 0xf,  0x18), …) = 0   x 438
ioctl(9</dev/kd_camera_hw>, _IOC(WR, 0x69, 0x41, 0x18), …) = 0   x   3
ioctl(8</dev/seninf>,       _IOC(WR, 0x73, 0x28, 0x18), …) = 0   x   8
ioctl(8</dev/seninf>,       _IOC(WR, 0x73, 0x3c, 0xc),  …) = 0   x  12
openat("mnt/vendor/nvcfg/cctNvramFile/nv_version_main",    -1 ENOENT
openat("mnt/vendor/nvcfg/cctNvramFile/nv_version_Back1",   -1 ENOENT
openat("/dev/ion",                                          -1 ENOENT
```

IOCTL `0x69`/15 = `KDIMGSENSORIOC_X_FEATURECONCTROL` (yes spelled
that way in `inc/kd_imgsensor.h:44`); `0x69`/0x41 = `_X_GETINFO2`.
The HAL hammers FEATURECONCTROL across slots querying sensor
identity, AE/AF/AWB ability, etc.  Every call returns 0 — no
failing ioctls.

Logcat shows the actual sensor IDs the kernel reads back over i2c:

```
CAM_CUS_MSDK [compareSensorIdAndModuleId] SID F8D1   is found  (S5KGM1ST 12 MP)
CAM_CUS_MSDK [compareSensorIdAndModuleId] SID 561642 is found  (OV16A1Q  16 MP)
CAM_CUS_MSDK [compareSensorIdAndModuleId] SID 8A5    is found  (GC08A3W   8 MP)
```

So the kernel side enumerates correctly.  But the HAL still ends
with `dumpsys media.camera → 0 devices`, because immediately before
the sensor-ID compares, it tries to load the NV calibration:

```
nvbuf_util_dep: readVerNvramNoLock:228  Try to read nv_version_front1 but not exist
MtkCam/HalSensor: get_boot_mode fail to open: /sys/class/BOOT/BOOT/boot/boot_mode
```

The NV-calibration miss is fatal for enumeration even though the
sensor i2c probe succeeded.  The HAL refuses to expose sensors it
can't tune.  (`/sys/class/BOOT/BOOT/boot/boot_mode` miss is
harmless — printed multiple times throughout, never blocks anything.)

#### Why the NV path is missing

Halium's `fstab.mt6789` declares five NV-related partitions:

| Block dev (X23 mapping)  | Mountpoint               | Used by             |
|--------------------------|--------------------------|---------------------|
| sdc6 → `by-name/nvcfg`   | `/mnt/vendor/nvcfg`      | **Camera HAL (3A)** |
| sdc7 → `by-name/nvdata`  | `/mnt/vendor/nvdata`     | (auto-mounted)      |
| sdc16 → `by-name/protect1` | `/mnt/vendor/protect_f`| keymaster           |
| sdc17 → `by-name/protect2` | `/mnt/vendor/protect_s`| keymaster           |
| sdc34 → `by-name/nvram`    | `/nvram` (raw)         | radio / RIL         |

Under native boot, only `nvdata` ends up mounted (legacy by some
init shortcut); the rest don't.  Camera enumeration depends on
`nvcfg`.

#### Fix

Added `mount_vendor_nv()` in `androidd.c` (commit 2026-05-20).  It
runs from the parent watchdog post-clone-of-the-child, uses a
`PARTNAME=`-lookup helper (`find_partition_by_label()`) to resolve
the GPT label `nvcfg` to a raw block device, `setns()`es into the
child's mount NS, and mounts `/dev/block/by-name/nvcfg` (with raw
`/dev/block/sd…` fallback in case Halium ueventd hasn't populated
`by-name` yet) at `/root/mnt/vendor/nvcfg` (Halium's post-pivot
view).  Retries up to 16 × 500 ms because Halium's `mount tmpfs
/mnt` runs early but not instantly.

Verified live: with the fix in place, `dumpsys media.camera`
reports `Number of camera devices: 3` and `lshal debug
android.hardware.camera.provider@2.6::ICameraProvider/internal/0`
lists all three sensors (rear S5KGM1ST + front OV16A1Q + rear-wide
GC08A3WIDE) with correct facing/orientation + flash unit on
cam 0.

#### Not yet mounted: protect_f/_s

`protect1`/`protect2` are also declared in fstab but currently fail
to mount with `EBUSY` — block devices are held open by something
else (probably an OHOS service that grabbed them at boot).  Not a
blocker for camera enumeration (they're for keymaster); revisit if
keymaster-dependent features regress.

#### camerahalserver — startup behaviour

After kernel modules are up + sensors bound, Halium's
`camerahalserver` (PID 254 in Halium namespace, uid `cameraserver`)
runs once at init then sits in `binder_wait_for_work` indefinitely.
Its open file descriptors are *only* `/dev/hwbinder`,
`/dev/vndbinder`, `/dev/pmsg0`, `/sys/kernel/tracing/trace_marker`,
and a couple of sockets — **no camera devices**.  So it probed at
boot, found nothing, and gave up.

This is consistent with the "probe returned 1 / partial init"
hypothesis: the kernel module is loaded and bound, the chardev
exists, but issuing an IOCTL against it returns no sensors detected
(because the MCLK pinctrl is wrong, the sensor doesn't ACK on i2c).

A restart of `camerahalserver` after the DT is fixed would be the
verification: `setprop ctl.restart camerahalserver` inside the
Halium NS, then `dumpsys media.camera`.

#### Next debug steps

1. **Capture stock Halium boot DT.**  Boot the X23 into stock
   Halium (recovery has the original boot/vendor_boot images), then
   `cat /proc/device-tree/soc/kd_camera_hw1@1a004000/...` recursively
   to see the pinctrl + clock properties.  Diff against the same
   path on native boot.  This identifies exactly which DT props are
   missing.
2. **DTB diff.**  `dtc -I dtb -O dts` on the two boot images'
   embedded DTBs (Halium's `vendor_boot.img` vs ours).  Show every
   property added/removed by our build pipeline.
3. **`strace` camerahalserver restart.**  Captures the IOCTL
   sequence + any `read()` returns so we see exactly what the HAL
   thinks the sensor identity is (vs the expected GC8054/etc.).
   Try via `strace -ttt -y -f /vendor/bin/hw/camerahalserver` from
   inside the Halium NS after stopping the service via setprop —
   the daemon respawn doesn't get strace-attached automatically.
4. **Try setprop `vendor.camera.disable.driver = 0`** or similar
   imgsensor-debug knobs.  MTK ships a number of camera-debug
   property gates; some unlock verbose imgsensor logs at next boot.

This blocker does **not** prevent the rest of N12 from proceeding:
the HIDL ICameraProvider is alive, OHOS-side VDI scaffolding (N12.4)
is in place, and the HIDL bridge (N12.5) can connect — the provider
will just return an empty `getCameraIdList()` until the DT gap is
fixed.  N12.2.5 can be debugged in parallel with VDI work.

### Decision (Milestone 1)

Initial cfg ships **only the 17 safe modules** at pre-init.  The
ISP sub-blocks (DIP/DPE/MFB/RSC/WPE/PDA) are *staged into
`vendor_boot` overlay by the build* (kept in `extra-modules.list`)
but **not insmod'd at pre-init**.  We retain them on the device
filesystem at `/mnt/kmodules/` so that:

1. The HAL or a future `androidd`-side helper can `insmod` them
   on demand once the camera power-domain is up.
2. We can A/B test whether basic enumeration / preview works
   without them.

If the HIDL provider can `getCameraIdList()` against only the
17-module set, post-Milestone-1 work picks up the deferred load
via a dedicated runtime-PM-aware service (probably a small
`camera-modload-init` helper modelled on `wmtdetect-init` for
WiFi, but gated on the power-domain being on).

### Update 2026-05-21 — Deferred modules are required, not optional

End-to-end test through the N12.D droidmedia pivot
([phase_n12_camera_droidmedia.md](phase_n12_camera_droidmedia.md))
revealed that **the MTK HAL cannot process capture requests
without the deferred ISP sub-block modules**.  With only the 17
safe modules loaded:

- Enumeration works (`dumpsys media.camera → 3 devices`).
- `ICameraDevice::open` works.
- `configureStreams_3_4` succeeds.
- `processCaptureRequest` is accepted; P1Node starts; sensor is
  configured (e.g. S5KGM1ST → 2000x1500 BAYER10) and powered.
- But on the first frame's downstream processing, the MTK kernel
  drivers log `RscDrv ENQUE_REQ fail (-1)`, `no rsc device`,
  followed by `CamsvStatisticPipe: deque DumpSenDebug fail` and
  `PDOBufMgr: dequeueHwBuf fail`.  9 inflight frames stay
  unfulfilled (`metadata arrived: false` in flushInflightRequests).
- cameraserver times out (`Camera2Client::waitUntilCurrentRequest
  IdLocked: Camera 0: Timed out waiting for current request id to
  return in results!`), the HAL transitions to ERROR_DEVICE, gets
  killed, and respawns.

Hands-on attempts to load the deferred modules at runtime, in
order of escalating effort:

1. **Plain `insmod` while power-domain is off**: triggers SoC
   watchdog reboot ~30 s later (matches the original finding in
   the table above).
2. **Plain `insmod` while camera HAP is open (HAL has called
   `pm_runtime_get`, `dumpsys` shows the camera as CONNECTED)**:
   reboots within ~10 s.  We have a few hundred milliseconds
   between camera-HAP launch and HAL request-time, but a
   poll-and-insmod loop doesn't reliably win the race.
3. **Force `power/control=on` on `/sys/devices/platform/1a000000.
   camisp_legacy` (driver runtime-PM knob — `pm_genpd_summary`
   confirms `cam on-0` after the write), then insmod**: returns
   rc=0 silently, then the SoC reboots ~5–10 s later anyway.  The
   power-domain being on isn't sufficient; something else in the
   probe path is the actual trigger.

This is now the gating issue for camera preview.  Next-step
investigation:

- **DT diff vs stock Halium**: capture
  `/proc/device-tree/soc/{kd_camera_hw1@1a004000, *cam_isp*,
  *seninf*}/` recursively on a stock-Halium boot vs ours.  Identify
  pinctrl / clock / mtk-cmdq-sec / SMI properties that differ.
- **Halium first-stage init module load**: pull
  `vendor_boot/ramdisk/lib/modules/` + `modules.load` (if present)
  from Halium's `vendor_boot.img` to see whether stock Halium
  loads these via Linux module-init or via modprobe at later
  init.rc stages.  If the latter, identify which init.rc trigger
  fires first (e.g. on property `vold.decrypt=trigger_default_encryption`).
- **Pre-load `cmdq_helper_inf` / `mtk-cmdq-sec-drv`**: these are
  cmdq-secure helpers used by the deferred modules.  cmdq_helper_inf
  is already loaded but the secure (`mtk-cmdq-sec-drv`) variant
  might not be — and the deferred modules may need both.  Inspect
  the registered platform devices before+after their insmod to
  see what the probe touches.
- **Hold a userspace fd to `/dev/camera-isp` then insmod**: opening
  the chardev triggers `pm_runtime_get` inside the existing
  camera_isp.ko driver, which should fan out to the SMI / cmdq
  larbs the deferred modules need at probe time.

### Update 2026-05-21 (part 2) — Stock Halium 12 ships NONE of these

Investigation of the Halium blobs in
`device/board/oniro/hybris_generic/halium-blobs/` (`bootstrap.zip`
+ `device.tar.xz` + `halium_vendor_a.img` + `halium_system_a.img`)
shows:

- `vendor_boot.img` ramdisk `lib/modules/modules.load` lists **161
  modules**, *none* of which are `camera_*` or `imgsensor*`.
- `vendor_boot.img` ramdisk `lib/modules/` directory holds **181
  `.ko` files**; the diff against `modules.load` is unrelated to
  camera (zram, mtk-mbox, etc.).
- The vendor partition has `/vendor/lib/modules` as a **broken
  symlink** to `/vendor_dlkm/lib/modules` — and `super.img` has
  no `vendor_dlkm` partition (only `system_a` + `vendor_a`).  No
  `.ko` files are shipped via the system_a image (the LXC-host
  Android rootfs) either.
- The Halium kernel `Image` is not built with these drivers `=y`
  either: `strings vmlinux | grep -E 'rsc.*isp|dip.*isp|...'`
  returns nothing.
- The port-repo source (`kernel/linux/volla-vidofnir/
  vendor-ramdisk-overlay/lib/modules/modules.load`) matches the
  built ramdisk — confirming `modules.load` is the upstream
  source-of-truth, not a build-time stripping artefact.

**Implication:** stock Halium 12 for Volla X23 **never loaded
these modules at all** under Ubuntu Touch / Halium.  Cameras under
upstream Halium-12 + UBports never reached a working state on
this device either; this is not a regression caused by native
boot.  There is no "stock loading sequence" to copy.

The deferred-modules-cause-watchdog-reboot blocker is therefore a
**new feature** the OHOS native-boot port has to solve, with no
upstream-port reference to crib from.  The investigation has to
work from kernel-side first principles (DT inspection, probe
tracing) rather than reverse-engineering a known-good loader.

Practical implication for the camera HAP preview goal: stock
Halium 12 droidmedia paths assume `/dev/camera-rsc` exists; on a
fresh boot with only the 17 safe modules loaded, the device does
not exist; the MTK HAL fails with `no rsc device`.  Three
possible paths forward:

1. **Solve the watchdog reboot** for the deferred 6 modules
   (kernel debugging — DT, probe sequencing, register
   dependencies).
2. **Fall back to a different camera HAL revision** that doesn't
   require the RSC sub-block (e.g. an older MTK HAL on a
   different vendor.img base).
3. **Implement frame delivery in droidmedia without going through
   the MTK HAL** — direct V4L2 against `/dev/video*` + `imgsensor`,
   plus manual ISP staging.  Substantially more work; loses
   3A/tuning/HDR.

### Update 2026-05-21 (part 3) — 4 of 6 deferred modules DO load

Empirical retest of the deferred 6 modules **individually** on a
warm boot shows the watchdog blocker is narrower than the original
"all six reboot" finding suggested:

| Module               | result                | chardev created           |
|----------------------|-----------------------|---------------------------|
| `camera_pda.ko`      | ✅ loads, rc=0        | (no /dev/camera-pda)      |
| `camera_dpe_isp60.ko`| ✅ loads, rc=0        | (no /dev/camera-dpe)      |
| `camera_rsc_isp60.ko`| ✅ loads, rc=0        | `/dev/camera-rsc` ✓       |
| `camera_mfb_isp6s.ko`| ✅ loads, rc=0        | (no /dev/camera-mfb)      |
| `camera_wpe_isp6s.ko`| ❌ insmod hangs → WDT | —                          |
| `camera_dip_isp6s.ko`| ❌ insmod hangs → WDT | —                          |

The DT view bears this out: the safe modules each match exactly
**1 DT node** (e.g. `1b003000.rsc → mediatek,rsc`), while the
crashers match either:
- `wpe_a@15011000` + `wpe_b@15811000` (2 nodes, both bare —
  `compatible`/`reg`/`interrupts` only, no `mediatek,larb` /
  `power-domains` properties), or
- `dip_a0@15021000` only — the other 23 dip_a*/dip_b* DT nodes
  have unique compatible strings (`dip_a1`..`dip_a11`,
  `dip_b0`..`dip_b11`) that `dip_of_ids` does NOT match.

So WPE/DIP each really probe 1 device — but those DT nodes are
**bare** (no `mediatek,larb` / `power-domains` properties).  The
`{wpe,dip}_probe` paths call `pm_runtime_enable(dev)` before the
early-return on `!node_larb9 || !node_larb11`.  Hypothesis:
genpd attach from `pm_runtime_enable` (or the iommu attach
inside `dma_set_mask_and_coherent`) is touching an unpowered IMG
SoC region and faulting, which deadlocks the calling task — the
hardware watchdog then reboots after the timeout because the
kernel stopped petting it.  This is consistent with the
"insmod doesn't return; ~30 s later WDT reboot" symptom and the
fact that pstore console-ramoops loses the trailing seconds
before the reboot.

**Practical outcome:** the 4 safe deferreds (PDA + DPE + RSC +
MFB) have been added to `init.x23.cfg`'s pre-init `insmod`
chain.  `/dev/camera-rsc` is now present and the MTK HAL gets
past the original `no rsc device` block.

**New (smaller) gating issue:** the HAL next opens `/dev/camera-dip`
(via `libmtkcam_dip_isp6s.so`) and that fails with `errno=2 (ENOENT)`:

```
E IspDrvDipPhy: ERROR: DIP kernel 1st open fail, errno(2):No such file or directory.
E IspDrvDipPhy: ERROR: DIP kernel 2nd open fail, errno(2):No such file or directory.
```

So unblocking preview now requires only `camera_dip_isp6s.ko`
(and likely `camera_wpe_isp6s.ko`).  The kernel-debug scope
narrows from "all six deferred modules" to "fix the genpd /
iommu attach bug in the WPE + DIP probes".  Next steps for
debug (since pstore loses the pre-WDT seconds): boot with
`pstore.backend=ramoops` + a real serial console (or
`earlycon`); add a manual `pr_emerg("step N")` print before
each phase of DIP_probe and DIP_Init; or write a tiny pre-load
shim that NULL-checks `mediatek,larb` and returns -ENODEV
before `pm_runtime_enable` runs.

### Update 2026-05-22 — Kernel patch lands; WPE+DIP now loadable

The probe / init investigation bottomed out via a `dmesg -w` HDC
stream + a 1-line marker (`echo === BEGIN_X === > /dev/kmsg`)
that survives just long enough for the kernel-side `pr_emerg()`
prints to flush before the HW watchdog fires.  Final picture:

1. **WPE_probe / DIP_probe — bare DT nodes**.  `wpe_a@15011000`,
   `wpe_b@15811000`, and `dip_a0@15021000` are declared with
   only `compatible` / `reg` / `interrupts`.  The driver's
   `of_parse_phandle("mediatek,larb", 0)` returns NULL, but
   `of_find_device_by_node(NULL)` walks the platform bus and
   matches *any* device with `of_node == NULL` (many non-DT
   platform devices fit), returning a NON-NULL garbage pointer.
   The driver's `WARN_ON(!pdev_larb)` check is bypassed.
   `pm_runtime_enable(dev)` is called *before* the would-be
   bail-out, leaking an enable-count too.

2. **DIP_probe — imgsys_config has 1 larb, driver wants 2**.
   `imgsys_config@15020000` (compatible `"mediatek,imgsys"`)
   carries `mediatek,larb = <&smi_larb9>;`.  Only 1 entry.
   The driver reads index 0 (good — resolves to smi_larb9)
   AND index 1 (returns NULL).  Original code LOG_INFs but
   keeps going; `of_find_device_by_node(NULL)` then returns
   garbage as in #1 → driver stores a non-NULL but invalid
   pointer in `dip_devs->larb11`.

3. **WPE_Init / DIP_Init — cmdqCoreRegisterCB or
   register_pm_notifier deadlocks**.  Even with both probes
   bailing cleanly at the top, `WPE_Init` calls
   `cmdqCoreRegisterCB(mdp_get_group_wpe(), …)` then
   `register_pm_notifier(…)`.  On this kernel one of those
   never returns — `insmod` hangs, hardware watchdog fires
   ~30 s later.  Root cause inside cmdq/pm not yet known.

**Kernel patch** —
`device/board/oniro/hybris_generic/kernel/x23/patches/kernel-source/
camera-isp-bare-dt.patch`:
- WPE_probe + DIP_probe: top-of-function bail
  (`return -ENODEV;`) when `of_property_read_bool(np,
  "mediatek,larb")` is false.  Prevents the
  pm_runtime_enable + garbage-pdev_larb on the bare nodes.
- DIP_probe: NULL-check the phandle before
  `of_find_device_by_node`.  Leave `dip_devs->larb11 = NULL`
  if its phandle is missing — its only consumer is conditional
  on `DIP_IMG_MFB_DIP` / `DIP_IMG_DIP2` clocks being non-NULL,
  which they aren't on this device's DT.
- WPE_Init + DIP_Init: `#if 0` (under
  `VOLLAX23_BISECT`) the `cmdqCoreRegisterCB` +
  `register_pm_notifier` calls — defers the camera HAL
  integration with GCE / system PM until that hang is rooted.
  Reasonably-safe defer: those callbacks fire during
  GCE-batch capture and during system suspend, neither of
  which is the immediate-preview path.
- pr_emerg bisection markers retained for the next debug
  pass.

**Empirical result after the patch + the user-space wiring
(below)**: WPE + DIP modules `insmod` cleanly.
`/dev/camera-{isp,mem,rsc,dip}` all exist.  The Halium MTK HAL
gets past every earlier failure (`no rsc device`, `DIP open
fail errno=2`, `DIP open fail errno=13`) and into P1Node + P2Node
configuration.  Sensor frames are captured (P1Node logs `Sensor
2000x1500` with non-zero TG timestamps).

**Userspace wiring needed alongside the patch** (in
`vendor/oniro/hybris_generic`):
- `etc/init/init.x23.cfg` pre-init insmods the 4 safe
  deferred modules + `chmod 666` on `/dev/camera-{isp,mem,
  rsc,dip,kd_camera_hw,seninf}`.  The chmod is required
  because Halium-side `camerahalserver` runs as Halium
  `cameraserver` (uid 1047) in its own user namespace which
  doesn't overlap with any OHOS-named group — chowning to
  `camera_host:camera_host` wouldn't grant Halium access.
- `etc/ueventd/ueventd.config` carries matching `0666 root
  root` rules for chardevs created by post-boot module
  reloads.
- WPE + DIP are deliberately NOT auto-loaded at boot.  See
  "Remaining blocker" below.

**Remaining blocker — Camera HAP preview frames don't flow**:
loading all 6 deferred modules at boot is **stable**; the
device stays up; the OHOS Camera HAP launches and renders
its UI.  Halium-side, `camerahalserver` connects, opens the
camera, configures streams, P1Node starts capture, and sensor
TG timestamps come back non-zero (the sensor IS producing
frames).  But the P2/DIP processing chain that turns those
sensor frames into Camera3 metadata never completes:
`IspDrvDipPhy::setMemInfo` reports `set tpipe mem info fail
cmd:0x1 memSizeDiff:0x10000`, then `IspDrv_B::setLCE_D1`
SIGSEGVs at `0x5a14` (NULL + offset).  cameraserver times
out at ~5 s on "9 inflight frames, metadata arrived: false";
Camera3 enters ERROR_DEVICE, `camerahalserver` is killed +
respawned; the cycle repeats.  Net effect: HAP preview area
stays black.

The device-tree mt6789.dts has no `reserved-memory` region
for the DIP `tpipe` (the `memPa:0xfc000000` the userspace
HAL hands the kernel doesn't correspond to anything in
`/proc/iomem`), and `imgsys_config@15020000` carries only
`<&smi_larb9>` where the driver wants two larbs — so the
HAL is reaching into hardware state our DT doesn't describe.

Earlier note (now corrected): I initially read the post-HAP
hdc disconnect as a kernel reboot.  It isn't — `uptime`
through the same sequence shows the device stays up the
whole time.  The disconnect was hdcd hiccupping while
camerahalserver respawned; the kernel is fine.

Next debug: ftrace + pr_emerg the DIP ioctl handlers that
the userspace `setMemInfo` + `setLCE_D1` path opens, and
audit `dip_devs->larb11` derefs for any outside the
clock-gated `mtk_smi_larb_{get,put}` paths.

## New modules to add — dependency-resolved load order

22 modules, broken into three sub-blocks.  See "Empirical load
behaviour" above for which subset of these get insmod'd at pre-init
vs deferred.

### Sub-block A — TEE (GenieZone + Trustonic MobiCore) + secure heap

Required transitively by `camera_mem.ko` (and `camera_fdvt_isp51.ko`,
if we choose to load FD HW).  Order is critical: gz_trusty before
gz_ipc; mcDrvModule + gz_ipc + gz_trusty before gz_tz_system; etc.

| # | Module | Why |
|---|---|---|
| 1 | `gz_trusty_mod.ko`     | GenieZone trusty base; root of the chain |
| 2 | `gz_ipc_mod.ko`        | IPC channel; deps: gz_trusty_mod |
| 3 | `mcDrvModule.ko`       | Trustonic MobiCore driver — no deps (independent of gz_) |
| 4 | `gz_tz_system.ko`      | TZ system; deps: MobiCoreDriver + gz_ipc + gz_trusty |
| 5 | `iommu_gz.ko`          | GenieZone IOMMU bridge; deps: gz_tz_system + chain |
| 6 | `trusted_mem.ko`       | Trusted memory subsystem; deps: iommu_gz + chain |
| 7 | `mtk_sec_heap.ko`      | Secure DMA-BUF heap; deps: trusted_mem + chain + mtk_iommu |

### Sub-block B — Camera ISP6s + image-sensor framework

Loaded after sub-block A; deps within block resolved by ordering.

| # | Module | Direct deps |
|---|---|---|
|  8 | `archcounter_timesync.ko` | (none) |
|  9 | `v4l2-fwnode.ko`         | (none) |
| 10 | `imgsensor-glue.ko`      | (none) |
| 11 | `mtk_ccu.ko`             | aee_aed, mtk-icc-core, mrdump, mtk-smi, mtk-smi-dbg (block already loaded) |
| 12 | `imgsensor.ko`           | imgsensor-glue, mtk_ccu, v4l2-fwnode (v4l2 framework; only matches `mediatek,imgsensor` DT compatible — see Sensor binding blocker) |
| 12b | `imgsensor_isp6s.ko`    | hardware_info (already loaded), clk-common — legacy MTK ISP6s sensor framework; registers `image_sensor`, `seninf`, `seninf_n3d` platform drivers |
| 13 | `cam_qos.ko`             | mtk-icc-core |
| 14 | `camera_isp.ko`          | archcounter_timesync, mtk-smi-dbg, cam_qos |
| 15 | `camera_mem.ko`          | mtk-smi, mtk_sec_heap |
| 16 | `camera_dip_isp6s.ko`    | cmdq_helper_inf, mtk-smi, mtk-cmdq-drv-ext — **deferred (probe hangs SoC)** |
| 17 | `camera_dpe_isp60.ko`    | mtk-cmdq-drv-ext, mtk-smi — **deferred (suspect same)** |
| 18 | `camera_mfb_isp6s.ko`    | mtk-smi, mtk-cmdq-drv-ext, mtk-icc-core — **deferred (suspect same)** |
| 19 | `camera_rsc_isp60.ko`    | mtk-cmdq-drv-ext, mtk-smi, irq-dbg — **deferred (suspect same)** |
| 20 | `camera_wpe_isp6s.ko`    | cmdq_helper_inf, mtk-cmdq-drv-ext, mtk-smi — **deferred (suspect same)** |
| 21 | `camera_pda.ko`          | mtk-icc-core, iommu_debug — **deferred (suspect same)** |
| 22 | `camera_eeprom.ko`       | (none) — calibration data parser, binds to i2c-4/4-0051, 4/4-0052, 8/8-0050 |
| 23 | `camera_af_media.ko`     | (none) — autofocus media controller |

### Sub-block C — JPEG encoder (used by HAL BLOB stream for still capture)

| # | Module | Direct deps |
|---|---|---|
| 24 | `mtk_jpeg.ko`            | mtk-smi, mtk-icc-core |

### Optional / deferred (Milestone 2+)

| Module | Why deferred |
|---|---|
| `camera_fdvt_isp51.ko` | HW face-detection accelerator; not required for preview/still/torch (CONTROL_FACE_DETECT_MODE can stay OFF in Milestone 1).  Drag-in of the cmdq-sec-drv + trusted-memory chain we already loaded for `camera_mem`. |
| `imgsensor_isp6s.ko`   | Legacy (non-v4l2) imgsensor adaptation.  The src-v4l2 `imgsensor.ko` is canonical for Halium 12; loading both would compete for /dev/kd_camera_hw.  Skip. |
| `mtk-cam-isp.ko` + `mtk-cam-plat-util.ko` | These are ISP7 (mt6879/mt6895/mt6983 SoC family).  MT6789 uses ISP6s.  Skip. |
| `camera_dpe_isp70.ko`  | ISP7 depth processing engine.  MT6789 uses dpe_isp60. |
| `camera_eeprom_isp4.ko`| ISP4 (older SoC family) EEPROM fallback.  `camera_eeprom.ko` covers the generic path. |

## Build-system integration

### `kernel/x23/extra-modules.list` append

```
# MT6789 ISP6s camera + image-sensor stack.  Native boot doesn't run
# Halium's second-stage init, so the modules MTK ships in vendor_dlkm
# never load.  Bundle them into vendor_boot via build_kernel.sh + insmod
# at OHOS pre-init.  See phase_n12_camera.md § N12.2.

# TEE chain (transitive dep of camera_mem.ko for secure DMA-BUF heap).
gz_trusty_mod.ko
gz_ipc_mod.ko
mcDrvModule.ko
gz_tz_system.ko
iommu_gz.ko
trusted_mem.ko
mtk_sec_heap.ko

# Camera ISP6s core + image-sensor framework + JPEG.
archcounter_timesync.ko
v4l2-fwnode.ko
imgsensor-glue.ko
mtk_ccu.ko
imgsensor.ko
cam_qos.ko
camera_isp.ko
camera_mem.ko
camera_dip_isp6s.ko
camera_dpe_isp60.ko
camera_mfb_isp6s.ko
camera_rsc_isp60.ko
camera_wpe_isp6s.ko
camera_pda.ko
camera_eeprom.ko
camera_af_media.ko
mtk_jpeg.ko
```

### `vendor/oniro/hybris_generic/etc/init/init.x23.cfg` insmod block

Append **after** the existing audio, connsys, and touch blocks; the
deps above are guaranteed loaded by then.  Block ships only the
17 safe modules (see Empirical load behaviour above).  ISP
sub-blocks (DIP/DPE/MFB/RSC/WPE/PDA) are intentionally omitted —
they trigger a kernel watchdog reboot when their probe runs against
an OFF camera power-domain.

```jsonc
"insmod /mnt/kmodules/gz_trusty_mod.ko",
"insmod /mnt/kmodules/gz_ipc_mod.ko",
"insmod /mnt/kmodules/mcDrvModule.ko",   // no-op if rpmb_mtk already pulled it in
"insmod /mnt/kmodules/gz_tz_system.ko",
"insmod /mnt/kmodules/iommu_gz.ko",
"insmod /mnt/kmodules/trusted_mem.ko",
"insmod /mnt/kmodules/mtk_sec_heap.ko",

"insmod /mnt/kmodules/archcounter_timesync.ko",
"insmod /mnt/kmodules/v4l2-fwnode.ko",
"insmod /mnt/kmodules/imgsensor-glue.ko",
"insmod /mnt/kmodules/mtk_ccu.ko",
"insmod /mnt/kmodules/imgsensor.ko",
"insmod /mnt/kmodules/imgsensor_isp6s.ko",  // legacy MTK ISP6s sensor framework
"insmod /mnt/kmodules/cam_qos.ko",
"insmod /mnt/kmodules/camera_isp.ko",
"insmod /mnt/kmodules/camera_mem.ko",
"insmod /mnt/kmodules/camera_eeprom.ko",
"insmod /mnt/kmodules/camera_af_media.ko",
"insmod /mnt/kmodules/mtk_jpeg.ko",
```

Modules deliberately **not** insmod'd at pre-init (deferred — see above):

```jsonc
// "insmod /mnt/kmodules/camera_dip_isp6s.ko",   // probe hangs SoC
// "insmod /mnt/kmodules/camera_dpe_isp60.ko",   // (suspected same)
// "insmod /mnt/kmodules/camera_mfb_isp6s.ko",   // (suspected same)
// "insmod /mnt/kmodules/camera_rsc_isp60.ko",   // (suspected same)
// "insmod /mnt/kmodules/camera_wpe_isp6s.ko",   // (suspected same)
// "insmod /mnt/kmodules/camera_pda.ko",         // (suspected same)
```

## Verification gate

After `pre-init` runs:

```sh
hdc shell "lsmod | grep -E 'camera_|imgsensor|mtk_ccu|mtk_sec_heap|trusted_mem|gz_|mcDrv|mtk_jpeg|archcounter'"
# expect: 22 lines (1 per added module)

hdc shell "ls /dev | grep -iE 'video|v4l|kd_camera|camera-'"
# expect: video0..videoN, v4l/v4l-subdev0..N, kd_camera_hw, /dev/camera-* nodes

hdc shell "dmesg | grep -iE 'imgsensor|camera_isp|mtk_ccu' | tail -30"
# expect: sensor probes succeed, ISP firmware load OK, no -EPROBE_DEFER loops

hdc shell "nsenter --mount=/proc/\$(pgrep -f 'init second_stage' | head -1)/ns/mnt \
            --pid=/proc/\$(pgrep -f 'init second_stage' | head -1)/ns/pid \
            -- chroot /root /system/bin/dumpsys media.camera 2>&1 | head -50"
# expect: "Number of camera devices: 2" (rear + front), per-camera id blocks
```

Failures to diagnose next:

- `insmod` fails on `gz_trusty_mod.ko` with `ENOENT` → re-check
  `extra-modules.list` exact filename; build_kernel.sh logs a
  `WARNING: extra module ... not found in build output` if the
  module name is mistyped.
- `insmod` fails on `imgsensor.ko` with `Unknown symbol mtk_ccu_*` →
  `mtk_ccu.ko` not yet loaded; ordering bug.  Re-check insmod order.
- `dumpsys` still shows 0 devices → either sensor probe failed
  (check `dmesg`) or `cameraprovider@2.6` cache is stale; one-shot
  `setprop ctl.restart vendor.camera-provider-2-6` from inside the
  Halium namespace to restart the HAL with the fresh `/dev` tree.
- ISP firmware (`lib3a.ccu`) load failure → confirm
  `firmware_class.path = /android/vendor/firmware` is still in effect
  (cat `/sys/module/firmware_class/parameters/path`).  If empty,
  N9.1's audio-firmware retarget regressed.

## Module-list growth budget

Pre-N12: ~57 modules `insmod`'d at pre-init (GPU+audio+wifi+touch).
Post-N12.2: ~79.  Plan budget was 70 before adding a `class=core`
service split (N12.2 pitfall in [phase_n12_camera.md](phase_n12_camera.md)).
We're over budget by ~9 modules.  Expected boot impact:
~5 s extra black screen at boot.

**Decision:** accept for Milestone 1; revisit splitting kernel-module
load to a class=core service if user-perceptible black-screen extends
past 15 s, or if Milestone 2 (camera + sensors + BT) crosses ~100.
