# Phase N8 — Graphics & Display (Native)

**Status:** ✅ Source-side complete (2026-04-30)

`render_service` lights pixels on the panel, reusing all of Phases 5–8 unchanged.

---

## N8.1 — Library path strategy ✅

**Plan adjustment from analysis:** the original plan worried about path collisions and proposed two options:
- (a) Relocate Android-vendor libs to `/system/lib64/hybris_vendor/...` (touches many libhybris source files).
- (b) Keep `/vendor/lib64/...` bind-mount but mount Android libs at `/vendor/lib64/{egl,hw}_android/`.

**Reading the actual libhybris source reveals neither is needed.** Libhybris already has a path-redirect map in `third_party/libhybris/hybris/common/hooks.c:2368`:

```c
{ "/vendor/lib64/egl", "/android/vendor/lib64/egl" },
```

And the linker (`hybris/common/q/linker.cpp:119`):
```c
static const char* const kVendorLibDir = "/android/vendor/lib64";
```

So **libhybris already remaps `/vendor/lib64` to `/android/vendor/lib64`** for any Android-vendor library load. The OHOS-side processes (composer_host, render_service, etc.) load Android libs via libhybris, and libhybris finds them under `/android/vendor/lib64/` — zero `/vendor` collision.

**The actual N8 requirement** is therefore:
1. OHOS host's `/android/system` and `/android/vendor` must be populated before any libhybris-using process starts (composer_host runs in OHOS host mount NS, not inside the Android namespace).
2. The Android namespace (under androidd) gets its own re-mounts at the same paths plus `/system` and `/vendor` post-pivot_root.

**Implementation:** add to `init.x23.cfg` pre-init:
```
exec_start /system/bin/mount -o loop,ro /var/lib/android/system.sfs /android/system
exec_start /system/bin/mount -o loop,ro /var/lib/android/vendor.sfs /android/vendor
```

`mount` and `losetup` are toybox symlinks in `/system/bin/` of the OHOS rootfs (verified). The `mount -o loop` toybox option transparently allocates a `/dev/loopN` and mounts. (The androidd launcher does its own loop-mounts inside its child namespace, parallel to this — see Phase N4.2.)

> Updated `vendor/oniro/hybris_generic/etc/init/init.x23.cfg` with the two `mount -o loop,ro …` lines (2026-04-30).

**The N8.1 LXC-bind workaround from Phase 5 (`lxc.mount.entry = /vendor/lib64/egl vendor/lib64/egl …`) is REPLACED — not needed at all natively.** The 2026-03-20 SPHAL-revert incident referenced in the LXC config (`/var/lib/lxc/openharmony/config:71-79`) was caused by Android libs ending up in OHOS's *own* `/vendor/lib64/`. Native boot's `/vendor` *is* OHOS vendor; Android lives at `/android/vendor`; libhybris's path-redirect map keeps them separate.

---

## N8.2 — EGL/GLES symlinks ✅ (analysis)

The OHOS-side EGL impl symlinks `/system/lib64/libEGL_impl.so → libhybris EGL.so` etc. are shipped by Phase 6 source (`device/soc/oniro/hybris_generic/hardware/display/`). No change needed for native boot — they live in OHOS system, not vendor.

The Android-side EGL impl (`/vendor/lib64/egl/libEGL_mali.so`) is in the Halium Android rootfs we squashfs from `system_a`/`vendor_a`. Libhybris's linker remap (`/vendor/lib64/egl → /android/vendor/lib64/egl`) finds it after the loop-mount in N8.1.

---

## N8.3 — Env vars ✅ (analysis)

`device/board/oniro/hybris_generic/cfg/hybris_graphic_env.cfg` (Phase 6) already sets `HYBRIS_EGLPLATFORM=ohos`, `HYBRIS_LD_LIBRARY_PATH`, and other env vars on the relevant services. **Carries over to native boot unchanged** — the cfg targets services by name, and the services have the same names.

---

## N8.4 — Composer readiness gate ✅ (specification)

Per Phase N4.4 plan, `composer_host` (the OHOS-side display VDI host) needs to wait for the Android composer service to be registered with `hwservicemanager` before it tries to call `IComposer::createClient`.

