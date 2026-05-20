# Phase N12 — Camera (Camera3 HIDL via androidd)

**Status (2026-05-20):** N12.1 discovery ✅, N12.2 module staging ✅,
N12.2.5 NV-calibration mount ✅ (3 cameras enumerated, full Camera3
characteristics returned by HIDL provider), N12.4 VDI scaffolding ✅.
See subphase index below for live status of each step; this header
summarises only.

> Original status, kept for reference of what was unknown at plan time:
> ⏳ Planned (2026-05-20).  No source written yet.  This doc is a
> pre-execution plan; structure mirrors `phase_n4_androidd.md` and
> `phase_n8_graphics_native.md` in their pre-implementation form (option
> tables + sub-phase breakdown + obstacle list).  All subphases below
> declared `⏳ Planned` until each lands and a status update overwrites
> the line.

> **Goal.** Bring up OHOS Camera HDI on the Volla X23 (then `mimir`)
> under native boot, sourcing the camera HAL via the same libhybris +
> Halium-namespace bridge that already powers display, audio, and
> touch.  Target end-state for **Milestone 1**: preview viewfinder
> renders on screen, still capture produces a JPEG to the gallery,
> flashlight toggle works.  Video, slow-mo, RAW, multi-camera composer
> deferred.
>
> **Primary path:** Camera3 HIDL — wrap Halium's
> `android.hardware.camera.provider@2.x::ICameraProvider` over the
> shared `/dev/hwbinder`, exactly like `composer_host` wraps Halium's
> `IComposer@2.x`.  Camera3 is what OHOS Camera HDI is shaped for
> (streams + metadata + async results); the mapping is mostly direct.
>
> **Plan B (smoke test only):** Camera1 via libhybris's existing
> `hybris/camera` + `compat/camera` shims (Ubuntu-Touch heritage,
> wraps the deprecated Android `Camera` client API).  Useful purely as
> an early proof that kernel modules + buffer interop are sane —
> *not* the milestone end-state, since Camera1 on Android-12 MTK is
> patchy and lacks Camera3-only features.  Decision tree at N12.3.

---

## Why camera is hard (read this first)

Camera is the largest single HAL we'll have bridged.  The reasons it is
not just "another libhybris VDI like display":

1. **More state.**  Display is mostly idempotent draw calls.  Camera
   carries a *session* (open ↔ close), a *stream configuration*
   (allocate buffers of specific format/usage), and a *capture queue*
   of inflight requests with async result + buffer-release callbacks
   and per-frame metadata blobs.
2. **Two metadata vocabularies.**  Android uses
   `camera_metadata_t` tagged with `ANDROID_*` keys.  OHOS uses its
   own `camera_metadata_operator` with `OHOS_*` keys.  The wire format
   (TLV-tagged blob) is similar but the tag namespaces only partially
   overlap.  A translation table is unavoidable.
3. **Buffer interop with fences.**  Camera output buffers cross from
   the Android HAL into the OHOS rendering / encoder path with
   acquire/release fences attached.  Display has the same shape but
   only one buffer at a time and one consumer; camera has multiple
   concurrent streams (preview + still + …) with different consumers
   (SurfaceTexture, JPEG encoder, video encoder).
4. **MTK ISP kernel stack is `vendor_dlkm` only.**  Same shape as the
   GPU (§ N8.11), audio (§ N9.5), and WiFi (§ N9.2) — the
   `mtk-cam-isp-*`, `seninf`, `imgsensor_drv`, sensor-specific
   drivers all live in Halium's vendor_dlkm partition and are loaded
   by Halium's second-stage init.  Native boot has no second-stage
   init, so the modules never load and `/dev/camera*` never appears
   — even before any userspace HIDL work matters.  Bundle them into
   `vendor_boot` + `insmod` at pre-init, dep-ordered.
5. **Sensor calibration data + tuning blobs.**  MTK ISPs read per-unit
   calibration (`eeprom`/`otp`) and per-tuning `.bin` files from
   `/vendor/firmware/`.  N9.1's `firmware_class.path` retarget
   already covers it, but expect surprises (some paths are hard-coded
   in the closed-source `mtk-cam-isp-*` userspace).

The result: N12 is a 4-to-6-week project, comparable to N4+N8 combined.
Not a 1-week port of an existing VDI.

---

## Dependencies — already done, must stay healthy

| Prereq | Phase | Why N12 needs it |
|---|---|---|
| `androidd` running, Halium HAL services up | N4 | `ICameraProvider` registers there |
| Default `/dev/binder` for OHOS + shared `hwbinder` | N6 | OHOS-side VDI calls cross hwbinder |
| `/android/system` + `/android/vendor` mounted | N5 | libhybris loads Android camera libs from `/android/vendor/lib64/hw` |
| libhybris linker remap (`/vendor` → `/android/vendor`) | N8.1 | gralloc + camera HAL `.so` resolve |
| `allocator_host` alive + gralloc HAL loaded | N8 | buffer allocation goes through it |
| `firmware_class.path` retargeted to `/android/vendor/firmware` | N9.1 | ISP firmware + sensor calibration land |
| MT6789 audio/GPU/WiFi kernel-module bundling pattern in `init.x23.cfg` | N8.11 / N9.2 / N9.5 | same mechanism for camera modules |
| `extra-modules.list` consumed by `build_kernel.sh` into vendor_boot overlay | N8.11 | where we add the camera modules |
| OHOS BufferHandle.stride is BYTES (vs Android gralloc PIXELS) | `project_ohos_bufferhandle_stride.md` memory | helper already in `display_common.h` — same pitfall in camera buffers |

If any of these regresses (e.g. someone breaks the `firmware_class.path`
retarget), camera fails before N12's code is even reached — debug
sequence below assumes they hold.

---

## N12 subphase index

```
N12.1  Discovery  — what HIDL services + kernel modules + sensors actually exist on the X23
N12.2  Kernel modules — bundle MTK camera/ISP/sensor .ko into vendor_boot + dep-ordered insmod
N12.3  Bridge approach — adopt Camera3 HIDL; document Camera1-fallback decision tree
N12.4  VDI scaffolding — libcamera_host_vdi_impl_1.0.z.so + BUILD.gn + device_info.hcs hook
N12.5  HIDL client bridging — libhybris loads HIDL stubs from Halium; OHOS VDI calls over hwbinder
N12.6  Camera enumeration + ability — getCameraIdList + getCameraCharacteristics → OHOS metadata
N12.7  Open + session — ICameraDevice.open → ICameraDeviceSession ↔ OHOS ICameraDeviceVdi
N12.8  Stream configuration — CreateStreams ↔ configureStreams_3_4; format/usage translation
N12.9  Buffer interop — BufferHandle ↔ native_handle_t; fence forward + return
N12.10 Preview path — repeating CaptureRequest at 30 fps; verify on display
N12.11 Still capture (JPEG) — BLOB stream + orientation/quality metadata
N12.12 Flashlight + flash mode — ICameraProvider.setFlashlight passthrough; capture-time CONTROL_AE_MODE
N12.13 HCS metadata sync — replace generic camera_host_config.hcs with values from real sensors
N12.14 Test harness + bring-up checklist — camera_demo + Camera HAP visual smoke
N12.15 Stability + teardown — close-while-streaming, configure-during-capture, the camera analogue of bug 8.17
N12.16 mimir tablet variant — what carries over, what doesn't (MT8781 vs MT6789)
```

Dependency graph:

```
N12.1 ─┬─→ N12.2 ──→ N12.7 (open)
       │             ↓
       └─→ N12.5 ──→ N12.6 ──→ N12.8 ──→ N12.9 ──→ N12.10 (preview) ──→ N12.11 (still)
                                                                        ↓
                                                                       N12.12 (flash)
                                              N12.4 (scaffolding) ─────┘
                                              N12.13 (HCS) — anytime after N12.6 lands
                                              N12.14 (tests) — gates each of 10/11/12
                                              N12.15 (stability) — after 10 is reliable
                                              N12.16 (mimir) — last
```

---

## N12.1 — Discovery (no code; do this first) ✅ DONE (2026-05-20)

**Outcome:** all three open questions answered, two artefacts committed:
[phase_n12_camera_x23_inventory.md](phase_n12_camera_x23_inventory.md) +
[phase_n12_camera_modulemap.md](phase_n12_camera_modulemap.md).
Key findings (full detail in the inventory doc):

- **HIDL provider:** all three `android.hardware.camera.provider@2.4`,
  `@2.5`, `@2.6` register simultaneously from the same Halium process
  (PID 249 inside the namespace = `camerahalserver`).  Adopt `@2.6`
  for the OHOS-side bridging (modern stream config + buffer manager).
- **Currently 0 cameras enumerated.**  `dumpsys media.camera` reports
  `Camera Provider HAL internal/0-0 (v2.6, remote) static info: 0
  devices` — the HAL is healthy, vendor-tag descriptor cache is
  populated (192 entries), but no MTK ISP / sensor kernel modules
  loaded so the HAL has nothing to enumerate.  N12.2 fixes this.
