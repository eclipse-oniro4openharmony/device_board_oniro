# Phase N4 — Android Guest Namespace Launcher (`androidd`)

**Status:** 🔄 In Progress (2026-04-30)

Run the five Android HIDL services inside a child namespace of OHOS PID 1, with the smallest possible runtime footprint. C launcher in lieu of LXC.

---

## N4.1 — Decision: launcher vs LXC ✅

**Decision: ship the C launcher (`androidd`).** Documented LXC fallback path: if cgroup or hwbinder edge cases force us to LXC later, wrap `lxc-start android` invocation around `androidd`'s namespace setup logic — no source rewrite needed since the configuration set is now small.

Confirmation signals from N0 retired the main risk (cross-namespace hwbinder works in production). No reason to take on LXC port debt.

---

## N4.2 — Launcher source ✅

**Authored:** `device/board/oniro/hybris_generic/launcher/androidd.c` (~290 LOC including the verbose error logging and structured comments).

### Behaviour

1. **Pre-flight (still in OHOS root mount NS, OHOS PID 1's child).** Verify `/dev/binderfs/` exists; provision the `android-binder` binderfs context via `BINDER_CTL_ADD` ioctl on `/dev/binderfs/binder-control`. This is the dedicated binder context for the Android servicemanager (Phase N6.3).
2. **Allocate child stack + clone(2).** Flags: `CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD`. Deliberately *not* `CLONE_NEWIPC` (so hwbinder + POSIX MQ cross to OHOS samgr) and *not* `CLONE_NEWNET` (so WiFi from Phase 10 + RIL flow to/from the guest).
3. **Inside the child namespace:**
   - `mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)` — make the parent mount tree private so our work doesn't propagate.
   - `mount("tmpfs", "/android", "tmpfs", 0, "size=64M,mode=755")` — fresh root for pivot_root to land in.
   - Mount the Android rootfs onto `/android/system` and vendor onto `/android/vendor`. Two source modes:
     - **Block device** (e.g. `/dev/mapper/system_a` from a still-mapped Halium dynamic partition): direct ext4 RO mount.
     - **Squashfs file** (e.g. `/var/lib/android/system.sfs` shipped inside OHOS system.img): loop-mount via `LOOP_CTL_GET_FREE` + `LOOP_SET_FD`.
     - Source path is configurable via `ANDROID_ROOTFS_SRC`/`ANDROID_VENDOR_SRC` env vars; Phase N5 picks one and sets them in `androidd.cfg`.
   - `mount("tmpfs", "/android/dev", "tmpfs", 0, "size=8M,mode=755")` — per-namespace `/dev`.
   - `mknod` the minimum bionic-required nodes: `null`, `zero`, `random`, `urandom`, `tty`, `console`. Android's own ueventd will populate the rest after `init` starts.
   - **Phase N6.4 binds:** `/dev/binderfs/android-binder` → `/android/dev/binder` (private to Android), `hwbinder` and `vndbinder` shared with OHOS host.
   - `mount("tmpfs", "/android/dev/__properties__", "tmpfs", 0, "mode=755")` — empty mount-point that Android init populates with the property store.
   - GPU + DMA-BUF passthrough binds: `/dev/mali0`, `/dev/dri/*` (rbind), `/dev/dma_heap/*` (rbind).
   - `mount("proc", "/android/proc", "proc", ...)`, `mount("sysfs", "/android/sys", "sysfs", ...)`.
   - rbind `/data/android` → `/android/data` (Android sees its `/data` as a subdirectory of the OHOS userdata partition).
   - `setenv("ANDROID_ROOT", "/system", 1)`, `ANDROID_DATA`, `ANDROID_VENDOR`, `INIT_USER_RC` (for Phase N5.2 trimmed init.rc).
   - `pivot_root(".", "old_root")` → `umount2("/old_root", MNT_DETACH)` → `rmdir("/old_root")`.
   - `execv("/init", ...)`.
4. **Parent (in OHOS host):** `waitpid` for the child. If the child exits, OHOS init's restart-limit on the service entry kicks in.

### Key non-obvious bits

- `BINDER_CTL_ADD` ioctl number defined inline (`_IOWR('b', 1, struct binderfs_device)`) so we don't depend on a specific `linux/android/binderfs.h` location in the OHOS sysroot.
- `LOOP_CTL_GET_FREE` (0x4C82) + `LOOP_SET_FD` (0x4C00) hard-coded — same rationale, avoiding `<linux/loop.h>` header version drift.
- `pivot_root` invoked via `syscall(SYS_pivot_root, ".", "old_root")` because libc/musl don't expose a wrapper. `chdir(".")` precondition is essential — the kernel resolves both paths in the new namespace's CWD.
- Idempotent: `EEXIST` on the binderfs ioctl is treated as success so a launcher restart after crash works without OHOS first cleaning up `/dev/binderfs/android-binder`.

### Plan deviations vs original draft

1. **No `CLONE_NEWUSER`.** Original suggested keeping the door open; we explicitly skip it. User-namespaces add a capability surface (uid mapping etc.) we don't need yet, and complicate file ownership in `/data/android`.
2. **Bind paths corrected.** The plan had vague `bind_recursive("/dev/block/mapper/system_a:ro", ...)` — that's a colon-RO syntax neither `mount(2)` nor LXC supports literally. The launcher does a normal `mount(...MS_RDONLY...)` instead.
3. **No `mount("tmpfs", "/android", ...)` *at the parent's* mount NS.** Original draft did the tmpfs mount before unsharing — that would propagate to OHOS. The launcher does it AFTER `MS_PRIVATE` + clone, inside the child's NS.
4. **Loop-mount support added.** The plan's "leave Android rootfs in slot _a" only works while Halium's dm-mapping is still active. After native flips to slot _b, slot _a's `super` metadata isn't auto-loaded. We support BOTH paths: block device when the dm exists; loop-mount squashfs when it doesn't (Phase N5 packages it).
5. **`/data/android` instead of `/android/data` from `/data`.** Avoids namespace bleed: if Android's `/data` were exactly `/data`, Android could reach OHOS user data via `..` traversal. Subdirectory keeps the Android user-data tree contained.

---

## N4.3 — Service registration ✅

**Authored:** `device/board/oniro/hybris_generic/launcher/androidd.cfg`

```json
{
    "services" : [{
        "name" : "androidd",
        "path" : [ "/system/bin/androidd" ],
        "caps" : [ "SYS_ADMIN", "MKNOD", "NET_ADMIN", "DAC_OVERRIDE",
                   "SYS_RESOURCE", "SYS_PTRACE", "CHOWN", "FOWNER",
                   "FSETID", "KILL" ],
        "start-mode" : "boot",
        "importance" : -10,
        "ondemand" : false,
        "sandbox" : 0
    }],
    "jobs" : [{
        "name" : "post-fs-data",
        "cmds" : [
            "mkdir /data/android 0755 root root",
            "start androidd"
        ]
    }]
}
```

Started after `post-fs-data` so `/data` is mounted; before `boot && param:bootevent.bootcompleted=true` so the composer is up before render_service starts. `sandbox=0` because androidd needs to set up its own PID/MNT namespace (mksandbox conflicts with that).

Caps include `SYS_PTRACE` so we can debug-attach Android-side processes from OHOS during bring-up. Drop later for production.

### BUILD.gn

`device/board/oniro/hybris_generic/launcher/BUILD.gn`:

```gn
ohos_executable("androidd") {
  sources = [ "androidd.c" ]
  cflags = [ "-Wall", "-Wextra", "-Wno-unused-parameter", "-O2" ]
  install_images = [ "system" ]   # /system/bin/androidd
  part_name = "device_hybris_generic"
  subsystem_name = "device_hybris_generic"
}
ohos_prebuilt_etc("androidd_cfg") {
  source = "./androidd.cfg"
  install_images = [ "system" ]   # /system/etc/init/androidd.cfg
  relative_install_dir = "init"
  part_name = "device_hybris_generic"
}
group("androidd_group") { deps = [ ":androidd", ":androidd_cfg" ] }
```

Hooked into `device/board/oniro/hybris_generic/BUILD.gn`'s `hybris_generic_group` (added `"launcher:androidd_group"` to deps).

---

## N4.4 — OHOS-side composer-readiness gate (deferred)

The plan describes a `start-mode: condition` on `composer_host` waiting on `android.composer.ready=1`, set by `androidd` after polling `hwservicemanager`. This is a Phase N8 concern (graphics gate); we'll wire it once N5 has a running Android namespace to poll against. `composer_host.cfg` lives in `device/soc/oniro/hybris_generic/hardware/display/` (Phase 6) and is the integration point.

**Plan adjustment:** the polling helper from `androidd` itself is the cleanest place for this — when the child reports composer ready (via a unix socket or just a watchdog file at `/data/android/composer-ready`), the parent sets the OHOS param. Defer concrete implementation to N8.

---

## N4.5 — cgroup constraints (deferred, optional)

OHOS init service entries support `cgroup` blocks for memcg/cpuset limits (`init_cgroup.c`). Not blocking for bring-up; we'll add a 512 MB memcg ceiling on `androidd` after Milestone 2 stability is confirmed.

---

## N4 plan adjustments emitted

1. **Loop-mount support** added to handle the case where Halium dynamic partitions aren't dm-mapped at native boot (i.e. when slot _b is active and slot _a's super metadata hasn't been re-loaded by anything).
2. **`/data/android` path** for Android-side user data (not `/data` directly) to prevent namespace traversal bleed.
3. **Idempotent binderfs creation** (treat `EEXIST` as success) so launcher restarts work after crashes.
4. **`pivot_root` via `syscall(SYS_pivot_root, …)`** instead of a libc wrapper; needs the `chdir(".")` precondition.
5. **No `CLONE_NEWUSER`** — keep root mapping; userns adds complexity without buying us anything for the static HAL set.

## Tasks status

- ✅ **N4.1** — Launcher decision finalised; LXC path documented as escape hatch
- ✅ **N4.2** — `androidd.c` authored (~290 LOC, with both block-device and loop-mount rootfs sources)
- ✅ **N4.3** — Service `androidd.cfg` + BUILD.gn authored; integrated into `hybris_generic_group`
- ⏳ **N4.4** — Composer readiness gate deferred to N8 integration
- ⏳ **N4.5** — Cgroup constraints deferred to post-Milestone-2

## Next phase entry condition

N5 needs: a launcher that knows how to mount Android rootfs from either a block device or a squashfs (✅), the trimmed init.rc to ship inside that rootfs (next), and a decision on rootfs source format. Move forward to N5.