**Mechanism:**

1. `androidd` (Phase N4.2 launcher) tracks Android namespace state. After the child reports composer ready (via writing `composer-ready` to `/data/android/`), the parent (still in OHOS context) sets `param android.composer.ready=1`.
2. `composer_host.cfg` is augmented to wait on this param:
   ```
   "start-mode" : "condition",
   "condition" : "param:android.composer.ready=1"
   ```

**Concrete polling implementation in androidd:** child opens a Unix datagram socket at `/data/android/composer-ready.sock` and writes "ready\n" once it sees the composer service register. Parent reads from the socket and sets the param. Pseudo-code:

```c
/* In android_child(), shortly before execv("/init"): */
/* (Actually this needs to happen AFTER exec — Android init calls hwservicemanager
 * itself. So instead, the parent polls /android/dev/binder hwservicemanager
 * registration status by stat'ing /data/android/composer-ready, which a
 * lightweight on-init helper inside the Android namespace touches once
 * composer@2.1 is registered.) */
```

**This is plumbing-level work; defer concrete coding to Milestone 2 when we can test against the running Android namespace.** For Milestone 1 (boot to hdc shell), no graphics is needed — composer_host stays unstarted, no harm.

---

## N8.5 — Device-node access ✅

Verified in N2.6 — the vendor ueventd.config sets:
```
/dev/mali0       0666 graphics graphics
/dev/dma_heap/*  0666 graphics graphics
```

Plus the system-side `/system/etc/ueventd.config` sets `/dev/dri/card0` and `/dev/dri/renderD128` perms.

**No additional N8 work for device-node perms.** Inherits from N2.6.

---

## Inheritance from Phases 5–8 ✅

| Phase | Carries over unchanged? | Notes |
|---|---|---|
| Phase 5 (libhybris build) | ✅ | Same library binaries; new path resolution via libhybris's remap (no LXC bind needed). |
| Phase 6 (display VDIs) | ✅ | `composer_host` and `allocator_host` cfg unchanged; composer-ready gate is a new addition (N8.4). |
| Phase 7 (input) | ✅ | `multimodalinput` `CAP_DAC_OVERRIDE` cfg overlay carries over; `/dev/input/*` perms set by N2.6. |
| Phase 8.1–8.18 | ⏳ | All carry over; **expect 8.17 Mali NULL+0x1d8 crash to reproduce** since the EGL teardown sequence is unchanged. Track in §8.17. |
| Phase 11 (backlight) | ✅ | sysfs writer in composer_host already in place. |
| Phase 12 (sharefs) | ⏳ | LXC bind disappears; replace with N9.10 (kernel port or androidd-side bind). |

---

## N8 plan adjustments emitted

1. **N8.1 simplified** — libhybris already remaps `/vendor/lib64 → /android/vendor/lib64`. No source-level relocation work needed. Just ensure OHOS host has `/android/{system,vendor}` populated (loop-mount squashfs in init.x23.cfg pre-init).
2. **The 2026-03-20 SPHAL revert** described in the LXC config doesn't reproduce natively because /vendor IS OHOS vendor; no Android libs land there.
3. **Composer-readiness gate** is a small cross-process signal (Unix socket or param), not a heavy poll loop — defer concrete code to Milestone 2.
4. **Phase 8.17 Mali crash** expected to reproduce; track in `phase8_system_stability.md` §8.17.

## Tasks status

- ✅ **N8.1** — Library path: libhybris already handles remapping; loop-mount squashfs at /android/{system,vendor} in pre-init
- ✅ **N8.2** — EGL/GLES symlinks unchanged (Phase 6 source carries over)
- ✅ **N8.3** — `hybris_graphic_env.cfg` carries over unchanged
- ⏳ **N8.4** — Composer readiness gate: spec authored, code deferred to Milestone 2
- ✅ **N8.5** — Device-node perms set by N2.6 ueventd

## Next phase entry condition

N9 needs: WiFi inheritance from Phase 10 (just rfkill unblock + start services — ✅ done in N3.3), audio from Phase 13B (already native — ✅), backlight from Phase 11 (already in composer_host, no LXC dependency — ✅). N9 is mostly ratification + Bluetooth deferral.