- **ISP generation: ISP6s** (not ISP7).  MT6789 uses the
  `cameraisp/src/isp_6s/camera_isp.ko` stack, not the newer
  `media/platform/mtk-isp/camsys/isp7_1/mtk-cam-isp.ko` driver.
  This corrects the original plan's tentative `mtk-cam-isp-7s.ko` name.
- **Sensor framework:** modern v4l2-based (`imgsensor/src-v4l2/imgsensor.ko`),
  not the legacy `imgsensor_isp6s.ko`.  Per-sensor drivers are
  collapsed into the framework + device-tree on Halium 12; there are
  no per-sensor `.ko` files to load (corrects the plan's `gc02m1.ko`
  / `s5kjn1.ko` etc. placeholders).
- **22-module delta to add** to `extra-modules.list`, including a 7-module
  TEE (GenieZone + Trustonic MobiCore) sub-block for `mtk_sec_heap`.

The detailed dependency-resolved load order is in
[phase_n12_camera_modulemap.md](phase_n12_camera_modulemap.md).

### Original plan recorded for reference (pre-execution)

**Goal.** Inventory what is already on the device before writing any
source.  Three things to find out:

1. **Which `android.hardware.camera.*` HIDL services Halium registers**
   when `androidd` is up.  Without an `ICameraProvider`, every later
   subphase is a no-op.
2. **Which MTK camera/ISP/sensor kernel modules exist** in
   `/android/vendor/lib/modules/` and what their dependency order
   looks like (`modules.dep`).  This is the input to N12.2.
3. **Which physical sensors the X23 actually ships with**, what their
   IDs, orientation, and supported formats are.  Sets expectations
   for N12.13's HCS metadata and N12.8's stream format table.

### Commands

All from `hdc shell` after a clean boot (composer + audio + wifi all
already green):

```sh
# 1. HIDL services registered by Halium
nsenter --mount=/proc/$(pidof androidd)/ns/mnt \
        --pid=/proc/$(pidof androidd)/ns/pid \
        -- sh -c 'chroot /root /system/bin/lshal --neat 2>/dev/null' \
   | grep -i camera

# Expected: at least one of
#   android.hardware.camera.provider@2.4::ICameraProvider/internal/0
#   android.hardware.camera.provider@2.5::ICameraProvider/legacy/0
#   android.hardware.camera.provider@2.6::ICameraProvider/internal/0
# (any minor 2.4–2.7 is fine; record exact version — N12.5 binds to it)

# 2. MTK camera kernel module set in Halium vendor_dlkm
ls /android/vendor/lib/modules/ | grep -Ei 'cam|seninf|imgsensor|isp|imgsys|larb_cam|iommu_cam'

# Also: what does Halium's own modules.load say about ordering?
grep -Ei 'cam|seninf|imgsensor|isp' /android/vendor/lib/modules/modules.load

# Note any dep chain via modules.dep — that's the load order for N12.2.
grep -E '^(mtk-cam|seninf|imgsensor)' /android/vendor/lib/modules/modules.dep

# 3. Sensors actually fitted to this X23
# (Run only AFTER ICameraProvider is up — i.e. once Halium init has
# completed its camera-related .rc imports.  May need to wait 10–15 s
# after boot.)
nsenter --mount=/proc/$(pidof androidd)/ns/mnt \
        --pid=/proc/$(pidof androidd)/ns/pid \
        -- sh -c 'chroot /root /system/bin/dumpsys media.camera 2>/dev/null' \
   | head -100

# Look for: "Camera id: 0/1", "Facing: BACK/FRONT", "Orientation: 90/270",
# "Resolutions: …", "Format: YCbCr_420_888/JPEG/IMPLEMENTATION_DEFINED"
```

### Likely findings (educated guess; verify on device)

| Source | Expected on MT6789 (Helio G99 family) |
|---|---|
| ICameraProvider version | `@2.5` or `@2.6` (Halium 12 typically ships 2.5; legacy/0 is the AOSP placeholder) |
| Service instance name | `internal/0` (vendor provider) |
| Camera kernel modules | `mtk-cam-isp-7s.ko`, `seninf.ko`, `mtk-cam-meta.ko`, `mtk-cam-larb.ko`, `imgsensor_drv.ko`, sensor-specific `.ko` (e.g. `gc02m1.ko`, `hi556.ko`, `s5kjn1.ko`) — exact mix is per-SKU |
| Rear sensor | one of: GalaxyCore GC8054 (8 MP), Hynix Hi556 (5 MP), Sony IMX350 — depends on Volla X23 batch; verify the kernel `imgsensor` device list |
| Front sensor | typically a low-end GC02M2 / SP2509 — verify |
| Pixel formats | `IMPLEMENTATION_DEFINED` (NV21 internally), `YCbCr_420_888`, `JPEG`, `RAW10` (only if exposed) |

### Output of N12.1

Three artefacts, committed under
`device/board/oniro/docs/hybris_generic/`:

- `phase_n12_camera_x23_inventory.md` — fresh `lshal | grep camera`,
  `ls /android/vendor/lib/modules`, `dumpsys media.camera` outputs.
