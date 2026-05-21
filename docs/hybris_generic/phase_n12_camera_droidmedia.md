# Phase N12.D — Camera via droidmedia (pivot from hand-rolled HIDL)

**Status:** 🔄 **N12.D.1–D.5 + production wiring landed (2026-05-21).**
3 cameras enumerate end-to-end through `Droid::Loader`; the new
`HybrisCameraHostVdiImpl` + `HybrisCameraDeviceVdiImpl` +
`HybrisStreamOperatorVdiImpl` replace the hand-rolled HIDL
camera-device path; the obsolete `hw_camera_provider` /
`hw_camera_device` / `hw_camera_device_session` /
`hw_camera_device_callback` files are deleted from the tree.
Camera HAP → CameraService → camera_host → VDI → droidmedia all the
way through `Capture(start_preview)` returns success.  Halium-side
`dumpsys media.camera` confirms the MTK HAL connects (`CONNECT
device 0 client for package droidmedia`).  D.6 frame delivery
remains BLOCKED — not on the droidmedia layer but on a kernel-side
limitation: MTK HAL fails capture-request processing with
`RscDrv ENQUE_REQ fail`, `no rsc device` because the ISP sub-block
drivers (RSC, DIP, DPE, MFB, WPE, PDA) are deferred from pre-init
load (their probe causes an SoC watchdog reboot — captured in
[phase_n12_camera_modulemap.md](phase_n12_camera_modulemap.md)
"Decision (Milestone 1)").  We now know these aren't just optional
optimisations; **they're required for the HAL to actually deliver
frames**.  Tracked as a separate kernel/PM investigation, outside
the droidmedia pivot scope.  D.9 (torch) deferred: the on-device
libdroidmedia.so doesn't export `droid_media_camera_set_torch_mode`
(see § Open risks #7).

Supersedes the configureStreams_3_4 line of work in
[phase_n12_camera.md](phase_n12_camera.md) § N12.6/N12.7.  Pure-musl
HIDL transport (N12.5.0–N12.6.1b) stays in tree — it's the right
architecture for *small* HIDL surfaces (the existing ICameraProvider
enumeration + open works fine) but the wrong tool for camera streaming.

## Why we're pivoting

§ N12.6 made it to "OHOS Camera HAP launches with full UI" by
hand-rolling a HIDL transport (`hw_binder_client/server/parcel` +
`hw_camera_provider` + `hw_camera_device` + `hw_camera_device_session`).
Pushing through to actual frames-on-screen would still need:

1. `configureStreams_3_4` nested-struct serialiser (~500 lines, V3_4
   StreamConfiguration → V3_4 Stream → hidl_string physicalCameraId
   → bufferSize, each with per-element binder_buffer_object fixups).
2. FMQ (Fast Message Queue) shared-memory ring for per-frame metadata.
3. `processCaptureRequest_3_4` (~600 lines, vec<CaptureRequest> with
   vec<StreamBuffer> with native_handle_t* gralloc handles).
4. Server-side `BnHwCameraDeviceCallback::processCaptureResult` parser
   (~400 lines).
5. Android `native_handle_t` ↔ OHOS `BufferHandle` round-trip with
   gralloc-private layout matching.

Each is mechanical but unforgiving — wire-format errors surface as
`BR_DEAD_REPLY` (Halium HIDL stub crashes silently) and cost a
build+flash+repro cycle to localize.  The 2026-05-21 session burned
the entire afternoon on `configureStreams` alone (V3.2 → DeadReply,
V3.4 with 48-byte struct → status_t=-22, V3.4 with 40-byte struct +
out-of-order buffer fixups → DeadReply again).  Estimated cost to
finish on the hand-rolled path: 4–6 sessions.

**Meanwhile, the same wheel has already been invented and is sitting
on the Halium rootfs.**

## What's already on the X23

```
/android/system/lib64/libdroidmedia.so          (119 KB)
/android/system/lib64/libcamera_client.so       (Camera1 client lib)
/android/system/lib64/libcameraservice.so       (cameraserver framework)
/android/system/bin/camera_service              (AOSP cameraserver, running)
```

`libdroidmedia.so` exports a pure-C ABI of ~45 functions wrapping
AOSP's Camera1 (`<camera/Camera.h>`) over libbinder + libgui.  It
hides every piece of HIDL/FMQ/native_handle_t marshalling listed
above behind opaque pointers.

The C symbols (excerpt — full list in
[droidmedia upstream](https://github.com/sailfishos/droidmedia/blob/master/droidmediacamera.h)):

```c
int  droid_media_camera_get_number_of_cameras(void);
bool droid_media_camera_get_info(DroidMediaCameraInfo *info, int n);
DroidMediaCamera *droid_media_camera_connect(int camera_number);
void droid_media_camera_disconnect(DroidMediaCamera *cam);

bool droid_media_camera_start_preview(DroidMediaCamera *cam);
void droid_media_camera_stop_preview(DroidMediaCamera *cam);
bool droid_media_camera_take_picture(DroidMediaCamera *cam);
bool droid_media_camera_set_torch_mode(bool enabled);

DroidMediaBufferQueue *droid_media_camera_get_buffer_queue(DroidMediaCamera *cam);

void droid_media_camera_set_callbacks(DroidMediaCamera *cam,
                                      DroidMediaCameraCallbacks *cb, void *data);
void droid_media_buffer_queue_set_callbacks(DroidMediaBufferQueue *bq,
                                            DroidMediaBufferQueueCallbacks *cb, void *data);

bool droid_media_buffer_lock_ycbcr(DroidMediaBuffer *buf, uint32_t flags,
                                    DroidMediaBufferYCbCr *out);
void droid_media_buffer_unlock(DroidMediaBuffer *buf);
void droid_media_buffer_release(DroidMediaBuffer *buf, void *display, void *fence);

void droid_media_buffer_get_info(DroidMediaBuffer *buf, DroidMediaBufferInfo *info);
const void *droid_media_buffer_get_handle(DroidMediaBuffer *buf);   /* native_handle_t* */
```

The `DroidMediaBufferYCbCr` struct gives raw pointers to the Y, Cb,
Cr planes of a locked preview frame — exactly the shape we need to
memcpy into an OHOS `BufferHandle`.

## Who else does it this way

`libdroidmedia` was originally written by Jolla / SailfishOS in 2014.
Every modern Halium-based Linux distro uses it for camera:

| Distro              | Camera path                                          |
|---------------------|------------------------------------------------------|
| **SailfishOS**      | `gstdroidcamsrc` (GStreamer) → libdroidmedia        |
| **Droidian**        | `gst-droid` → libdroidmedia                          |
| **postmarketOS**    | libdroidmedia (Phosh, Plasma Mobile)                |
| **Manjaro Phosh**   | libdroidmedia                                        |
| **Ubuntu Touch**    | `libhybris/compat/camera/camera_compatibility_layer.cpp` (older C++ wrapper around the same Camera1 client; predates droidmedia) |

None of them hand-roll HIDL parcels.  All ship working camera apps.
Their adaptation effort per device is "make sure libcamera_client +
cameraserver come up cleanly under Halium" — which we already have on
the X23 (`/android/system/bin/camera_service` is running as PID
`cameraserver`).

## Architecture

Same pattern as `composer_host` / `audio_host` / `allocator_host`
already use for display, audio, gralloc — except this time the
bionic-loaded library is `libdroidmedia.so` instead of the HIDL
provider directly.

```
┌─────────────────────────── OHOS musl process ──────────────────────────┐
│  camera_host (HDF host service)                                        │
│    ↓ HdfLoadVdi                                                        │
│  libcamera_host_vdi_impl_1.0.z.so   ← our VDI, musl-linked             │
│    ├ HybrisCameraHostVdiImpl (enumerate, open, GetCameraAbility)       │
│    ├ HybrisCameraDeviceVdiImpl  (per-camera session)                   │
│    └ HybrisStreamOperatorVdiImpl (streams, capture, frame delivery)    │
│         ↓ hybris_dlopen + hybris_dlsym                                 │
└─────────┼────────────────────────────────────────────────────────────────┘
          │ libhybris bionic-namespace linker
          ↓
┌────────────────────────── bionic namespace ───────────────────────────┐
│  /android/system/lib64/libdroidmedia.so                               │
│    NEEDED libcamera_client.so libgui.so libbinder.so libui.so ...     │
│         ↓ AOSP binder transactions to                                 │
│  /android/system/bin/camera_service  (Halium cameraserver, PID running)│
│    ↓ Camera2 HIDL over /dev/hwbinder to                               │
│  /android/system/bin/camerahalserver  (Halium HIDL camera HAL)        │
│    ↓ /dev/video* / V4L2 / MTK ISP6s kernel drivers                    │
└────────────────────────────────────────────────────────────────────────┘
```

The N12.5.0 smoke already proved libhybris can `dlopen` Halium
libraries from an OHOS-side musl process; this is the same load path
exercised at scale.

## What stays from the pure-musl HIDL work

| Module                            | Kept? | Why                                                   |
|-----------------------------------|-------|-------------------------------------------------------|
| `src/hidl/hw_binder_client.cpp`   | ✅    | Reusable for *any* HIDL service we need to talk to that isn't camera (sensors, BT, etc.); the BR_TRANSACTION nested-call dispatch is hard-won. |
| `src/hidl/hw_binder_server.cpp`   | ✅    | Same — server-side binder dispatch is generic. |
| `src/hidl/hw_parcel.cpp`          | ✅    | Generic HIDL Parcel encoder/decoder. |
| `src/hidl/hw_service_manager.cpp` | ✅    | Resolving `android.hidl.manager::IServiceManager` is universal. |
| `src/hidl/hw_camera_provider.cpp` | ⚠️    | `GetCameraIdList` and `GetCameraDeviceInterface` worked end-to-end and could in theory still be used to enumerate IDs.  But droidmedia's `droid_media_camera_get_number_of_cameras` / `_get_info` does the same in 2 function calls.  **Delete.** |
| `src/hidl/hw_camera_device.cpp`   | ❌    | Superseded by `droid_media_camera_connect`.  **Delete.** |
| `src/hidl/hw_camera_device_callback.cpp` | ❌ | Superseded by droidmedia's callback registration.  **Delete.** |
| `src/hidl/hw_camera_device_session.cpp`  | ❌ | Superseded by droidmedia.  **Delete.** |
| `tools/hidl_*.cpp` smoke tests    | ✅    | Keep — they prove the transport works for non-camera HALs. |
| `tools/camera_hidl_smoke.cpp`     | ✅    | Keep — proves we can dlopen Halium libs (preamble for the droidmedia pivot). |
| `src/camera_host/hybris_camera_ability.cpp` | ✅ | The per-camera hand-rolled `CameraMetadata` blobs (S5KGM1ST / OV16A1Q / GC08A3WIDE) still serve while we don't translate Halium characteristics — droidmedia's `DroidMediaCameraInfo` is minimal (`facing` + `orientation`) so we'd hand-roll the rest anyway. |

The `src/camera_device/*` files (device VDI, stream operator VDI) get
re-implemented against droidmedia.  The OHOS-facing API surface
doesn't change.

## Substep plan

### N12.D.1 — Vendor droidmedia headers

Pull from upstream
[sailfishos/droidmedia](https://github.com/sailfishos/droidmedia):

  `droidmedia.h`         core types + init/deinit + `DroidMediaBuffer` API
  `droidmediabuffer.h`   internal `DroidMediaBuffer` definition (we only need the opaque `*DroidMediaBuffer` typedef from droidmedia.h, but vendoring the full set keeps the headers self-consistent)
  `droidmediacamera.h`   camera API (~45 functions)
  `droidmediacodec.h`    not needed for stills/preview MVP but small; vendor for future video record

Drop into `device/soc/oniro/hybris_generic/hardware/camera/include/droidmedia/`.
~15 KB total.  Apache-2.0 licensed (compatible with our tree).

### N12.D.2 — Smoke test: dlopen libdroidmedia + enumerate cameras

New tool `tools/droidmedia_smoke.cpp`.  Same shape as
`tools/camera_hidl_smoke.cpp`:

```cpp
void *lib = hybris_dlopen("/android/system/lib64/libdroidmedia.so", RTLD_NOW);
auto init = (bool(*)())hybris_dlsym(lib, "droid_media_init");
auto get_n = (int(*)())hybris_dlsym(lib, "droid_media_camera_get_number_of_cameras");
auto get_info = (bool(*)(DroidMediaCameraInfo*, int))hybris_dlsym(lib,
    "droid_media_camera_get_info");

init();
int n = get_n();
for (int i = 0; i < n; ++i) {
    DroidMediaCameraInfo info;
    get_info(&info, i);
    printf("camera %d: facing=%d orientation=%d\n", i, info.facing, info.orientation);
}
```

Expected output: `3 cameras` (S5KGM1ST + OV16A1Q + GC08A3WIDE), each
with `facing` ∈ {0=FRONT, 1=BACK} and an `orientation` in degrees.

Acceptance: prints exactly 3 cameras with reasonable values.  No
further work proceeds until this prints.

### N12.D.3 — Refactor VDI to load droidmedia once

Add `src/droidmedia/droidmedia_loader.{h,cpp}` — a thin process-local
struct that:

  - On first use: `hybris_dlopen` libdroidmedia, `hybris_dlsym` every
    function we need, store function pointers in a vtable struct.
  - Provides those pointers as inline accessors.
  - Calls `droid_media_init()` exactly once per process.
  - Refuses to retry on failure (camera_host will respawn).

Replace the existing `HwBinderClient`/`HwCameraProvider`/etc. usage in
`HybrisCameraHostVdiImpl::Init` + `OpenCamera` with calls through this
loader.

### N12.D.4 — Connect + GetCameraAbility from real device info

`OpenCamera(ohosId)` looks up the OHOS-→droidmedia camera index, then:

```cpp
DroidMediaCamera *cam = dm_->camera_connect(droid_idx);
```

Wraps the `cam` handle in `HybrisCameraDeviceVdiImpl`.  Constructor
also pulls `DroidMediaCameraInfo` for `facing`/`orientation` —
populates them into our `hybris_camera_ability.cpp` profile so
`GetCameraAbility` no longer hard-codes orientation.

`hybris_camera_ability.cpp` still hand-rolls stream config /
JPEG sizes etc. — those need the `getParameters` string which is a
Camera1 concept; droidmedia exposes it as
`droid_media_camera_get_parameters` returning a `;`-separated KV
string we'd parse.  Not MVP; we keep the hand-rolled config and
log-validate the HAL accepts whatever we ask for.

### N12.D.5 — Bridge `GetStreamOperator` + `CreateStreams` to droidmedia preview

`HybrisStreamOperatorVdiImpl::CommitStreams` becomes:

  1. `dm_->camera_get_buffer_queue(cam)` → `DroidMediaBufferQueue *bq`
  2. `dm_->buffer_queue_set_callbacks(bq, &cbs, this)` where
     `cbs.frame_available = OnFrameAvailable` and
     `cbs.buffer_created` / `cbs.buffers_released` are no-ops for MVP.
  3. `dm_->camera_set_parameters(cam, "preview-size=1280x720;...")`
     to request the resolution the OHOS framework asked for.
  4. `dm_->camera_start_preview(cam)`.

`CommitStreams` returns NO_ERROR.  HalStreamConfiguration translation
is not needed — droidmedia handles the negotiation internally and
emits whatever resolution it landed on via frame metadata.

### N12.D.6 — Per-frame: lock → memcpy → push (MVP zero-knowledge buffer copy)

The `OnFrameAvailable(DroidMediaBuffer *buf)` callback runs on a
droidmedia worker thread:

```cpp
DroidMediaBufferInfo info;
dm_->buffer_get_info(buf, &info);

DroidMediaBufferYCbCr yuv;
if (!dm_->buffer_lock_ycbcr(buf, DROID_MEDIA_BUFFER_LOCK_READ, &yuv)) { /* drop */ }

// Request an OHOS BufferHandle from the producer
SurfaceBuffer *out;
bufferProducer_->RequestBuffer({.width=info.width, .height=info.height,
                                .format=PIXEL_FMT_YCRCB_420_SP, .usage=...}, ...);

// memcpy Y + interleaved CrCb (NV21).  Slow but correct.
const uint8_t *srcY = (const uint8_t *)yuv.y;
uint8_t *dstY = (uint8_t *)out->GetVirAddr();
for (int row = 0; row < info.height; ++row) {
    memcpy(dstY + row * out->GetStride(), srcY + row * yuv.ystride, info.width);
}
// Then UV plane.  NV12/NV21 layout depends on yuv.cb vs yuv.cr ordering.

dm_->buffer_unlock(buf);
dm_->buffer_release(buf, NULL, NULL);

bufferProducer_->FlushBuffer(out, ...);   // pushes to OHOS surface
streamCallback_->OnFrameShutter(captureId, {streamId}, info.timestamp);
```

This is the "guaranteed to work, possibly slow" path.  Zero-copy via
`droid_media_buffer_get_handle()` + OHOS gralloc import is N12.D.7
optimisation.

### N12.D.7 — Optional: zero-copy via gralloc handle import

`droid_media_buffer_get_handle()` returns the underlying
`native_handle_t *` (Android gralloc).  `allocator_host` already
loads Halium's gralloc HAL — we can ask it to import this handle and
hand back an OHOS `BufferHandle` with the same DMA-BUF fd, zero-copy.

Same trick `composer_host` already uses for the framebuffer.  Saves
~1280×720×1.5 = 1.4 MB memcpy per frame at 30 fps = 42 MB/s.  Not
critical for MVP but the right end-state.

### N12.D.8 — `take_picture` + JPEG delivery

`HybrisStreamOperatorVdiImpl::Capture(streaming=false)` on a JPEG
stream:

```cpp
DroidMediaCameraCallbacks cbs = {};
cbs.compressed_image_cb = OnCompressedImage;  // gets DroidMediaData with JPEG bytes
dm_->camera_set_callbacks(cam, &cbs, this);
dm_->camera_take_picture(cam);
```

`OnCompressedImage` gets the JPEG bytes, allocates an OHOS BufferHandle
of `PIXEL_FMT_BLOB`, memcpy's the JPEG in, FlushBuffer's, fires
`OnCaptureEnded`.

### N12.D.9 — SetTorchMode

```cpp
int32_t SetFlashlight(ohosCameraId, bool enable) {
    return dm_->camera_set_torch_mode(enable) ? NO_ERROR : DEVICE_ERROR;
}
```

One-liner.

## What we learned bringing this up

Captured here so the production wiring task and any future port to the
mimir tablet doesn't repeat the same diagnostic walk.

1. **libdroidmedia.so on the X23 Halium rootfs is older than the
   sailfishos/droidmedia master headers.**  The shipped .so exports
   `_droid_media_init` (`void`-return, leading underscore — the older
   internal name).  Modern upstream renames it to `droid_media_init`
   (returns `bool`).  `Droid::Loader` resolves both and uses whichever
   is present.  The on-device build also lacks
   `droid_media_camera_set_torch_mode`, which is why D.9 (torch) is
   parked.  Decision: ship the loader's dual-name handling; don't try
   to upgrade libdroidmedia.so yet — a fresh build would also pull in
   newer dependencies that may not match the rest of the Halium rootfs.

2. **libhybris's compiled-in `DEFAULT_HYBRIS_LD_LIBRARY_PATH`
   (`/android/{vendor,system,odm}/lib64`) is *not* enough for the
   libdroidmedia dep chain.**  `libdroidmedia.so → libmedia.so →
   libandroidicu.so` and `libandroidicu.so` lives in the Halium i18n
   APEX at `/android/system/apex/com.android.i18n/lib64`.  Without
   that path on `HYBRIS_LD_LIBRARY_PATH`, hybris_dlopen dies with
   `"library "libandroidicu.so" not found"`.  The loader now
   `setenv()`s the apex path before `hybris_dlopen` (init service
   .cfg `env` overrides via `z_hybris_hdf_env.cfg` do NOT take effect
   — see #6).

3. **`/dev/binder` inside an OHOS-side process points at OHOS's binder
   driver (binderfs 510:1), not Halium's (`/dev/binderfs/android-
   binder`, 510:7).**  AOSP libbinder's `ProcessState::self()`
   hard-codes `open("/dev/binder")` and is called from libdroidmedia's
   bionic static constructor — so the dlopen itself hangs (binder is
   the wrong driver, lookups loop forever).  Fix: `Droid::Loader::Init`
   does `unshare(CLONE_NEWNS)` + `mount(MS_REC|MS_SLAVE, /)` +
   `mount(MS_BIND, /dev/binderfs/android-binder, /dev/binder)` before
   the dlopen.  Same trick `androidd.c` already uses to set up
   Halium-side init's NS.  Requires CAP_SYS_ADMIN — see #5.

4. **Strace was the diagnostic tool that pinned this down.**  The hang
   was silent (no hilog, no printf past the banner), but `strace -e
   openat,open -f -o /tmp/strace.log ./smoke` showed the dlopen
   walking the full transitive dep list cleanly, then `openat
   /dev/binder` and stopping — that's where libbinder is blocking.
   Worth remembering for any future libhybris bring-up that loads
   stateful AOSP services.

5. **camera_host runs as uid 3028 / no caps by default.**  Granting
   CAP_SYS_ADMIN via the `caps` field on the host block in
   `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs`
   propagates through hc-gen → `hdf_devhost.cfg` → init.  HCS does
   NOT accept C-style `/* */` comments (parse error
   `"invalid character"`); use `//` line comments instead.

6. **OHOS init does NOT merge `env` or `caps` from
   `z_hybris_hdf_env.cfg`-style override files** into services
   already defined elsewhere — only the primary cfg's fields take
   effect.  composer_host's caps come from `hdf_devhost.cfg`, not
   `z_hybris_hdf_env.cfg`.  This explains why the same override
   pattern that "works" for composer_host turned out to be a no-op:
   composer_host doesn't actually *need* HYBRIS_LD_LIBRARY_PATH (the
   compiled-in default suffices for its dep chain).  For camera_host
   we set the env in code via setenv() inside `Droid::Loader::Init`.

7. **`innerapi_tags = [ "passthrough" ]` on the camera VDI breaks
   HdfLoadVdi.**  HdfLoadVdi hard-codes the search path as
   `/vendor/lib64/<libname>` (see
   `drivers/hdf_core/framework/core/host/src/hdf_load_vdi.c:34`).
   Adding the passthrough tag moves the install path to
   `/vendor/lib64/passthrough/<libname>` instead — fine for the
   display VDIs (they're dlopen'd by name + namespace search, which
   does cover passthrough), but the camera HDF host can't find them.
   Symptom: camera_host falls back to the HCS "lcam001" placeholder
   even though the .so is on disk.  Fix: drop the tag from the
   camera VDI's BUILD.gn target.  Documented inline in
   `device/soc/oniro/hybris_generic/hardware/camera/BUILD.gn`.

8. **Upstream droidmedia `set_callbacks` / `buffer_queue_set_callbacks`
   dereference `*cb` unconditionally.**  Clearing droidmedia
   callbacks by passing `nullptr` SIGSEGVs inside libdroidmedia.  The
   only safe way to detach is to pass a zero-initialised
   DroidMediaCameraCallbacks / DroidMediaBufferQueueCallbacks struct
   (all function pointers NULL).  Observed 2026-05-21 in the
   HybrisStreamOperatorVdiImpl dtor crash trace — fix landed in the
   same session.

## Open risks

1. **libdroidmedia might not initialise under the chainload root.**
   ✅ Verified — D.2 smoke prints 3 cameras after the binder-NS
   bind, including `_droid_media_init` legacy entry point.  AOSP
   `cameraserver` (`/android/system/bin/camera_service`) runs as a
   child of androidd, so `/dev/binderfs/android-binder` registrations
   are live by the time the smoke runs.

2. **Camera1 API limits.**  Camera1 advertises a limited subset of
   what Camera2 HAL supports — no multi-camera composition, no RAW,
   no custom shutter speed beyond `auto/night/sports` presets.  All
   acceptable for MVP (preview + JPEG capture + flashlight).  For
   future RAW / pro mode work we'd revisit.

3. **Buffer format negotiation.**  Camera1's
   `setParameters("preview-format=yuv420sp")` returns NV21 by default
   on most MTK HALs.  Our `hybris_camera_ability.cpp` already
   advertises `OHOS_CAMERA_FORMAT_YCRCB_420_SP` (NV21) for preview —
   match.  If the HAL gives us NV12, we'd swap U/V during the memcpy.

4. **Threading.**  droidmedia callbacks land on a bionic-side worker
   thread (bionic TLS, not musl TLS).  Calling OHOS APIs (hilog,
   BufferProducer flushes) from those threads requires the marshal
   pattern composer_host already uses: SPSC queue into an OHOS-side
   worker that runs the BufferProducer flush.  ~50 lines.

5. **libdroidmedia path on the chainload.**  Currently
   `/android/system/lib64/libdroidmedia.so`.  Loader uses the absolute
   path directly — no redirect logic needed.  ✅ Verified.

6. **Permissions.**  camera_host runs as OHOS-side `camera_host` uid
   (3028).  Cross-namespace binder calls into Halium's `cameraserver`
   work because we bind `/dev/binderfs/android-binder` over
   `/dev/binder` inside the loader's private NS — the OHOS-side
   process's libbinder then opens Halium's binder driver instead of
   OHOS's.  Requires CAP_SYS_ADMIN (granted via HCS — see § What we
   learned #5).

7. **Torch / flashlight (N12.D.9) — symbol absent on this droidmedia
   build.**  `droid_media_camera_set_torch_mode` was added to upstream
   droidmedia after the X23's Halium rootfs was cut.  Future paths:
   (a) build a fresh droidmedia from source against the Halium AOSP
   tree; (b) drive `/sys/class/leds/flashlight/brightness` directly
   from a separate flashlight VDI.  (b) is simpler and avoids
   re-cutting the Halium rootfs; logged as a follow-up task.

## Verification plan

| Step | Acceptance | Status |
|---|---|---|
| N12.D.2 | `droidmedia_smoke` prints 3 cameras with sensible facing/orientation | ✅ 2026-05-21 |
| N12.D.3 | `droidmedia_loader_smoke` exercises `Droid::Loader` end-to-end (Init → enumerate → connect/disconnect each) | ✅ 2026-05-21 — params strings come back per-camera, e.g. `preview-size=1920x1080;…;preview-size-values=2560x1440,…` |
| N12.D.4 | `HybrisCameraHostVdiImpl::Init` populates camera IDs `0/1/2` from `get_info`; matches the hand-rolled X23 ability table | ✅ 2026-05-21 |
| N12.D.5 part 1 | camera_host loads our VDI cleanly under HdfLoadVdi after a fresh super.img reflash (CAP_SYS_ADMIN granted via HCS, no symlinks required) | ✅ 2026-05-21 |
| N12.D.5 part 2 | OHOS Camera HAP → HCameraService → camera_host → VDI → droidmedia: OpenCamera + CreateStreams + CommitStreams + Capture(start_preview) all return success | ✅ 2026-05-21 — `set_parameters` accepts `preview-size=1280x720` after the round-trip fix (mutate full params string instead of replacing it), `start_preview OK` |
| N12.D.6 | hilog `OnFrameAvailable cb` fires repeatedly at ~30 Hz after HAP opens preview, Camera HAP shows live frames | ❌ **OPEN — blocker is kernel-side, not droidmedia.**  Halium-side `dumpsys media.camera` + Halium `logcat` traced end-to-end (via a custom `halium_exec` helper that does setns(pid)+setns(mnt)+chroot to reach Halium's NS): the MTK Camera3 HAL accepts our capture requests, starts P1Node, configures the sensor (S5KGM1ST 2000x1500 BAYER10) — but then capture-request processing fails inside the kernel with `RscDrv ENQUE_REQ fail (-1), no rsc device`.  9 inflight requests time out (`metadata arrived: false`), cameraserver reports `Camera 0: Timed out waiting for current request id`, broken pipe, HAL respawn.  Root cause: `camera_rsc_isp60.ko` (Resampler ISP driver, plus DIP / DPE / MFB / WPE / PDA) is intentionally **deferred** from the pre-init load (see [phase_n12_camera_modulemap.md](phase_n12_camera_modulemap.md) § "Deferred (Milestone 2+)") because their probe touches camera-power-domain registers in a way that reliably triggers a watchdog reboot.  Even forcing the power-domain ON via `/sys/devices/platform/1a000000.camisp_legacy/power/control=on` (genpd reports `cam on`) does NOT prevent the watchdog — insmod returns rc=0 silently, then the SoC dies ~5-10 s later.  Tracked as a separate kernel/PM investigation in phase_n12_camera_modulemap.md follow-up — outside the droidmedia pivot scope. |
| N12.D.7 | zero-copy via gralloc handle import | ❌ not attempted — D.6 must work first |
| N12.D.8 | Camera HAP shutter → JPEG file saved under `/storage/Users/.../DCIM/` | ❌ blocked on D.6 — implementation complete, awaits frames |
| N12.D.9 | Camera HAP flash toggle | ❌ deferred — `droid_media_camera_set_torch_mode` not exported by this libdroidmedia build (see § Open risks #7) |

## Estimated effort

| Substep | Lines added | Sessions |
|---|---|---|
| N12.D.1 vendor headers | ~600 (4 .h files) | trivial |
| N12.D.2 smoke test | ~150 | 0.5 |
| N12.D.3 loader refactor | ~250 | 0.5 |
| N12.D.4 connect + ability | ~150 | 0.5 |
| N12.D.5 stream operator preview | ~400 | 1 |
| N12.D.6 frame copy | ~300 | 1 |
| N12.D.7 zero-copy (optional) | ~250 | 1 |
| N12.D.8 take_picture | ~200 | 0.5 |
| N12.D.9 torch | ~30 | trivial |
| **Total to frame-on-screen (N12.D.1–6)** | **~1850** | **3–4 sessions** |

vs. the hand-rolled HIDL completion estimate (~3500 lines + multiple
wire-format debug cycles, 5+ sessions, with non-trivial risk of more
wire-format gotchas surfacing in FMQ + buffer interop).

## Decision

**Pivot.**  N12.D supersedes N12.6.1c / N12.7 in
[phase_n12_camera.md](phase_n12_camera.md).  The hand-rolled HIDL
work doesn't go in the bin — `hw_binder_client/server/parcel` /
`hw_service_manager` are useful infrastructure for *other* HIDL HALs
(sensors, bluetooth) where we don't have a droidmedia equivalent.
Only the camera-specific HIDL files get deleted.

Next session: N12.D.1 + N12.D.2 (vendor headers + smoke).
