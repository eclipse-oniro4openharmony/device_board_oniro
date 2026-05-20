# Phase N12 Camera — X23 Discovery Inventory

Captured 2026-05-20 on the running native-boot Volla X23 (OHOS + Halium
chainload, see [phase_n11_chainload.md](phase_n11_chainload.md)).
Inputs for [N12.2 module staging](phase_n12_camera.md#n122--kernel-modules-stage-mtk-camera-stack-into-vendor_boot)
and [N12.5 HIDL bridging](phase_n12_camera.md#n125--hidl-client-bridging-the-load-bearing-step).

## Halium HAL services (`lshal --neat`)

Halium init is host PID 1905 (`init second_stage`, parent `androidd`).
`lshal` runs Android-side via `nsenter --mount/--pid` + `chroot /root`
(see [phase_n4_androidd.md](phase_n4_androidd.md) for the namespace
recipe).

```
DM,FC Y android.hardware.camera.provider@2.4::ICameraProvider/internal/0    PID 249
DM,FC Y android.hardware.camera.provider@2.5::ICameraProvider/internal/0    PID 249
DM,FC Y android.hardware.camera.provider@2.6::ICameraProvider/internal/0    PID 249

DM    Y vendor.mediatek.hardware.camera.atms@1.0::IATMs/default             PID 249
DM    Y vendor.mediatek.hardware.camera.bgservice@1.0::IBGService/internal/0 PID 249
DM    Y vendor.mediatek.hardware.camera.bgservice@1.1::IBGService/internal/0 PID 249
DM    Y vendor.mediatek.hardware.camera.isphal@1.0::IISPModule/internal/0   PID 249
DM    Y vendor.mediatek.hardware.camera.isphal@1.1::IISPModule/internal/0   PID 249
```

**Versions are aligned for our purposes:** all three AOSP provider
interfaces (`@2.4` / `@2.5` / `@2.6`) register on the same backing
`camerahalserver` (Halium PID 249).  Camera3 calls into the highest
version (`@2.6`) get the modern stream config + buffer manager
methods; older app paths get downgraded automatically by HIDL.

**Decision (revises plan N12.1 step 1):** target **`@2.6`** for the
HIDL bridging.  No version-pinning build flag needed — all three are
served simultaneously by the same process.

The MTK vendor extensions (`vendor.mediatek.hardware.camera.*`) sit
on top of the standard provider and are MTK-internal feature paths
(face feature, ISP HAL, ATMS).  Camera3 HIDL alone does not require
them; flag, defer, only call into them if a specific milestone needs
e.g. HDR or ATMS-tuned features.

## Currently-enumerated cameras

`dumpsys media.camera` (Android-side, same nsenter recipe):

```
== Service global info: ==
Number of camera devices: 0
Number of normal camera devices: 0
Number of public camera devices visible to API1: 0
Active Camera Clients:
[]
Allowed user IDs: 0, 1

== Camera Provider HAL internal/0-0 (v2.6, remote) static info: 0 devices: ==

== Vendor tags: ==
  Dumping configured vendor tag descriptors: 192 entries
    0x80000000 (gesturemode) ... 0x80060009-… (3afeature, hdrfeature, mfnrfeature, ...)
```

The HIDL provider has registered with `hwservicemanager`, the vendor
tag descriptor cache is populated (192 entries — MTK's full feature
tag set is loaded into Halium's `hwservicemanager`), but the provider
reports **zero** physical cameras.  This is the expected state when
the MTK ISP / `imgsensor` kernel modules have not been loaded — the
HAL has nothing to enumerate from the kernel side.  N12.2 fixes that.

## Boot-time hardware properties

Halium `getprop`:

```
[ro.boot.hardware]        = mt6789
[ro.hardware]             = mt6789
[ro.product.board]        = mt6789
[ro.product.model]        = Volla Phone X23
[ro.vendor.mtk.sensor.support] = yes
[ro.vendor.camera.isp.support.colorspace] = 0
[ro.vendor.camera3.zsl.default] = 260
[init.svc.camerahalserver]= running    (PID 249, our HIDL HAL)
[init.svc.camera_service] = running    (PID 239 — Halium-side Camera1 framework svc)
[camera.disable_zsl_mode] = 1
[persist.camera.shutter.disable] = 1
[persist.vendor.camera3.pipeline.bufnum.base.{imgo,lcso,rrzo,rsso}] = 4..5
[persist.vendor.camera3.pipeline.bufnum.min.high_ram.{imgo,lcso,rrzo,rsso}] = 7
```

The MTK pipeline buffer-count properties (`imgo`/`lcso`/`rrzo`/`rsso`)
are MTK's internal stream identifiers (image-out / linear C statistics
out / resize-region-zoom out / RAW-self-shading out).  We don't need
to wire these into the OHOS VDI directly — Halium's HAL applies them
internally and the OHOS framework just sees standard Camera3 streams.

## Camera devices on the device

`ls /dev | grep -iE 'cam|video|media|isp|v4l'`:

```
v4l            (empty directory)
```

No `/dev/video*`, no `/dev/media*`, no `/dev/camera-*` — confirms the
ISP / `imgsensor` modules are unloaded.

`/proc/devices` shows `81 video4linux` major registered (kernel V4L2
framework is built-in — `videodev.ko` is in `modules.builtin`), so
the framework is there waiting for drivers to register.

Halium's `init.mt6789.rc` references these device nodes (post-driver
load they should appear):

- `/dev/camera-{sysram,isp,mem,dip,tsf,dpe,mfb,rsc,owe,fdvt,wpe,pipemgr}`
  (one per ISP6s sub-block)
- `/dev/kd_camera_hw`, `/dev/kd_camera_hw_bus2` (imgsensor framework)
- `/dev/video*`, `/dev/v4l-subdev*`, `/dev/media*` (V4L2 nodes from
  `mtk-cam-isp` if we go that path — see N12.2 module decision)

## Available kernel modules

Local build output:
`kernel/linux/volla-vidofnir/build-dir/tmp/system/lib/modules/5.10.209/`
(753 `.ko` total; relevant subset below).

### ISP6s (MT6789) core stack

| Module | Path | Direct deps (modinfo) |
|---|---|---|
| `camera_isp.ko` | `cameraisp/src/isp_6s` | archcounter_timesync, mtk-smi-dbg, cam_qos |
| `cam_qos.ko` | `cameraisp/src/isp_6s` | mtk-icc-core |
| `camera_mem.ko` | `camera_mem` | mtk-smi, mtk_sec_heap |
| `camera_dip_isp6s.ko` | `cameraisp/dip/isp_6s` | cmdq_helper_inf, mtk-smi, mtk-cmdq-drv-ext |
| `camera_dpe_isp60.ko` | `cameraisp/dpe` | mtk-cmdq-drv-ext, mtk-smi |
| `camera_mfb_isp6s.ko` | `cameraisp/mfb` | mtk-smi, mtk-cmdq-drv-ext, mtk-icc-core |
| `camera_rsc_isp60.ko` | `cameraisp/rsc` | mtk-cmdq-drv-ext, mtk-smi, irq-dbg |
| `camera_wpe_isp6s.ko` | `cameraisp/wpe/isp_6s` | cmdq_helper_inf, mtk-cmdq-drv-ext, mtk-smi |
| `camera_fdvt_isp51.ko` | `cameraisp/fdvt` | mtk_sec_heap, cmdq-sec-drv (deferred — for face detection HW) |
| `camera_pda.ko` | `cameraisp/pda/isp_71` | mtk-icc-core, iommu_debug |
| `camera_eeprom.ko` | `cam_cal/src/custom` | (none) |
| `camera_af_media.ko` | `lens/vcm/v4l2/media` | (none) |
| `archcounter_timesync.ko` | `cam_timesync` | (none) |

### Image sensor framework (v4l2-based, ISP6s/ISP7 unified)

| Module | Path | Direct deps |
|---|---|---|
| `imgsensor-glue.ko` | `imgsensor/src-v4l2/imgsensor-glue` | (none) |
| `imgsensor.ko` | `imgsensor/src-v4l2` | imgsensor-glue, mtk_ccu, v4l2-fwnode |
| `imgsensor_isp6s.ko` | `imgsensor/src/isp6s` | hardware_info, clk-common (LEGACY pre-v4l2 driver — likely **NOT** what `mtk-cam-isp.ko` and the @2.6 HIDL provider exercise; the `src-v4l2` set is canonical for Halium 12) |

### Image processing / JPEG / aux

| Module | Path | Direct deps |
|---|---|---|
| `mtk_jpeg.ko` | `media/platform/mtk-jpeg` | mtk-smi, mtk-icc-core |
| `mtk_ccu.ko` | `remoteproc` | aee_aed, mtk-icc-core, mrdump, mtk-smi, mtk-smi-dbg |
| `v4l2-fwnode.ko` | `media/v4l2-core` | (none) |

### Notes on absent modules

- **No `seninf.ko`.**  The sensor interface (CSI/MIPI receiver) is
  built into the kernel image on this Halium 12 build, not a module.
  Confirmed by absence from `modules.dep` and `modules.builtin`
  references to `mtk-sensors`-style code.  Nothing to insmod.
- **No per-sensor `gc*` / `s5k*` / `imx*` / `hi*` `.ko`.**  The
  v4l2-imgsensor framework (`imgsensor.ko`) is a generic driver
  that reads the per-sensor table from the kernel's compiled-in
  `imgsensor_drv` data + device-tree.  On older Halium builds these
  were per-sensor `.ko`; on Halium 12 it's collapsed into the
  framework module + DT bindings.  All sensor enumeration goes
  through `imgsensor.ko` alone.
- **No `mtk-cam-plat-mt6789.ko`.**  The `mtk-cam-isp.ko` driver at
  `media/platform/mtk-isp/camsys/isp7_1/` is the next-gen ISP7 path
  (mt6879/mt6895/mt6983 SoCs); MT6789's ISP is ISP6s, served by the
  separate `cameraisp/src/isp_6s/camera_isp.ko` stack listed above.
  Confirmed by `dmesg` references to ISP6s sub-blocks in Halium's
  init.mt6789.rc and the absence of `mtk-cam-plat-mt6789.ko`.
  **Skip `mtk-cam-isp.ko`** — not for this SoC.

### TEE chain (required transitively by `camera_mem.ko`)

`camera_mem.ko` modinfo deps include `mtk_sec_heap` for the secure
DMA-BUF allocation path.  `mtk_sec_heap.ko` in turn drags in the
MediaTek GenieZone (gz_*) + Trustonic TEE (MobiCore) stack.  Required
modules (none currently loaded):

```
mcDrvModule.ko          — Trustonic MobiCore Driver
gz_trusty_mod.ko        — GenieZone trusty base
gz_ipc_mod.ko           — GenieZone IPC channel (deps: gz_trusty_mod)
gz_tz_system.ko         — GenieZone TZ system (deps: MobiCoreDriver, gz_ipc, gz_trusty)
iommu_gz.ko             — GenieZone IOMMU bridge (deps: gz_tz_system + chain)
trusted_mem.ko          — Trusted memory subsystem (deps: iommu_gz + gz chain)
mtk_sec_heap.ko         — Secure DMA-BUF heap (deps: trusted_mem chain)
```

These are the same modules that fire on a normal Halium boot.
Re-using them is correct.

### Modules already loaded (from current GPU + audio + WiFi + touch blocks)

`lsmod` snapshot of relevant transitive deps already up at pre-init:

```
aee_aed, mrdump, emi, emi-mpu, mtk-smi, mtk-smi-dbg, mtk-icc-core,
iommu_debug, iommu_secure, mtk_iommu, mtk-cmdq-drv-ext, cmdq_helper_inf,
vcp_status, mmprofile, irq-dbg, device-apc-common, system_heap,
clk-common, clk-mt6789-cam (already in extra-modules.list from GPU block)
```

So the *delta* N12.2 has to introduce is: the TEE chain (7 modules) +
the ISP6s + imgsensor stack (~14 modules) + JPEG + ccu + v4l2-fwnode
= roughly **22 new modules**.

## Open questions answered

From the original N12.1 question list:

1. **Exact ICameraProvider major.minor:** all three `@2.4` / `@2.5` /
   `@2.6` register from the same process; target `@2.6`.
2. **Exact MTK ISP kernel module set:** ISP6s (`cameraisp/src/isp_6s/`)
   not ISP7 — see table above for the full list.
3. **Number of sensors:** zero enumerated currently; the HAL reports
   `0 devices`.  Cannot determine actual sensor count until N12.2
   loads the kernel drivers (chicken-and-egg with discovery).
   Volla X23 ships with 1 rear (50 MP per Volla spec sheet) + 1 front
   (16 MP); confirm post-N12.2.
4. **Per-sensor .ko filenames:** N/A on Halium 12 — sensors are in
   `imgsensor.ko` + DT.  Sensor identity comes from runtime probe.
5. **Camera HAL needs `cameraserver`?**  `init.svc.camera_service =
   running` (PID 239) on Halium — that's Halium's `cameraserver`,
   the framework process not the HAL.  Whether HIDL provider calls
   need it is TBD; first cut bypass it and only re-introduce if we
   see a transaction failure that traces to a `cameraserver` lookup.
6. **`vendor.camera.*` properties:** ~50 of them are set, mostly
   pipeline buffer counts; Halium init.mt6789.rc sets them and they
   are visible to the HAL via the shared `/dev/__properties__` from
   N4.1.  Nothing OHOS-side needs to pre-seed them; `androidd`'s
   env vars + Halium init already cover it.

## Next actions (gates N12.2)

1. Stage the 22-module delta into
   `kernel/x23/extra-modules.list` + `init.x23.cfg` insmod block in
   dep-resolved order (modulemap doc below).
2. Rebuild kernel `vendor_boot` (will pick up new modules into the
   overlay) + flash.
3. Re-run `dumpsys media.camera`; expect non-zero device count and a
   `Camera id: 0`, `Camera id: 1` block with resolutions + facing.
4. Capture that output to update this inventory with the *actual*
   sensor IDs.