- `phase_n12_camera_modulemap.md` — module → dep-chain table (input
  to N12.2's `extra-modules.list` block + init.x23.cfg insmod order).
- Filled-in N12.13 HCS targets (sensor IDs, resolutions, AF/AE/AWB
  ranges) so we know what to put in `camera_host_config.hcs`.

### Risk if skipped

We've consistently hit "kernel modules from vendor_dlkm don't load"
under native boot (GPU N8.11, audio N9.5, WiFi N9.2).  Camera will be
the same shape with more modules and tighter ordering.  Walking the
running device first costs an hour and prevents days of "I bundled
the wrong subset" iteration.

---

## N12.2 — Kernel modules: stage MTK camera stack into vendor_boot ✅ DONE (2026-05-20)

Kernel rebuilt + `vendor_boot.img` flashed; on next boot all 18 safe
camera modules are `insmod`'d by `init.x23.cfg` pre-init.  `lsmod`
on the freshly-rebooted device shows the full set: TEE chain
(gz_trusty_mod, gz_ipc_mod, mcDrvModule, gz_tz_system, iommu_gz,
trusted_mem, mtk_sec_heap), ISP6s framework (archcounter_timesync,
v4l2-fwnode, imgsensor-glue, mtk_ccu, imgsensor, imgsensor_isp6s,
cam_qos, camera_isp, camera_mem), helpers (camera_eeprom,
camera_af_media, mtk_jpeg).  /dev/camera-{isp,mem}, /dev/camera_eeprom{0,1,2},
/dev/video0 (jpeg) all appear automatically.  Six ISP sub-blocks
(camera_dip_isp6s, camera_dpe_isp60, camera_mfb_isp6s,
camera_rsc_isp60, camera_wpe_isp6s, camera_pda) are staged into
`/mnt/kmodules/` by `build_kernel.sh` (via the updated
`extra-modules.list`) but intentionally NOT loaded at boot — they
trigger a watchdog reboot if their probe runs without the camera
power-domain on (see [phase_n12_camera_modulemap.md](phase_n12_camera_modulemap.md)
§ Empirical load behaviour).

**Still open:**
- N12.2.5 (camera NV-calibration mount) — see below.

### N12.2.5 — Camera HAL NV-calibration mount ✅ DONE (2026-05-20)

After all 18 safe modules load, `dumpsys media.camera` initially
reports `Number of camera devices: 0`.  The earlier framing of the
blocker as "sensor i2c devices don't bind" turned out to be wrong —
i2c sensors do bind to `kd_camera_hw*` shims, chardev nodes
(`/dev/kd_camera_hw`, `/dev/seninf`, `/dev/camera-isp`, `/dev/camera_eeprom{0,1,2}`,
`/dev/video0`) are created, and `strace` of camerahalserver shows it
issues 438 successful FEATURECONCTROL IOCTLs and reads back correct
sensor IDs over i2c (`SID F8D1 = S5KGM1ST`, `SID 561642 = OV16A1Q`,
`SID 8A5 = GC08A3WIDE`).

The actual root cause: the MTK camera HAL needs `/mnt/vendor/nvcfg`
(GPT-labelled `nvcfg` partition, sdc6 on X23) mounted to find its
3A / sensor calibration NV files.  Under native boot Halium's
first-stage init never runs the fstab mount step, so `nvcfg` stays
unmounted, and `nvbuf_util_dep: Try to read nv_version_front1 but
not exist` is logged — the HAL refuses to expose sensors it can't
tune.

**Fix.** `mount_vendor_nv()` in
`device/board/oniro/hybris_generic/launcher/androidd.c` (commit
2026-05-20).  Resolves the `nvcfg` GPT label to a raw block device
via `/sys/class/block/*/uevent`, `setns()`es into the post-pivot
child mount NS, then mounts `/dev/block/sdc6 → /root/mnt/vendor/nvcfg`
RO.  Retries up to 16 × 500 ms because Halium init's `mount tmpfs
/mnt` runs early but not instantly.  Verified live: with the fix
in place, `dumpsys media.camera → 3 devices` (S5KGM1ST 12 MP rear
primary + OV16A1Q 16 MP front + GC08A3WIDE 8 MP rear wide); full
Camera3 characteristics blob (~13 KB / 118 keys per camera) is
returned through `ICameraProvider@2.6::getCameraIdList` /
`getCameraDeviceInterface_V3_6`.  Full investigation notes (strace,
logcat, fstab analysis) in
[phase_n12_camera_modulemap.md](phase_n12_camera_modulemap.md)
§ Sensor binding blocker — RESOLVED.

**Open follow-ups (non-blocking):**
- `/mnt/vendor/protect_f` and `protect_s` (keymaster NV) fail to
  mount with EBUSY — block device held open by some OHOS-side
  service.  Not required for camera enumeration; revisit if
  keymaster-dependent features regress.
- `/sys/class/BOOT/BOOT/boot/boot_mode` sysfs entry missing — HAL
  logs `MtkCam/HalSensor: get_boot_mode fail to open`, harmless
  warning printed multiple times during enumeration; never blocks
  anything.

### Original plan recorded for reference (pre-execution)

**Goal.** All MT6789 camera/ISP/sensor kernel modules load at OHOS
pre-init, in dep-resolved order, so `/dev/v4l-subdev*`,
`/dev/video*`, `/dev/mtk-cam*`, and any I²C sensor probe nodes are up
before `androidd` starts Halium's camera HAL.

### Mechanism (already established by N8.11, N9.5, N9.2)

1. Add modules to
   `device/board/oniro/hybris_generic/kernel/x23/extra-modules.list`.
   `build_kernel.sh` stages them into the `vendor_boot` overlay →
   `/mnt/kmodules` on device.
2. Add dep-ordered `insmod /mnt/kmodules/<name>.ko` lines to the
   `pre-init` job in
   `vendor/oniro/hybris_generic/etc/init/init.x23.cfg`.
3. Verify the iommu/larb/SMI deps from N8.11 are already loaded
   before the camera modules (they're shared with the GPU and almost
   certainly already up — but confirm; the camera ISP also goes
   through SMI larbs).

### Expected module set (refine after N12.1 discovery)

Educated guess; treat as a starting point:

```
# Sensor + ISP common deps (camera larbs are SMI-tied; may already be loaded by GPU block)
mtk_cam_larb.ko                  ← if not already pulled in by N8.11 GPU/iommu chain
mtk_iommu_v2.ko                  ← if not already loaded; check first
mtk-cam-meta.ko                  ← metadata helper
mtk-cam-pipeline.ko              ← pipeline scheduler (if =m)

# Per-MT6789 ISP block
mtk-cam-isp-7s.ko                ← ISP core (7s = Helio G99 ISP gen)
seninf.ko                        ← sensor interface (CSI/MIPI driver)
mtk-cam-aov.ko                   ← always-on viewfinder (may be optional)
mtk-cam-raw.ko                   ← raw image pipeline
mtk-cam-camsv.ko                 ← stats / DCG / HDR helpers

# Sensor framework + per-sensor drivers (only what's fitted — N12.1 inventory)
imgsensor_drv.ko                 ← imgsensor framework
# pick the right ones from below per X23 unit's bill of materials:
gc02m1.ko gc02m2.ko gc8054.ko hi556.ko s5kjn1.ko sp2509.ko ov02b1b.ko
```

Add to `extra-modules.list` under a new comment block:

```
# MT6789 / Helio G99 camera + ISP + sensor stack.  Same vendor_dlkm
# pattern as GPU (N8.11), audio (N9.5), WiFi (N9.2).
# See phase_n12_camera.md § N12.2.
mtk-cam-meta.ko
mtk-cam-isp-7s.ko
seninf.ko
mtk-cam-raw.ko
mtk-cam-camsv.ko
imgsensor_drv.ko
# … per-sensor .ko picked from N12.1's discovery
```

`init.x23.cfg` insmod block (append **after** the existing
audio/connsys blocks, in pre-init):

```
"insmod /mnt/kmodules/mtk-cam-meta.ko",
"insmod /mnt/kmodules/imgsensor_drv.ko",
"insmod /mnt/kmodules/seninf.ko",
"insmod /mnt/kmodules/mtk-cam-isp-7s.ko",
"insmod /mnt/kmodules/mtk-cam-raw.ko",
"insmod /mnt/kmodules/mtk-cam-camsv.ko",
"insmod /mnt/kmodules/gc8054.ko",    # … rear sensor (placeholder)
"insmod /mnt/kmodules/gc02m1.ko",    # … front sensor (placeholder)
```

### Verification

After `pre-init` runs:

- `lsmod | grep -E 'cam|seninf|imgsensor'` lists the inserted set.
- `ls /dev | grep -E 'video|v4l|media|mtk-cam'` shows the camera/ISP
  char devices the Halium HAL will open.
- `dmesg | grep -iE 'imgsensor|mtk-cam|seninf'` shows successful
  sensor probe (`gc8054 i2c-X: probe successful`) and no
  `-EPROBE_DEFER` loops.

### Pitfalls — anticipate, don't rediscover

- **MTK i2c sensor probe may need a regulator / clock the GPU block
  already brought up.**  If `imgsensor` probes deferred-forever,
  re-check dep chain — likely missing `pmic-mt635*` consumer (already
  in audio block) or a camera-specific regulator (e.g. `mt6363_vcam_a`).
  Add the missing `.ko` to the bundle.
- **Firmware reachable?**  N9.1's `firmware_class.path` →
  `/android/vendor/firmware` should cover ISP firmware and any
  per-sensor OTP/EEPROM blob.  If a sensor driver does its *own*
  hardcoded path lookup (some MTK sensor drivers do), bind the
  per-sensor subdir explicitly.  Same pattern as N9.5's AW883xx
  retarget.
- **`status="ok"` lies in MT DTs.**  N8.13 lesson: MTK device trees
  declare every supported touch panel `status="ok"`; the kernel
  enumerates the i2c/spi device, but actual silicon may be different.
  Same risk for camera: a sensor driver may probe successfully and
  expose a device node while the actual fitted sensor is a different
  one.  Cross-check with `dumpsys media.camera`'s sensor make/model
  string before trusting the kernel-side device.
- **Module list grows past ~70.**  We already insmod 50+ modules at
  pre-init for GPU+audio+wifi+touch.  Camera adds 10–20 more.  Once
  the list crosses ~70, OHOS init takes long enough that the user can
  see a black screen for several extra seconds.  Acceptable for now;
  later subphase to move to a proper `service` with `class=core` if
  it becomes intolerable.

---

## N12.3 — Bridge-approach decision tree

**Adopt Camera3 HIDL via androidd as the primary path.**  Document why
Camera1 is *not* the primary path, and the exit criterion that would
make us reach for it.

### Why Camera3 HIDL is right here

| | Camera3 HIDL via androidd | Camera1 via libhybris/compat |
|---|---|---|
| OHOS Camera HDI shape match | ★★★★★ (1:1 — streams, metadata, callbacks all map) | ★★ (we'd have to synthesize Camera3 metadata + simulate stream configure from a Camera1 client) |
| Modern sensor support | ★★★★★ (all formats, RAW, multi-stream) | ★★ (Camera1 client emulates Camera3 underneath, but only exposes legacy features) |
| Android 12 readiness | ★★★★★ (HIDL `@2.5`/`@2.6` is the supported provider) | ★★ (Android 12 deprecated Camera1; many MTK builds stub the client) |
| Bridging surface | ★★ (lot of code — provider, device, session, callbacks, metadata translation, buffer marshal) | ★★★★ (libhybris's existing `hybris/camera` covers most of the API) |
| Risk of "Android service we don't run" deps | ★★★ (HIDL HAL doesn't need `cameraserver`/`mediaserver` — talks directly to the HAL) | ★ (Camera1 client typically routes through `cameraserver` which we don't run) |
| Halium / UBports precedent | none on MTK | Ubuntu Touch uses this on Halium 11 devices including some MTK |

The "modern sensor support" + "Camera1 needs `cameraserver`" rows are
the dominant factors: Camera1 won't work on this device without also
bringing up the Android camera service framework, which is a much
heavier lift than just wrapping the HIDL HAL directly.

### Plan B — Camera1 as smoke test, never as milestone end-state

If after N12.5 + N12.6 the HIDL bridging proves unexpectedly painful
(e.g. binder transaction crashes from HIDL stub version mismatch, see
N12.5 pitfalls), spend half a day on Camera1 via the existing
libhybris `hybris/camera` shim to confirm that:

- the MTK camera kernel modules from N12.2 are correctly loaded,
- `imgsensor` is exporting a working sensor,
- buffer allocation through `allocator_host` round-trips with a
  camera HAL on the producer side.

If Camera1 also fails, the bug is below the HIDL layer (kernel or
gralloc); if Camera1 works, the bug is in our HIDL bridging — and
that's a much more tractable scope.

Camera1 is **never** the milestone end-state: it caps at ~5 MP, no
multi-stream, no RAW, no portrait mode, no manual exposure on most
MTK sensors.

### Exit criterion to switch primary path

> Camera1 only becomes the primary path if N12.5–N12.7 burn more than
> two weeks without `getCameraIdList` returning a non-empty list.  At
> that point, document the HIDL blocker and fall back.  Otherwise we
> persist with HIDL.

---

## N12.4 — VDI scaffolding (no logic yet) ✅ DONE (2026-05-20)

Stub VDI built, deployed to device, and verified loading correctly.
hilog on the live device shows:

```
[I] CAMERA_VDI: HybrisCameraHostVdiImpl ctor
[I] CAMERA_VDI: VDI registered, cameraIds=lcam001
[I] CAMERA: HCameraHostManager::AddCameraHost camera host camera_service added
```

Source layout (committed):

- `device/soc/oniro/hybris_generic/hardware/camera/BUILD.gn` — builds
  `libcamera_host_vdi_impl_1.0.z.so` (matches `vdiLibList` in
  `camera_host_config.hcs`).
- `include/hybris_camera_log.h` — `CAMERA_VDI` hilog tag wrappers.
- `src/camera_host/hybris_camera_host_vdi_impl.{h,cpp}` — minimal
  `ICameraHostVdi` impl:
  - `GetCameraIds()` returns `["lcam001"]` matching the HCS template
  - `GetCameraAbility()` returns `NO_ERROR` with empty blob (HCS
    supplies the static ability)
  - `OpenCamera()` / `SetFlashlight()` return `METHOD_NOT_SUPPORTED`
  - `HDF_VDI_INIT(g_vdiCameraHost)` exports the `hdfVdiDesc` symbol
    that `HdfLoadVdi()` looks up after `dlopen`

Wiring (committed):

- `device/soc/oniro/hybris_generic/BUILD.gn` — added
  `hardware/camera:camera_host_model` to `hybris_generic_soc_group`.
- `device/soc/oniro/hybris_generic/bundle.json` — added
  `drivers_interface_camera` to component deps.
- `vendor/oniro/hybris_generic/hdf_config/uhdf/BUILD.gn` — added
  `hc_gen` + `ohos_prebuilt_etc` rules to compile and install
  `camera_host_config.hcs` (+ pipeline_core/*.hcs) as `.hcb` files
  to `/vendor/etc/hdfconfig/`.  Without these, camera_host_service
  logged `file /vendor/etc/hdfconfig/camera_host_config.hcb is
  invalid` and never invoked our VDI.

Build command:

```sh
sudo docker exec -u root -w /home/openharmony/workdir 0bb7ce2c1ccc \
    ./build.sh --product-name hybris_generic --ccache \
        --build-target libcamera_host_vdi_impl
```

Deploy for incremental testing (avoids full super.img rebuild):

```sh
hdc shell "mount -o remount,rw /vendor"
hdc file send out/hybris_generic/oniro_soc_products/hybris_generic_soc/libcamera_host_vdi_impl_1.0.z.so \
              /vendor/lib64/libcamera_host_vdi_impl_1.0.z.so
hdc shell "chmod 0755 /vendor/lib64/libcamera_host_vdi_impl_1.0.z.so"

# Also need the HCB (until next vendor.img rebuild):
drivers/hdf_core/framework/tools/hc-gen/build/hc-gen -b -i \
    -o /tmp/camera_host_config.hcb \
    vendor/oniro/hybris_generic/hdf_config/uhdf/camera/hdi_impl/camera_host_config.hcs
hdc file send /tmp/camera_host_config.hcb /vendor/etc/hdfconfig/camera_host_config.hcb

# Restart camera_host (kill triggers init respawn):
hdc shell "killall camera_host"
hdc shell "hilog 2>&1 | grep CAMERA_VDI | head"
# Expected: "VDI registered, cameraIds=lcam001"
```

**Pitfalls encountered + lessons:**

1. **`drivers_interface_camera` dep:** had to be added to
   `device/soc/oniro/hybris_generic/bundle.json` `components` list,
   otherwise `check_build_target.py` rejected the build with
   `depend part drivers_interface_camera, need set part deps info`.
2. **Include path for VDI interfaces:** `icamera_host_vdi.h` uses
   `#include "v1_0/icamera_device_vdi.h"` style, so the parent
   `vdi_base/interfaces` dir must be on the include path, not just
   `vdi_base/interfaces/v1_0`.
3. **`hdf_load_vdi.h` lives in
   `drivers/hdf_core/interfaces/inner_api/host/uhdf/`** — that
   directory must be added to `include_dirs`; it's not pulled in
   by `hdf_core:libhdi` external_dep.
4. **HCS doesn't auto-compile.**  Adding the HCS file to the source
   tree isn't enough — you need explicit `hc_gen` +
   `ohos_prebuilt_etc` build rules.  Without the `.hcb` file at
   `/vendor/etc/hdfconfig/camera_host_config.hcb`, camera_host_service
   logs `hcb file is invalid` and the VDI is never loaded
   (silently — no error in camera_host hilog).

### Original plan recorded for reference (pre-execution)

**Goal.** Empty stub `libcamera_host_vdi_impl_1.0.z.so` that registers
with HDF and returns canned data for every method.  Lets us validate
the load path (HCS → `camera_host` HDF host → `dlopen` VDI → method
dispatch) before the harder work.

### Source layout

```
device/soc/oniro/hybris_generic/hardware/camera/
├── BUILD.gn
├── include/
│   ├── camera_common.h         ← format/usage tables, stride helpers (cf. display_common.h)
│   └── hybris_camera_log.h     ← CAMERA_VDI hilog tag
└── src/
    ├── camera_host/
    │   ├── hybris_camera_host_vdi_impl.cpp   ← ICameraHostVdi
    │   ├── hybris_camera_host_vdi_impl.h
    │   └── camera_provider_client.cpp        ← libhybris-loaded HIDL client
    ├── camera_device/
    │   ├── hybris_camera_device_vdi_impl.cpp ← ICameraDeviceVdi
    │   └── hybris_camera_device_vdi_impl.h
    ├── stream_operator/
    │   ├── hybris_stream_operator_vdi_impl.cpp ← IStreamOperatorVdi
    │   ├── hybris_stream_operator_vdi_impl.h
    │   ├── hybris_stream.cpp                ← per-stream state, buffer tracking
    │   └── hybris_capture_request.cpp       ← OHOS→HIDL request marshalling
    ├── metadata/
    │   ├── metadata_translator.cpp          ← Android↔OHOS tag remap (~80 tags)
    │   └── metadata_translator.h
    └── buffer/
        ├── hybris_buffer_handle.cpp         ← BufferHandle↔native_handle_t
        └── hybris_buffer_handle.h
```

The shape directly mirrors `device/soc/oniro/hybris_generic/hardware/display/`:

- a `camera_host/` mirroring `display_composer/` for the host-level VDI;
- a `camera_device/` analogue (display has no per-device split because there's
  only one composer);
- a `stream_operator/` mirroring nothing in display (display's atomic
  "commit layers" replaces this);
- shared `buffer/` and `metadata/` utilities.

### BUILD.gn shape

Mirrors `display/BUILD.gn`:

```gn
import("//build/ohos.gni")
import("//drivers/hdf_core/adapter/uhdf2/uhdf.gni")

group("camera_host_model") {
  deps = [ ":libcamera_host_vdi_impl" ]
}

ohos_shared_library("libcamera_host_vdi_impl") {
  sources = [
    "src/camera_host/hybris_camera_host_vdi_impl.cpp",
    "src/camera_host/camera_provider_client.cpp",
    "src/camera_device/hybris_camera_device_vdi_impl.cpp",
    "src/stream_operator/hybris_stream_operator_vdi_impl.cpp",
    "src/stream_operator/hybris_stream.cpp",
    "src/stream_operator/hybris_capture_request.cpp",
    "src/metadata/metadata_translator.cpp",
    "src/buffer/hybris_buffer_handle.cpp",
  ]
  include_dirs = [
    "include",
    "//third_party/libhybris/hybris/include",
    "//third_party/android-headers",
    "//drivers/peripheral/camera/vdi_base/interfaces/v1_0",
    "//drivers/interface/camera/v1_0",
    "//drivers/interface/camera/metadata/include",
    "//drivers/peripheral/display/utils/include",   # for BufferHandle helpers
  ]
  deps = [
    "//drivers/hdf_core/adapter/uhdf2/utils:libhdf_utils",
    "//drivers/interface/camera/v1_0:libcamera_proxy_1.0",
    "//drivers/interface/camera/metadata:metadata",
    "//third_party/libhybris:libhybris",
  ]
  external_deps = [
    "hilog:libhilog",
    "ipc:ipc_single",
    "hdf_core:libhdi",
    "graphic_2d:libsurface",
  ]
  install_images = [ chipset_base_dir ]
  subsystem_name = "oniro"
  part_name      = "device_hybris_generic"
}
```

### Wiring into device build

`device/soc/oniro/hybris_generic/BUILD.gn`'s existing
`hybris_generic_soc_group` adds:

```gn
deps += [ "hardware/camera:camera_host_model" ]
```

The HDF host glue (`camera_host` HDF host that dlopens the VDI) is
already wired in `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs`
(verified — `moduleName = "libcamera_host_service_1.0.z.so"`, and the
matched `camera_host_config.hcs` already points
`vdiLibList = ["libcamera_host_vdi_impl_1.0.z.so"]`).  Nothing to add
on the HDF side.

### Stub semantics

Until N12.5–N12.12 fill in real behaviour, every method returns:

- `GetCameraIds(...)` → `["lcam001"]` (matches existing HCS template
  so the demo apps don't blow up immediately).
- `GetCameraAbility(...)` → HCS-supplied static metadata (handled by
  the OHOS host service via HCS — VDI returns NO_ERROR + empty).
- `OpenCamera(...)` → returns NO_ERROR + a stub `ICameraDeviceVdi`.
- Everything else → `METHOD_NOT_SUPPORTED`.

This gets us a `libcamera_host_vdi_impl_1.0.z.so` that *loads* and the
host log shows the VDI registered with HDF — a milestone in itself,
because it proves the HCS→host→VDI chain is correctly wired before
the real bridging code touches the device.

### Exit criterion for N12.4

```
hdc shell hilog | grep CAMERA_VDI
# expect: "[I] libcamera_host_vdi_impl: VDI registered, cameraIds=lcam001"
```

---

## N12.5 — HIDL client bridging (the load-bearing step)

**Goal.** From inside the OHOS-side VDI (`composer_host`-sibling
process, uid `camera_host`), call
`android::hardware::camera::provider::V2_x::ICameraProvider::getService`
over the shared `/dev/hwbinder` and successfully invoke
`getCameraIdList`.

### Approach

Same pattern N8 already uses for `IComposer`:

1. `dlopen` the libhybris linker namespace.  It loads bionic
   `libc.so` + Halium's `libhidlbase.so`,
   `libhidltransport.so`, `libcamera_metadata.so`, and the
   per-version generated stub
   `android.hardware.camera.provider@2.5.so`.
2. From the OHOS process, call into those libs through libhybris's
   symbol forwarding.  Libhybris already remaps `/vendor` →
   `/android/vendor` and `/system/lib64` → `/android/system/lib64`
   (verified by N8.1), so the linker finds everything.
3. The HIDL stub talks over hwbinder which both namespaces share —
   the Halium HAL's binder objects are visible OHOS-side identically.

The composer / allocator path already proves the bionic-libc-loaded-in-a-musl-process
pattern works without crashing.  Camera reuses it.

### Concrete: `camera_provider_client.cpp` skeleton

```cpp
// Pseudocode — illustrates the bridge shape.
//
// libhybris exposes hybris_dlopen()/hybris_dlsym() which use the
// bionic linker for /android/system/lib64 paths.  Anything we
// dlsym out of an Android .so is a bionic-compiled symbol — call it
// through wrappers that match Android's calling convention.

#include <hybris/common/binding.h>
#include <hybris/common/binding_helpers.h>

namespace OHOS::Camera::Hybris {

using getService_t = sp<IBinder>(*)(const char *, const char *);

struct CameraProviderClient {
    void* hidlbase   = nullptr;
    void* providerSo = nullptr;
    sp<ICameraProvider> provider;

    bool Connect() {
        hidlbase = hybris_dlopen("/android/system/lib64/libhidlbase.so",  RTLD_NOW);
        if (!hidlbase) return false;

        providerSo = hybris_dlopen(
            "/android/system/lib64/android.hardware.camera.provider@2.5.so", RTLD_NOW);
        if (!providerSo) return false;

        // getService is the C++-generated convenience; through hybris
        // it's HIDL's C-ABI BpHwCameraProvider::tryGetService.
        provider = ICameraProvider::tryGetService("internal/0");
        return provider != nullptr;
    }

    Status GetCameraIdList(std::vector<std::string>& out) {
        auto rc = provider->getCameraIdList([&](auto status, auto& ids) {
            for (auto& id : ids) out.push_back(id);
        });
        return rc.isOk() ? Status::OK : Status::TRANSPORT_FAILED;
    }
    // … getVendorTags, setCallback, getCameraDeviceInterface_V3_x, setFlashlight
};

} // namespace
```

### Pitfalls — anticipate

- **HIDL version mismatch.**  Halium 12 ships `@2.5` (or `@2.4`/`@2.6`
  depending on patch level).  Our header pin must match — using `@2.4`
  headers against a `@2.5` provider crashes deep inside the HIDL stub
  with bad-method-dispatch.  Pin to whichever version N12.1 discovery
  observed, and rev the import when Halium is rev'd.
- **`tryGetService` returns null silently.**  HIDL's stub treats
  "service not registered yet" identically to "transport broken".
  Add an explicit poll-with-timeout, same shape as N4's composer-ready
  watchdog.  Don't proceed past `OpenCamera` until the provider has
  pumped at least one `cameraDeviceStatusChange` callback (proves the
  HAL is live, not just registered).
- **Property store sharing (N4.1 lesson).**  The Halium camera HAL
  probably also reads `ro.camera.*` and `persist.camera.*` properties
  via bionic libc.  Our existing tmpfs bind at `/dev/__properties__/`
  (shared between the two namespaces) already covers this — but
  verify the camera HAL doesn't `setprop` from inside its own
  namespace expecting the change to be visible OHOS-side (it shouldn't,
  but log a `inotify` on the property store during early bring-up to
  prove it).
- **Bionic-vs-musl ABI on callback structs.**  The HIDL callbacks are
  C++ virtual methods.  Hybris's vtable forwarding works because the
  callback object is *implemented* on the Android side; our `sptr`
  hold and refcount go through hybris's wrapper.  We do NOT implement
  the callback directly in OHOS C++ — instead we have a tiny
  bionic-compiled shim under `/android/...` that implements
  `ICameraProviderCallback` and forwards into our C-ABI dispatch.
  Done the same way as `composer_host`'s display callback.
- **Threading.**  HIDL callbacks land on hwbinder threads from a pool
  inside the Android linker namespace.  These threads have bionic
  TLS, not OHOS musl TLS — calling OHOS APIs (hilog, IPC) from them
  WILL crash.  Marshal to an OHOS thread via a lock-free SPSC queue
  in the VDI before invoking OHOS-side handlers.  Same pattern as
  composer_host's `eventCallback_` already does.

### Verification

```
hdc shell hilog -t CAMERA_VDI
# expect:
#  [I] Connecting to ICameraProvider/internal/0
#  [I] ICameraProvider/internal/0 connected, version=2.5
#  [I] getCameraIdList returned [0, 1]
```

---

## N12.6 — Camera enumeration + ability metadata

**Goal.** `ICameraHostVdi::GetCameraIds` returns the actual sensor IDs,
and `GetCameraAbility` for each returns a metadata blob that the OHOS
camera framework + Camera HAP both accept.

### Sequence

1. `provider->getCameraIdList(...)` → e.g. `["0", "1"]`.
2. For each id call `provider->getCameraDeviceInterface_V3_x(id, ...)`
   → `ICameraDevice`.
3. `device->getCameraCharacteristics(...)` returns a HIDL
   `CameraMetadata` (which is a `hidl_vec<uint8_t>` wrapping the
   tagged-blob format).
4. Translate Android `camera_metadata_t` → OHOS
   `camera_metadata_operator`-compatible blob via the
   `metadata_translator` module.
5. Return blob to the OHOS camera framework via `GetCameraAbility`.

### Tag translation strategy

The two metadata systems diverge in:

- **Tag IDs.**  Android `ANDROID_LENS_FACING` (tag `0x00080000`) and
  OHOS `OHOS_ABILITY_CAMERA_POSITION` are different numeric IDs even
  though they convey the same thing.  A static table maps known tag
  pairs.
- **Tag value vocabulary.**  `ANDROID_LENS_FACING_FRONT = 0`,
  `OHOS_CAMERA_POSITION_FRONT = 1`.  Per-tag value-mapper functions
  in the table.
- **Vendor-specific tags.**  Android lets vendors add tags above
  `0x80000000`; OHOS likewise.  Most are ignored by the framework
  unless an app explicitly asks for them.  Drop unknown vendor tags
  in the first pass.

Table lives in `metadata/metadata_translator.h`:

```cpp
struct TagMapping {
    uint32_t androidTag;
    uint32_t ohosTag;
    // Convert one HIDL camera_metadata value to its OHOS counterpart.
    void (*convert)(const camera_metadata_ro_entry_t&, std::vector<uint8_t>&);
};

extern const TagMapping kTagMap[];   // ~80 entries — see implementation
```

Start with the ~80 keys listed in the current
`camera_host_config.hcs` `availableCharacteristicsKeys` /
`availableRequestKeys` / `availableResultKeys`.  The HCS metadata
serves as our integration target; if the translator emits a blob with
those keys populated, the OHOS framework is happy.

### Verification

```
hdc shell hilog -t CAMERA_VDI
# expect, on enumeration:
#  [I] Camera 0: rear, orientation=90, resolutions=[(1920x1080) (1280x720) ...]
#  [I] Camera 1: front, orientation=270, resolutions=[(640x480) (1280x960)]
hdc shell "/system/bin/camera_dump 0"   # OHOS in-tree camera dump tool
# expect: prints the translated OHOS metadata blob
```

### Pitfalls

- **HAL-level vs framework-level keys.**  Android exposes a *superset*
  of keys at the HAL boundary; the framework filters before exposing
  to apps.  Our VDI is at the HAL boundary equivalent — we get
  everything.  Be selective; passing through unsupported keys
  triggers OHOS framework log spam at best, crash at worst.
- **Multi-camera composer logical IDs.**  Android lets one logical
  camera back several physical sensors (multi-cam phones).  X23 is
  almost certainly single-camera-per-id; flag and refuse if
  `physicsCameraIds` length > 1 in the first cut.

---

## N12.7 — Open camera → ICameraDeviceSession

**Goal.** `OpenCamera` returns a `sptr<ICameraDeviceVdi>` whose
methods are wired to a live Halium `ICameraDeviceSession`.

### Sequence

1. `ICameraHostVdi::OpenCamera(id, callback, &device)` →
2. VDI calls `provider->getCameraDeviceInterface_V3_x(id, ...)`
3. VDI calls `cameraDevice->open(localCallback)` →
   `ICameraDeviceSession` HIDL handle.
4. Wrap session in `HybrisCameraDeviceVdi` (OHOS class).
5. Local `ICameraDeviceCallback` (bionic-side shim) forwards
   `processCaptureResult` / `notify` to OHOS-side
   `IStreamOperatorVdiCallback` via the thread-marshal queue from
   N12.5.

### Callback marshalling

Android `ICameraDeviceCallback`:

```
processCaptureResult(uint64 frameNumber, CaptureResult result);
notify(NotifyMsg msg);  // {error, shutter}
requestStreamBuffers(...);   // 2.6+ only — needed if we use HAL buffer manager
returnStreamBuffers(...);    // 2.6+ only
```

OHOS `IStreamOperatorVdiCallback`:

```
OnCaptureStarted(int32 captureId, vector<int32> streamIds, uint64 timestamp);
OnCaptureEnded(int32 captureId, vector<CaptureEndedInfo>);
OnCaptureError(int32 captureId, vector<CaptureErrorInfo>);
OnFrameShutter(int32 captureId, vector<int32> streamIds, uint64 timestamp);
OnResult(int32 captureId, vector<uint8> result);
```

Bridge function table mapping each direction; defer the 2.6 buffer-
manager methods until N12.9 buffer interop demands them.

### Pitfalls

- **`close()` race.**  Calling
  `ICameraDeviceSession::close()` while a HIDL callback thread is
  mid-call: hangs.  Drain the marshal queue + cancel inflight
  HIDL operations before close.  Add `Close()` reentrancy guard.
- **Camera-already-open errors.**  Halium HAL allows only one open
  session per camera id; second `open` returns `CAMERA_IN_USE`.
  Surface as `VdiCamRetCode::CAMERA_BUSY`.
- **uid permissions.**  `camera_host` uid (declared in
  `device_info.hcs`) needs `CAP_SYS_ADMIN` *no* — but it does need
  read access to the bionic-libc-loaded paths.  Mirror
  `composer_host` cfg's caps + selinux skip.

---

## N12.8 — Stream configuration

**Goal.** OHOS framework's `CreateStreams` results in a Halium
`configureStreams_3_4` call that allocates buffers in formats we both
agree on.

### Format / usage translation table

Both sides use enum-tagged integers.  Build a static table.

| OHOS PixelFmt (`drivers/interface/display/composer/v1_0/include/idisplay_composer.h`) | Android HAL_PIXEL_FORMAT_* | Use |
|---|---|---|
| `PIXEL_FMT_YCBCR_420_SP` (NV12) | `HAL_PIXEL_FORMAT_YCBCR_420_888` | preview, video record |
| `PIXEL_FMT_YCRCB_420_SP` (NV21) | `HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED` (often NV21 on MTK) | preview (alt) |
| `PIXEL_FMT_RGBA_8888` | `HAL_PIXEL_FORMAT_RGBA_8888` | preview to surface |
| `PIXEL_FMT_BLOB` (JPEG bytes) | `HAL_PIXEL_FORMAT_BLOB` | still capture |
| `PIXEL_FMT_RAW10` | `HAL_PIXEL_FORMAT_RAW10` | RAW (deferred to Milestone 2) |

Usage flags:

| OHOS HBM_USE_* | Android GRALLOC_USAGE_* |
|---|---|
| `HBM_USE_CPU_READ` | `GRALLOC_USAGE_SW_READ_OFTEN` |
| `HBM_USE_HW_TEXTURE` | `GRALLOC_USAGE_HW_TEXTURE` |
| `HBM_USE_HW_COMPOSER` | `GRALLOC_USAGE_HW_COMPOSER` |
| `HBM_USE_HW_VIDEO_ENCODER` | `GRALLOC_USAGE_HW_VIDEO_ENCODER` |
| `HBM_USE_HW_CAMERA_WRITE` | `GRALLOC_USAGE_HW_CAMERA_WRITE` |

### Stream object lifetime

Per-stream OHOS state object (`HybrisStream`) tracks:

- streamId (OHOS), HAL streamId (Android — what HIDL assigns in
  `HalStreamConfiguration`)
- Buffer ring: a deque of (BufferHandle, native_handle_t) pairs
- Producer queue handle (`BufferProducerSequenceable`) for the
  OHOS-side consumer
- State: idle / configured / capturing / closing

`CommitStreams` calls `session->configureStreams_3_4(...)`; the
returned `HalStreamConfiguration` carries
`producerUsage`/`consumerUsage`/`maxBuffers` — reflect them into
`VdiStreamAttribute` so the OHOS framework can allocate the right
buffers.

### Pitfalls

- **`IMPLEMENTATION_DEFINED` is platform-specific NV21 on MTK** but
  AOSP frequently labels it NV12.  Don't trust the enum; ask the HAL
  via `getCameraCharacteristics` → `SCALER_AVAILABLE_STREAM_CONFIGURATIONS`.
- **`configureStreams_3_4` rejects sets it can't compose.**  Common:
  preview YUV + still BLOB + video YUV requires
  `maxStreamCount=3` and a high-resolution still that exceeds
  bandwidth.  Fall back to 2-stream (preview + still) initially;
  Milestone 1 doesn't need a video stream.

---

## N12.9 — Buffer interop (the hardest piece)

**Goal.** OHOS BufferHandle ↔ Android `native_handle_t` round-trip
without copying, with fences preserved.

### Why it's hard

Both sides allocate via the *same* Android gralloc HAL (we already
go through `allocator_host`, which loads Halium's gralloc via
libhybris — see Phase N8).  So the underlying DMA-BUF fd is identical;
the difference is just the wrapper struct.

- OHOS `BufferHandle` (HDF gralloc):
  ```c
  struct BufferHandle {
      uint32_t reserveInts;
      int32_t fd;
      int32_t width, stride /*BYTES*/, height, size, format, usage;
      uint64_t phyAddr;
      void *virAddr;
      int32_t key;
      uint32_t reserve[0];
  };
  ```
- Android `native_handle_t`:
  ```c
  typedef struct {
      int version;
      int numFds;       // typically 1 — the DMA-BUF fd
      int numInts;      // ~10–20 ints carrying width/height/format/usage/stride/etc
      int data[];       // fds first, then ints
  } native_handle_t;
  ```

The Android gralloc-allocated handle carries vendor-specific layout in
`data[numFds..]` — we **must not reinterpret** the int fields; we
only need to:

1. Open the DMA-BUF fd (data[0]) on the OHOS side and wrap it in an
   OHOS BufferHandle with the metadata we already know from the
   stream config (width/height/format/stride).
2. To pass an OHOS-allocated buffer to the HAL, do the inverse:
   construct an Android `native_handle_t` whose `data[0]` is our
   DMA-BUF fd and whose ints are the layout fields the HAL expects.

The "metadata we already know" is the trick: the HAL needs the
*exact* gralloc-private layout to interpret the buffer.  Two options:

- **A. Allocate exclusively through `allocator_host` (recommended).**
  `allocator_host`'s gralloc-mapper *is* Halium's; its
  `IAllocator::allocate` returns an Android `native_handle_t` that
  the camera HAL accepts identically.  We just have to track that
  handle in the `HybrisStream` and unwrap when crossing the OHOS
  boundary.
- **B. Allocate OHOS-side then `importBuffer` into the Android
  gralloc.**  More flexible but riskier — the Android gralloc may
  reject buffers it didn't allocate.  Try only if A doesn't work.

Adopt A.

### Fence interop

Both Android and OHOS use `sync_file` fds for synchronization (the
Linux kernel `sync_file` API at `drivers/dma-buf/sync_file.c`).  HIDL
marshals them via `hidl_handle`.  Direct fd passthrough works; just
duplicate the fd when crossing the OHOS/Android boundary so neither
side closes the other's reference prematurely.

### Buffer flow

```
preview path (steady-state, repeating request):

  OHOS framework (camera_service_proxy)
       │  dequeueBuffer() from producer surface (display preview surface)
       ▼
  BufferProducerSequenceable.RequestBuffer() → OHOS BufferHandle
       │
       ▼
  HybrisStream::EnqueueRequest() pulls from sptr<HybrisBuffer> ring
       │  matches BufferHandle to its previously-imported native_handle_t
       ▼
  HIDL CaptureRequest.outputBuffers = [StreamBuffer{streamId, buffer=native_handle_t, acquireFence}]
       │
       ▼
  ICameraDeviceSession::processCaptureRequest_3_4(...)
       │
       ▼
  Halium camera HAL writes into the DMA-BUF (ISP DMA), attaches release_fence
       │
       ▼
  ICameraDeviceCallback::processCaptureResult(...) - on Android binder thread
       │  marshal to OHOS thread via SPSC queue (N12.5)
       ▼
  HybrisStream::OnBufferReady(handle, releaseFence)
       │
       ▼
  IStreamOperatorVdiCallback::OnCaptureEnded + producer.FlushBuffer(BufferHandle, releaseFence)
       │
       ▼
  OHOS framework consumes — display composer reads it as a layer.
```

### Pitfalls

- **Stride confusion.**  Already in memory:
  `project_ohos_bufferhandle_stride.md`.  OHOS stride is BYTES;
  Android gralloc returns PIXELS.  Helper in `display_common.h` —
  reuse for camera buffers.
- **dup-the-fd discipline.**  Fences and DMA-BUF fds cross binder
  *as references*.  Crossing the hybris bionic/musl boundary, the fd
  count must be balanced: dup on entry, close on exit, never share an
  fd between the two libc heaps' file-descriptor tables.
- **Camera HAL may impose buffer-count minimums.**  HIDL
  `HalStreamConfiguration.maxBuffers` is per stream; some MTK HALs
  insist on 6+ for preview to maintain pipeline depth.  Pre-allocate
  generously.
- **OHOS surface re-attachment.**  When the camera app pauses /
  resumes, the OHOS preview surface may invalidate its buffer queue;
  the VDI must detect (via callback to `DetachBufferQueue`) and
  flush all in-flight requests so HAL-held buffers are released
  before the consumer disappears.

---

## N12.10 — Preview path (Milestone-1 viewfinder)

**Goal.** Camera app from the OHOS distro renders a live preview from
camera 0 at ≥24 fps to the display.

### Path

1. App calls OHOS Camera APIs (JS-side) → camera_service → HDI →
   `camera_host` → our VDI.
2. VDI creates a single PREVIEW stream
   (1280×720 NV12 or 1920×1080 NV12 — pick the lowest size first).
3. Configure stream via N12.8; allocate ring of 6 buffers (N12.9 A).
4. Submit a repeating CaptureRequest with
   `OHOS_CONTROL_AE_MODE = OHOS_CONTROL_AE_MODE_ON`,
   `OHOS_CONTROL_AWB_MODE = OHOS_CONTROL_AWB_MODE_AUTO`.
5. Frame results arrive ~30 ms apart; deliver buffers to the
   producer surface as they come.

### Verification

- Camera HAP launches; preview surface shows live image (not green/black).
- `hilog -t CAMERA_VDI -t CAMERA_FWK` shows no errors over 30 seconds.
- `dumpsys` (Android side) shows non-zero frame count.

### Pitfalls (anticipate)

- **First frame is junk** (sensor warm-up).  Drop first 2–3 frames.
- **Front camera mirror.**  OHOS framework typically mirrors front
  preview at the consumer; HAL doesn't.  Verify mirror direction
  matches user expectation (selfie should be mirrored).
- **Tearing under composition.**  If render_service composites the
  preview into a layer at 60 Hz and we feed at 30 Hz, the layer
  shows the same frame twice — fine.  If we feed at 24 Hz, judder
  visible.  Match preview frame rate to display VSYNC where possible
  (FPS range 24–30 typical).
- **Mali NULL+0x1d8 dropdown analogue.**  When the camera HAP exits,
  Mali EGL contexts created for the preview surface tear down.  The
  Phase 8.17 Mali bug *will* recur in this path.  Document as
  "ride-along" of N8 stability bug; don't try to fix in N12.

---

## N12.11 — Still capture (JPEG)

**Goal.** Tap shutter → JPEG file appears in gallery with correct
orientation.

### Path

1. App calls `takePicture()` → OHOS framework → VDI.
2. VDI adds a JPEG_BLOB stream (4 MB capacity for ~12 MP JPEG) to
   the existing preview stream — needs `configureStreams` re-commit.
   *Alternative: pre-add the JPEG stream at session start; less
   re-config overhead but uses memory while idle.*
3. Submit a non-repeating CaptureRequest for the JPEG stream with
   `OHOS_CONTROL_CAPTURE_INTENT = OHOS_CONTROL_CAPTURE_INTENT_STILL_CAPTURE`
   + JPEG_ORIENTATION/JPEG_QUALITY metadata.
4. HAL produces JPEG-encoded buffer in the BLOB stream
   (Android camera HAL has an internal JPEG encoder for BLOB
   streams; we don't need to encode).
5. Buffer arrives via `processCaptureResult`; VDI hands the byte
   stream to the OHOS framework which writes it to the gallery.

### Metadata passthrough

Critical tags for still capture:

| Direction | OHOS tag | Android tag |
|---|---|---|
| ← request | `OHOS_JPEG_ORIENTATION` | `ANDROID_JPEG_ORIENTATION` |
| ← request | `OHOS_JPEG_QUALITY` | `ANDROID_JPEG_QUALITY` |
| ← request | `OHOS_JPEG_THUMBNAIL_QUALITY` | `ANDROID_JPEG_THUMBNAIL_QUALITY` |
| ← request | `OHOS_JPEG_GPS_COORDINATES` | `ANDROID_JPEG_GPS_COORDINATES` |
| ← request | `OHOS_JPEG_GPS_TIMESTAMP` | `ANDROID_JPEG_GPS_TIMESTAMP` |
| → result | `OHOS_JPEG_SIZE` | `ANDROID_JPEG_SIZE` |

### Pitfalls

- **Shutter sound.**  OHOS framework owns it; we just have to fire
  `OnFrameShutter` on time (when the sensor exposure starts).
- **HAL-internal JPEG encoder may be slow (>200 ms on MTK).**  That's
  the HAL; nothing the VDI can do.  Latency target: <500 ms shutter
  → file.  If we miss, milestone 1 still ships — note for tuning.

---

## N12.12 — Flashlight + flash mode

**Goal.** Flashlight toggle from OHOS settings works; per-capture
flash mode honoured.

### Two distinct APIs

- **Standalone torch:** `ICameraHostVdi::SetFlashlight(cameraId, enable)`
  → `ICameraProvider::setTorchMode(cameraId, enable)`.  Direct
  passthrough.
- **Capture-time flash:** OHOS `OHOS_FLASH_MODE` metadata →
  Android `ANDROID_FLASH_MODE` + `ANDROID_CONTROL_AE_MODE_*_FLASH`.
  Translation table in `metadata_translator`.

### Pitfalls

- **Torch conflicts with open camera.**  Some MTK HALs reject torch
  while camera is open; gate the OHOS-side `SetFlashlight` on no
  active session for that id, or close-and-reopen if needed.
- **Front cameras typically don't have a flash.**  Honour
  `FLASH_INFO_AVAILABLE = false` in metadata so OHOS UI hides the
  toggle.

---

## N12.13 — HCS metadata sync

**Goal.** `camera_host_config.hcs` carries the real-sensor static
abilities (resolutions, FPS ranges, AF/AE/AWB modes), not the generic
template currently in tree.

### Why HCS instead of pure VDI

OHOS Camera HDI host queries HCS for *static* ability when an app
calls `getCameraAbility` — the VDI's runtime translation only runs
on `open`.  Both must agree, or the app sees discrepancies (e.g. UI
shows a resolution the HAL refuses).

### Approach

1. After N12.6 lands, dump real metadata via a small helper:
   ```sh
   /system/bin/camera_dump --hcs 0 > /tmp/cam0.hcs
   ```
   (Helper to be written; reads `getCameraCharacteristics` blob and
   emits HCS-shaped text.)
2. Replace
   `vendor/oniro/hybris_generic/hdf_config/uhdf/camera/hdi_impl/camera_host_config.hcs`
   `ability_01` block with values from `cam0.hcs`.
3. Repeat for `ability_02` (front camera).

### Pitfalls

- **HCS is static; sensors aren't.**  Hot-pluggable USB cameras
  aren't an X23 concern.  Document that HCS rebuild is a build-time
  task; runtime swap unsupported in Milestone 1.

---

## N12.14 — Test harness + bring-up checklist

### Smoke tools, in order of use

1. **`hilog -t CAMERA_VDI -t CAMERA_FWK`** — primary debug stream.
2. **`/system/bin/camera_dump <cameraId>`** — VDI metadata dump tool
   (to be written; tiny OHOS executable that calls
   `ICameraHost::GetCameraAbility` + prints the blob).
3. **`/system/bin/camera_demo`** — in-tree v4l2 demo
   (`drivers/peripheral/camera/test/...`) — preview to file, no
   display dep.  Tweak HCS to point its VDI at us; this isolates
   VDI bugs from display-pipeline bugs.
4. **OHOS Camera HAP** — visual confirmation.
5. **`dumpsys media.camera` (Halium NS)** — Android-side view of
   the same session.  Cross-check capture stats.

### Per-subphase pass criteria

| Subphase | Pass when |
|---|---|
| N12.2 | `lsmod | grep -E '(cam|seninf|imgsensor)'` lists all bundled `.ko`; `/dev/video*` present; no `-EPROBE_DEFER` loops in dmesg |
| N12.4 | `hilog` shows `libcamera_host_vdi_impl` registration; `camera_dump 0` returns stub data |
| N12.5 | `hilog` shows `ICameraProvider connected, version=2.x`; `getCameraIdList` returns ≥1 id |
| N12.6 | `camera_dump 0` returns real metadata blob (resolutions, orientation, lens facing) |
| N12.7 | `camera_dump 0` reports `OpenCamera` success; `ICameraDeviceSession` lifetime tracked in hilog |
| N12.8 | `camera_demo --preview-only` configures a 1280×720 NV12 stream without error |
| N12.9 | `camera_demo --preview-only` receives ≥10 buffers in 1 s; no fence-leak warnings |
| N12.10 | Camera HAP shows live preview, both cameras switchable |
| N12.11 | Camera HAP shutter → JPEG visible in gallery with correct orientation |
| N12.12 | Settings → torch toggle works; HAP flash mode auto/on/off all honour |

---

## N12.15 — Stability + teardown

Camera-specific stability concerns to track, in the same vein as the
Phase 8 graphics bugs:

- **Session-close while inflight.**  Drain logic in N12.7 must hold
  under realistic loads (camera HAP killed via swipe-up while
  preview running).
- **Re-configureStreams under load.**  Adding the JPEG stream
  mid-preview (N12.11 alt path) is a race; needs careful sequencing.
- **HAL crash → service restart.**  If
  `android.hardware.camera.provider@2.x` SIGSEGVs inside the
  Halium NS, Halium init restarts it but our VDI's HIDL `sptr` is
  now invalid.  Add death-recipient (`linkToDeath`) that drops the
  cached provider + replays open requests; covered by the inherited
  `CameraHostCallBackDeathRecipient` skeleton in the OHOS VDI
  interface.
- **Mali UAF analogue.**  Closing camera while a preview frame is
  still being composed by `render_service` *might* trigger the same
  EGL teardown race as Phase 8.17 (the dropdown crash).  Same
  class of bug; defer to a unified GPU-stability task.

These don't block Milestone 1 — they show up as `≤1 crash / day`
class issues like the existing Phase 8 ones — but track separately.

---

## N12.16 — `mimir` tablet variant

The Volla Tablet (`mimir`, MT8781) shares the Helio G99 ISP gen with
the X23 (MT6789).  Carry-overs and deltas:

- **Likely identical**: ISP / `seninf` / `imgsensor` framework
  modules; VDI source; metadata translator; buffer interop; HIDL
  bridging.
- **Probably different**: per-unit sensor `.ko` (mimir's sensors are
  bigger pixel pitch, higher resolution); HCS metadata
  (resolutions/FPS).  Re-run N12.1 inventory + N12.13 sync.
- **Maybe different**: Android-12 vs Android-13 HIDL version
  (mimir's vendor blobs may ship `@2.6` or `@2.7`).  Pin HIDL imports
  per-product if needed.

Mirror the existing `legacy_volla_tablet_mimir.md` documentation
shape: a single-page diff against the X23 plan.

---

## Source-tree deliverables (target)

| Path | Purpose |
|---|---|
| `device/soc/oniro/hybris_generic/hardware/camera/` | OHOS VDI tree (Camera HDI ↔ Halium HIDL bridge) |
| `device/soc/oniro/hybris_generic/hardware/camera/BUILD.gn` | builds `libcamera_host_vdi_impl_1.0.z.so` |
| `device/soc/oniro/hybris_generic/hardware/camera/include/camera_common.h` | format/usage tables, stride helpers |
| `device/soc/oniro/hybris_generic/hardware/camera/src/camera_host/` | `ICameraHostVdi` impl + HIDL `ICameraProvider` client |
| `device/soc/oniro/hybris_generic/hardware/camera/src/camera_device/` | `ICameraDeviceVdi` impl |
| `device/soc/oniro/hybris_generic/hardware/camera/src/stream_operator/` | `IStreamOperatorVdi` impl + per-stream + capture-request marshalling |
| `device/soc/oniro/hybris_generic/hardware/camera/src/metadata/` | Android↔OHOS metadata tag translator |
| `device/soc/oniro/hybris_generic/hardware/camera/src/buffer/` | BufferHandle ↔ native_handle_t marshalling |
| `device/board/oniro/hybris_generic/kernel/x23/extra-modules.list` | append MTK camera/ISP/sensor `.ko` |
| `vendor/oniro/hybris_generic/etc/init/init.x23.cfg` | append dep-ordered `insmod` block for camera modules |
| `vendor/oniro/hybris_generic/hdf_config/uhdf/camera/hdi_impl/camera_host_config.hcs` | replace placeholder ability with real X23 sensor metadata |
| `device/board/oniro/docs/hybris_generic/phase_n12_camera_x23_inventory.md` | output of N12.1 (lshal/lsmod/dumpsys) |
| `device/board/oniro/docs/hybris_generic/phase_n12_camera_modulemap.md` | output of N12.1 (dep graph) |
| `device/board/oniro/hybris_generic/launcher/camera_dump.c` (or similar) | small OHOS-side helper for HCS+metadata sanity |

---

## Obstacles & mitigations — summary table

| # | Obstacle | Risk | Mitigation |
|---|---|---|---|
| 1 | Wrong MTK camera kernel modules bundled / wrong order | High | N12.1 inventory walks the device first; cross-check with Halium's own `modules.dep` |
| 2 | HIDL version mismatch crashes deep in stubs | Medium | Pin import to the exact `@2.x` Halium ships; switch via build flag when Halium revs |
| 3 | Bionic-thread → OHOS-API crash in callbacks | Medium | Lock-free SPSC marshal queue between hwbinder threads and OHOS dispatch (same pattern as composer) |
| 4 | Camera HAL transitively needs `cameraserver` / `mediaserver` we don't run | Medium | HIDL provider should be self-contained; if not, add the missing services to Halium init's allowed-services list and pre-bind the deps |
| 5 | Gralloc-handle private layout differs between allocator instances | Medium | Allocate exclusively via `allocator_host` (same gralloc instance as the HAL uses) |
| 6 | Stride byte/pixel confusion | Low | Existing `display_common.h` helper |
| 7 | DMA-BUF fd leaks across boundary | Medium | dup-on-cross, close-on-out discipline; valgrind / `lsof` checks during bring-up |
| 8 | Mali EGL teardown UAF on camera close | Carry-over from Phase 8.17 | Track as part of unified GPU-stability work; not blocking for Milestone 1 |
| 9 | HCS static metadata diverges from runtime VDI metadata | Low-Medium | N12.13 sync step — derive HCS from runtime dump rather than hand-author |
| 10 | Insmod list crosses ~70 → boot black-screen extends | Low | Move kernel-module loading to a `class=core` service once intolerable; out of scope for now |
| 11 | OHOS HDI version (1.0) is older than current OHOS upstream (1.4/1.5) | Low | Wire VDI as `v1.0` to match in-tree HDF host; future-proof by isolating version-specific code in `interfaces/v1_X` files |
| 12 | Front-camera mirror direction wrong | Low | OHOS framework handles selfie-mirror; verify by app testing |

---

## Plan adjustments vs prior native-boot docs

- N9.8 in `phase_n9_firmware_peripherals.md` is downgraded from
  "Out of scope (multi-week project)" to a one-line redirect to this
  doc.
- The README phase index gets a new N12 row.  Cross-link
  `legacy_*.md` references stay as-is — none of them cover camera
  (the LXC build never had a working camera).
- No memory updates required at plan time; once a subphase lands,
  add or update a `project_*` memory in the standard pattern
  (`project_hybris_camera_status.md`).

## Open questions to resolve during N12.1

These are unknowns the discovery phase MUST answer before N12.2 can
be authored concretely:

1. Exact ICameraProvider major.minor — `@2.4`, `@2.5`, or `@2.6`?
2. Exact MTK ISP kernel module set on this Halium build —
   `mtk-cam-isp-7s.ko` vs `mtk-cam-isp-7sp.ko` etc.
3. Number of sensors and IDs (`0`/`1`; some MTK SKUs add `2` for
   depth or `3` for telephoto).
4. Per-sensor `.ko` filenames (driven by the actual fitted sensors).
5. Whether the camera HAL implicitly depends on Halium running
   `cameraserver` (HIDL `setTorchMode` should not; full open path
   should not — but verify).
6. Whether `vendor.camera.*` properties influence HAL behaviour and
   need pre-seeding by `androidd` (similar to N4.2's
   `androidboot.hardware=mt6789`).

Each unknown gets one line in `phase_n12_camera_x23_inventory.md`
once N12.1 is run.
