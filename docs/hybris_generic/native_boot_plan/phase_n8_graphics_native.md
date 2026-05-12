# Phase N8 — Graphics & Display (Native)

**Status:** 🔄 Open — rewritten 2026-05-12. Previous draft was marked "✅ Source-side complete" but its prerequisites (`/android/{system,vendor}` populated and a running Halium HAL stack) were never delivered. The earlier source-side claims were source-side-only; nothing reached a working frame on the panel under native boot.

> **Goal.** `render_service` lights pixels on the panel under native OHOS, inheriting Phases 5–8 (libhybris, display VDIs, stability fixes) without modification beyond the gating cfg added below.

## Dependencies

This phase is **wiring**, not new source. The hard work happens in:

- **N5** — Halium `system_a` + `vendor_a` mounted at `/android/system` + `/android/vendor` by the chainload.
- **N4** — `androidd` brings up hwservicemanager + servicemanager + vndservicemanager + composer@2.x + gralloc@4.0 in a child namespace, sharing OHOS's `/dev/hwbinder`. Parent sets `param android.composer.ready=1` when composer registers.
- **N6** — OHOS owns default `/dev/binder`; Android binds `android-binder` as its `/dev/binder` in its namespace; `hwbinder` + `vndbinder` shared.
- **Phases 5, 6, 7, 8, 11** of the LXC plan — all carry over unchanged.

If N4/N5 aren't done, **everything in N8 is a no-op**.

---

## N8.1 — Library path resolution: nothing to do

libhybris's path-redirect map already remaps `/vendor/lib64 → /android/vendor/lib64` for Android-vendor library loads (`third_party/libhybris/hybris/common/hooks.c:2368`, `hybris/common/q/linker.cpp:119`). OHOS-side processes (composer_host, render_service) using libhybris call into Android HALs via `/android/vendor/lib64/{egl,hw}/...` automatically — no source edits, no special bind mounts.

The 2026-03-20 SPHAL revert documented in `device/board/oniro/hybris_generic/utils/device/lxc/config:71-79` was caused by Android libs leaking into OHOS's *own* `/vendor/lib64/`. Native boot's `/vendor` is OHOS vendor only; Halium content lives at `/android/vendor`. **No collision possible.** The LXC bind-mounts that wired Android libs into OHOS-vendor in the old build are simply not present.

---

## N8.2 — EGL/GLES symlinks: inherit from Phase 6

The OHOS-side EGL impl symlinks (`/system/lib64/libEGL_impl.so → libhybris EGL.so`, etc.) are shipped by `device/soc/oniro/hybris_generic/hardware/display/` (Phase 6). These live in OHOS's `system` partition and ride along into native boot without modification.

The Android-side EGL impl (`/vendor/lib64/egl/libGLES_mali.so`) ships in Halium's `vendor_a` and is reachable at `/android/vendor/lib64/egl/libGLES_mali.so` after N5's chainload mount. Libhybris's linker remap finds it.

---

## N8.3 — Environment variables: already in tree

`device/board/oniro/hybris_generic/cfg/hybris_graphic_env.cfg` and `cfg/z_hybris_hdf_env.cfg` set:

```
HYBRIS_LD_LIBRARY_PATH = /android/vendor/lib64:/android/system/lib64
LD_LIBRARY_PATH        = /system/lib64/libhybris:/system/lib64
HYBRIS_EGLPLATFORM     = ohos
LIBEGL                 = /android/vendor/lib64/egl/libGLES_mali.so
LIBGLESV2              = /android/vendor/lib64/egl/libGLES_mali.so
```

on `composer_host`, `allocator_host`, `render_service`, `bootanimation`. Native boot inherits these unchanged.

> Important: these env vars *only* take effect when `/android/{system,vendor}` actually exist (N5). Without that, libhybris's `dlopen` lookups will fail with ENOENT and composer_host will SIGSEGV (or, with the gate cfg below, never start at all).

---

## N8.4 — Composer-readiness gate (the only new artifact in N8)

This is the one cfg N8 contributes. Without it, `composer_host` and `allocator_host` start at OHOS init's normal boot trigger and crash because hwservicemanager isn't up yet (the launcher is still running Halium init).

### `device/board/oniro/hybris_generic/cfg/z_composer_host_gate.cfg`

```json
{
    "services" : [{
            "name" : "composer_host",
            "start-mode" : "condition",
            "condition"  : "param:android.composer.ready=1"
        },
        {
            "name" : "allocator_host",
            "start-mode" : "condition",
            "condition"  : "param:android.composer.ready=1"
        }
    ]
}
```

The `z_` prefix sorts after the upstream `composer_host.cfg` in `/system/etc/init/`, so OHOS init's cfg-merge applies our `start-mode`/`condition` overrides last.

Wire into `device/board/oniro/hybris_generic/cfg/BUILD.gn` (sibling to `z_hybris_hdf_env.cfg` etc.). Add to `hybris_generic_cfg_group`.

Parameter source: the parent of `androidd` (Phase N4.4) polls hwservicemanager for `android.hardware.graphics.composer@2.1::IComposer/default` and calls `SystemSetParameter("android.composer.ready", "1")` on success.

### Why not just `wait_other` or `bootevent`?

