# Waydroid container data (tracked)

The small, hand-made inputs the Waydroid container needs. Everything else
under `/data/waydroid` is downloaded (`waydroid-images.sh`), regenerated
(`waydroid-patches.sh`) or built (`waydroid-deploy.sh`), so these three files
plus the scripts are the whole non-image source of truth. They were lost once
already by living only in an untracked cache — keep them here.

| File | What it is |
|---|---|
| `images.manifest` | sha256 pins for the two Waydroid OTA zips (`<zip> <sha256>`); `waydroid-images.sh` fetches and verifies against it. system = lineage-20.0 VANILLA arm64, vendor = HALIUM_13 arm64. No HALIUM_14 vendor image is published — the A13-image-on-A14-blobs skew is why the gralloc bridge and the two binary patches exist. |
| `waydroid.prop` | the container's property file, bind-mounted over the shim vendor's placeholder. Mirrors upstream `make_base_props()` for a Halium-14 MTK host, plus the ansuz identity, `ro.sf.lcd_density=480` and the ARM EGL block. |
| `hosthals.xml` | trimmed host-HAL passthrough list (plan D11): only allocator, mapper, memtrack, media.c2 and MediaTek mms. Every HAL that OHOS already owns a client of (camera, drm, power, thermal, vibrator, gnss, nfc…) is dropped to avoid double-client contention. |

Two `waydroid.prop` settings are load-bearing and were each paid for with a
boot failure:

* `ro.hardware.egl=mali` — with upstream's `meow` wrapper the container's
  zygote SEGVs in `eglGetDisplay` (null hooks).
* `waydroid.background_start=true`, and **no** `waydroid.active_apps` here —
  presetting `active_apps=Waydroid` makes the hwcomposer crash on
  SurfaceFlinger's first empty present (full-UI mode forwards a
  framebuffer-target layer whose handle is NULL until something
  client-composites). The switch to the full UI is a runtime prop flip after
  `sys.boot_completed`, which `waydroid-gralloc.rc` does.

To re-derive the prop file for a new device, follow upstream Waydroid's
`make_base_props()` and change the `ro.product.waydroid.*` identity block; the
EGL/gralloc lines above must stay as they are on a Halium MTK host.
