# Phase N5 — Android Container Image

**Status:** 🔄 In Progress (2026-04-30)

A read-only Android rootfs containing exactly the binaries needed for the 5 HAL services, sized to ~50–80 MB for the trimmed variant.

---

## N5.1 — Source the rootfs ✅

**Decision: Option A — bind-mount the existing Halium `system_a` + `vendor_a` directly during bring-up; switch to Option B (squashfs of trimmed Halium content) once Milestone 2 is green.**

Why this two-step pivot:

1. **Bring-up (today):** the launcher accepts a block device path via `ANDROID_ROOTFS_SRC` and `ANDROID_VENDOR_SRC`. When Halium's dynamic partitions are dm-mapped (which they are at boot of a Halium-active device, but **NOT** on first boot of a slot-_b OHOS), pointing at `/dev/mapper/system_a` works zero-copy. This is the path we exercise during in-Halium development.
2. **Production (after Milestone 2):** snapshot the Halium `system_a` and `vendor_a` content into squashfs files (`/var/lib/android/system.sfs`, `vendor.sfs`) shipped *inside* OHOS `system.img`. The launcher loop-mounts them. This eliminates the dependency on Halium dynamic partitions being dm-mapped on a slot-_b boot. ~2 GB cost inside our system.img — acceptable on the X23's 2 GB system_b budget.

### Why not Option C (rebuild stripped Halium tree)

Out of scope for Milestone 4. The current Halium 12 `system_a` is 600 MB and works; rebuilding it from source is multi-day work for marginal size gain.

### Plan adjustment

The original plan described "Option A (recommended for bring-up): mount the existing Halium … directly. No surgery, fast iteration." — that's correct ONLY when Halium's dm-linears exist. Native boot from slot _b means the kernel has *not* loaded slot _a's dynamic-partition metadata; the `/dev/mapper/system_a` device file does NOT exist. So Option A *as the plan described it* doesn't work for the actual native-boot scenario.

**Two practical workarounds, both supported by the launcher (Phase N4.2):**

- **Option A (revised)** — Boot Halium first to set up dm, then `kexec` or `chainload` into OHOS without re-loading firmware. Painful; not pursued.
- **Option B (squashfs)** — preferred. The launcher's loop-mount path handles this directly.

**N5.1 final decision: Option B for the actual native-boot.** Document the snapshot procedure in N5.4.

---

## N5.2 — Trimmed init.rc ✅

**Authored:** `device/board/oniro/hybris_generic/launcher/android-overlay/init.hal-only.rc`

```
import /init.environ.rc
on early-init
    write /proc/sys/kernel/sysrq 0
    write /proc/sys/kernel/modprobe \n

on init
    mkdir /dev/socket 0755 root root
    mkdir /mnt 0775 root system
    chmod 0666 /dev/binder
    chmod 0666 /dev/hwbinder
    chmod 0666 /dev/vndbinder
    setprop ro.hardware mt6789
    setprop ro.hardware.egl mali
    setprop ro.hardware.vulkan mali
    setprop ro.board.platform mt6789
    setprop ro.zygote zygote64
    setprop ro.bionic.arch arm64
    setprop debug.sf.no_hw_vsync 0

on boot
    start hwservicemanager
    start servicemanager
    start vndservicemanager

service hwservicemanager /system/bin/hwservicemanager
service servicemanager   /system/bin/servicemanager
service vndservicemanager /vendor/bin/vndservicemanager /dev/vndbinder
service vendor.hwcomposer-2-1 /vendor/bin/hw/android.hardware.graphics.composer@2.1-service
service vendor.gralloc-4-0    /vendor/bin/hw/android.hardware.graphics.allocator@4.0-service-mediatek
```

(Full file has class/user/group/caps and explicit suppression of zygote/surfaceflinger/system_server.)

**HAL service binary verification (on-device):**

```
$ ls /var/lib/lxc/android/rootfs/vendor/bin/hw/
android.hardware.graphics.allocator@4.0-service-mediatek
android.hardware.graphics.composer@2.1-service
android.hardware.graphics.composer@2.3-service        # also present, libhybris uses 2.1
android.hardware.graphics.composer@2.4-service        # also present, libhybris uses 2.1
... (~30 other HAL services we suppress)
```

The plan claimed the binary path is `/vendor/bin/hw/android.hardware.graphics.composer@2.1-service` — **confirmed**. composer@2.3 and @2.4 are also present in the Halium image; libhybris-hwc2 was built against the @2.1 HIDL surface (Phase 5), so we explicitly start that one.

### Plan adjustments

1. **Service `wait_for_prop` chain.** Halium's vendor `init.mt6789.rc` has `wait_for_prop hwservicemanager.ready "true"` before composer/allocator start. We rely on that — our overlay starts the three service-managers in `on boot`, then the existing Halium vendor init starts composer/allocator after their wait_for_prop unblocks. This matches what `init.hal-only.rc` does today.
2. **`onrestart` between composer and allocator.** Composer depends on allocator (gralloc); we restart allocator if composer dies. Mirrors AOSP convention.
3. **`audioserver` suppression note** — Phase 13B replaced this entire path with native ALSA, so even if we were to boot full Android, audioserver would be unused. Suppression here is purely an optimisation (RAM, fork time).