- `bootevent` watchers are for OHOS-side init events; we need a Halium-side signal crossing the namespace boundary.
- `wait_other` (waiting on another OHOS service) doesn't apply — `androidd` *is* an OHOS service, but its readiness signal is about a child in another namespace.
- `param:` condition is the natural fit and already supported by OHOS init's condition parser (used by many existing cfgs).

---

## N8.5 — Device-node access: already covered

ueventd handles `/dev/mali0`, `/dev/dri/*`, `/dev/dma_heap/*` perms via:

- `vendor/oniro/hybris_generic/etc/ueventd/ueventd.config` (existing)
- `vendor/oniro/hybris_generic/etc/init/init.x23.cfg` (creates the `/android` mount points)

The chainload also pre-creates these on the host `/dev` and the binds inherit. `composer_host` runs uid 3036 (composer_host) + `gid graphics` + `caps SYS_NICE DAC_OVERRIDE` (Phase 11 — for the backlight sysfs writer) — these are unchanged in native.

---

## N8.6 — Expect Phase 8 stability bugs to reproduce

Native boot doesn't change the EGL teardown sequence, the HWC2 spec violations, or the Mali driver's NULL+0x1d8 crash on dropdown close. Specifically:

- **Bug 8.11** — `composer_host` SIGSEGV in `SetLayerAlpha` after ~46 minutes. Reproduces.
- **Bug 8.17** — Mali NULL+0x1d8 on dropdown close (RSRenderThread). Reproduces. Mitigations from Phase 8.17 (rwlock in libhybris EGL) carry over.
- **Bug 8.18** — Webview / nweb sandbox fix (`chmod 0644` on appdata-sandbox.json). Need to verify the native rootfs has this fix baked in (it's currently in `deploy-lxc-container.sh` for the LXC build, not in the build pipeline).

Action: port the appdata-sandbox.json chmod into the OHOS image build (or into `init.x23.cfg` as a `chmod` cmd) so it survives the native flash. Track in Phase 8 doc updates.

---

## N8 deliverables

| Item | Path | Status |
|---|---|---|
| Composer-ready gate cfg | `device/board/oniro/hybris_generic/cfg/z_composer_host_gate.cfg` | TODO |
| BUILD.gn entry | `device/board/oniro/hybris_generic/cfg/BUILD.gn` | TODO (extend `hybris_generic_cfg_group`) |
| Port Bug 8.18 fix to native image | `vendor/oniro/hybris_generic/etc/init/init.x23.cfg` (chmod cmd) or system_patch | TODO |

## Bring-up checklist

After N4 + N5 + N8 deploy, from `hdc shell`:

```sh
# Halium content present
ls /android/system/bin/hwservicemanager       # exists
ls /android/vendor/lib64/hw/                  # populated
ls /android/vendor/lib64/egl/libGLES_mali.so  # exists

# Halium HAL stack alive
pidof androidd                                # one PID
nsenter -t $(pidof androidd) -m -p -- /system/bin/lshal | grep IComposer
# android.hardware.graphics.composer@2.1::IComposer/default ...

# Ready param flipped
param get android.composer.ready              # 1

# OHOS-side composers up
pidof composer_host allocator_host render_service
# three PIDs

# Boot animation visible on the panel
hilog -x | grep -E "render_service|bootanim|composer_host" | head
```

If the screen stays black despite all checks passing:

- `hilog -x | grep -i 'EGL_BAD\|GL_INVALID\|HwcLayer'` — Phase 6/8 stability issues.
- `/data/log/faultlog/faultlogger/` — service crashes.

## Inheritance map (Phase X → native boot)

| Phase | Inherits? | Notes |
|---|---|---|
| Phase 5 (libhybris) | ✅ | No source change. Library binaries identical; path redirection already in libhybris itself. |
| Phase 6 (display VDIs) | ✅ | `device/soc/oniro/hybris_generic/hardware/display/` unchanged. composer_host cfg gains the gate (N8.4). |
| Phase 7 (input) | ✅ | `CAP_DAC_OVERRIDE` + `z_multimodalinput_caps.cfg` carry over. `/dev/input/*` perms via ueventd. |
| Phase 8.1–8.18 | ⏳ | All reproduce. Bug 8.18 chmod needs porting (see N8.6). |
| Phase 11 (backlight) | ✅ | sysfs writer in `composer_host` works untouched. |
| Phase 12 (sharefs) | ⏳ | LXC bind disappears; replace with kernel port (Phase N9.10) or an equivalent bind in `init.x23.cfg`. |

## Plan adjustments vs prior draft

1. **Removed the "Source-side complete" claim** — N8 is fundamentally a wiring + gating phase, not source delivery. Its prerequisites (N4 + N5) had not been delivered.
2. **N8.1 simplified** — no source relocation, no LXC-bind workaround. libhybris's internal path map handles `/vendor → /android/vendor` for us; native boot inherits.
3. **N8.4 is the only new artifact** — a small cfg overlay gating `composer_host`/`allocator_host` on `android.composer.ready=1`.
4. **Bug 8.18 port called out explicitly** — was in `deploy-lxc-container.sh`; needs to move into the build / init path for native.
5. **Removed mention of `lshal` polling from inside `androidd`'s child** — that's an implementation detail of N4.4, not an N8 deliverable.