---

## N5.3 — Pre-seeded properties ✅

The required `setprop` commands are in `init.hal-only.rc` `on init` (above). This works for the squashfs path. For the **block-device path** (bring-up), Halium's existing `init.environ.rc` and `init.mt6789.rc` set the same properties via the bootloader — we don't need to override.

For mimir (Volla Tablet, Android 13 base), additionally set `ro.product.first_api_level=33` so Phase 9.2's libunwindstack `CallStack` hook trips correctly. This goes into a `mimir`-specific overlay we'll create when Phase 9 work is re-validated under native boot.

**Plan adjustment (mimir): defer to Phase 9 re-validation.**

---

## N5.4 — IPC namespace orientation ✅ (analysis)

The plan correctly observed that with `androidd`'s `clone(2)` *not* including `CLONE_NEWIPC`, the child inherits OHOS PID 1's IPC namespace. **Verified in Phase N4.2's source.**

Original LXC config used `lxc.namespace.share.ipc = android` (OHOS sharing Android's IPC ns) — i.e. the inverse direction. The launcher gets the correct direction for free; no string config to get wrong.

---

## N5.5 — Property system (Android side) ✅ (analysis)

Android `/dev/__properties__` is created and managed by Android's own `init` when it brings up the property service. The launcher (Phase N4.2) provides the *mount point* — a fresh tmpfs at `/android/dev/__properties__` — but does not seed any properties. They are independent param/property systems and stay independent, communicated cross-namespace only via hwbinder.

The plan claim "no host-side property bind from OHOS to Android — they're independent param/property systems" is correct and verified.

---

## Squashfs build script (deferred concrete implementation)

To produce the production rootfs:

```bash
# Run from a Halium-booted Volla X23 with both system_a and vendor_a mounted.
sudo mksquashfs /var/lib/lxc/android/rootfs/system  out/hybris_generic/android-system.sfs \
    -comp xz -b 1M -no-fragments -no-duplicates
sudo mksquashfs /var/lib/lxc/android/rootfs/vendor  out/hybris_generic/android-vendor.sfs \
    -comp xz -b 1M -no-fragments -no-duplicates

# Optional: overlay the trimmed init.rc onto the image
# (alternatively, leave Halium's init.rc — it works, just wastes ~30s of fork churn)
sudo unsquashfs -d /tmp/sysroot out/hybris_generic/android-system.sfs
sudo cp \
    out/hybris_generic/packages/phone/images/ohos-rootfs/system/etc/halium-overlay/init.hal-only.rc \
    /tmp/sysroot/init.rc
sudo mksquashfs /tmp/sysroot out/hybris_generic/android-system-trimmed.sfs \
    -comp xz -b 1M -no-fragments -no-duplicates -noappend
```

Then ship the squashfs files inside OHOS `system.img` at `/var/lib/android/system.sfs` and `/var/lib/android/vendor.sfs`. The androidd launcher's loop-mount path picks them up via `ANDROID_ROOTFS_SRC=/var/lib/android/system.sfs`.

Add to `androidd.cfg`:
```
"env" : [
    "ANDROID_ROOTFS_SRC=/var/lib/android/system.sfs",
    "ANDROID_VENDOR_SRC=/var/lib/android/vendor.sfs"
]
```

Defer the actual squashfs build script to Milestone 2 (after the launcher is verified booting against the Halium block-device path under a chrooted reproduction).

---

## N5 plan adjustments emitted

1. **Two-stage rootfs source.** Block device for bring-up (Halium-active context), squashfs for production (slot-_b boot of OHOS without any Halium dm setup).
2. **Halium init.rc usage during bring-up.** `init.hal-only.rc` is an *optimisation* applied via overlay onto the squashfs; not a runtime override of the bound Halium init.rc.
3. **mimir-specific properties** (Android 13 first_api_level=33) deferred to Phase 9 re-validation.
4. **Squashfs creation script** is concrete but deferred — Milestone 2 dependency only.

## Tasks status

- ✅ **N5.1** — Two-stage rootfs strategy decided (block dev → squashfs)
- ✅ **N5.2** — Trimmed `init.hal-only.rc` authored + BUILD.gn target added
- ✅ **N5.3** — Pre-seeded properties documented (in init.hal-only.rc + Halium fallback)
- ✅ **N5.4** — IPC namespace orientation verified (no `CLONE_NEWIPC` in Phase N4.2 launcher)
- ✅ **N5.5** — Property system independence confirmed
- ⏳ **Squashfs build script** — deferred to Milestone 2

## Next phase entry condition

N6 needs: the launcher's `create_binderfs_device("android-binder")` call (✅ in Phase N4.2), the binderfs mount in init.x23.cfg pre-init (✅ in Phase N3.3), and the `/dev/binder` bind into the Android namespace (✅ in Phase N4.2). N6 is mostly ratification + documentation now.
