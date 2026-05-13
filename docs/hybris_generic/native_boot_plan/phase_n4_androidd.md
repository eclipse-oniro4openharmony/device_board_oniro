# Phase N4 — `androidd` Launcher (Halium HAL Guest Namespace)

**Status:** ✅ Source-side complete (2026-05-12 PM); 🚧 on-device validation partial (2026-05-12 PM) — Halium init `SIGSEGV`s at startup before producing output.  See "Current blocker" at the bottom of this doc.

> **Goal.** Run Halium's HIDL HAL services (hwservicemanager + servicemanager + vndservicemanager + composer@2.x + gralloc@4.0) inside a child namespace of OHOS PID 1, so libhybris-using OHOS services (`composer_host`, `allocator_host`, `render_service`) can reach them over the shared `/dev/hwbinder`.

## Dependencies

- **N5** delivered: `/android/system` and `/android/vendor` mounted RO in the OHOS root by the chainload.
- **N6** binder layout (covered below): OHOS uses default `/dev/binder`; Android gets `android-binder` via `BINDER_CTL_ADD`; `hwbinder` + `vndbinder` shared.

---

## N4.1 — Why a C launcher, not LXC

| | C launcher (`androidd`) | LXC port |
|---|---|---|
| Effort | ~1 day | 5–10 days port + debug |
| Runtime size | ~30 KB | ~2 MB + libs |
| Failure modes | clear, our code | many: apparmor, seccomp, cgroup, pivot_root variants |
| Future-proof | drift with our fork | drift with LXC upstream |
| Already-working precedent | Phase 11 host-action hook, Phase N11 chainload | the LXC build, which we're moving away from |

The full LXC machinery exists because LXC supports multi-tenant dynamic container management. We have one container, started once, with a static config. C launcher wins.

LXC remains documented as an escape hatch should cgroup or namespace edge cases emerge.

---

## N4.2 — Source: `launcher/androidd.c`

Add `device/board/oniro/hybris_generic/launcher/androidd.c` (~250 LOC). Sequence:

### Pre-flight (in OHOS root mount NS)

1. Verify `/dev/binderfs/binder-control` exists (binderfs mounted by `init.x23.cfg` pre-init per N3.3).
2. Create `android-binder` device:
   ```c
   #define BINDER_CTL_ADD _IOWR('b', 1, struct binderfs_device)
   struct binderfs_device { char name[256]; __u32 major; __u32 minor; };

   int fd = open("/dev/binderfs/binder-control", O_RDWR | O_CLOEXEC);
   struct binderfs_device dev = {0};
   strncpy(dev.name, "android-binder", sizeof(dev.name) - 1);
   if (ioctl(fd, BINDER_CTL_ADD, &dev) < 0 && errno != EEXIST)
       die("BINDER_CTL_ADD: %s", strerror(errno));
   close(fd);
   ```
   `EEXIST` is success (idempotent across launcher restarts).

3. Allocate child stack (`mmap` 1 MB) and `clone(2)`:
   ```c
   int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD;
   pid_t pid = clone(child, stack_top, flags, NULL);
   ```
   - **No `CLONE_NEWIPC`** — Halium HALs register on `/dev/hwbinder`, which OHOS samgr also uses. They must share the IPC namespace.
   - **No `CLONE_NEWNET`** — WiFi (Phase 10) + future RIL share OHOS's net ns.
   - **No `CLONE_NEWUSER`** — root mapping; keeps file ownership simple.

4. After clone, parent (in OHOS root NS) starts the composer-readiness watchdog (N4.4). `waitpid(pid)` blocks until the child exits; on exit, OHOS init's restart policy kicks in.

### In the child (Halium NS)

5. Make the parent mount tree private so our work doesn't propagate:
   ```c
   mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
   ```

6. Per-NS `/dev` for Halium. The `/android/dev` tree must be writable and isolated from OHOS's `/dev` (different binder, different `__properties__`, different `null`/`zero`/etc.):
   ```c
   mount("tmpfs", "/android/dev", "tmpfs", 0, "size=8M,mode=755");
   mknod_min("/android/dev/null",    S_IFCHR | 0666, makedev(1, 3));
   mknod_min("/android/dev/zero",    S_IFCHR | 0666, makedev(1, 5));
   mknod_min("/android/dev/random",  S_IFCHR | 0666, makedev(1, 8));
   mknod_min("/android/dev/urandom", S_IFCHR | 0666, makedev(1, 9));
   mknod_min("/android/dev/tty",     S_IFCHR | 0666, makedev(5, 0));
   mknod_min("/android/dev/console", S_IFCHR | 0600, makedev(5, 1));
   mkdir("/android/dev/socket", 0755);
   mkdir("/android/dev/binderfs", 0755);
   ```

7. Bind binder devices:
   ```c
   touch("/android/dev/binder");
   touch("/android/dev/hwbinder");
   touch("/android/dev/vndbinder");
   mount("/dev/binderfs/android-binder", "/android/dev/binder",   NULL, MS_BIND, NULL);
   mount("/dev/binderfs/hwbinder",       "/android/dev/hwbinder", NULL, MS_BIND, NULL);
   mount("/dev/binderfs/vndbinder",      "/android/dev/vndbinder", NULL, MS_BIND, NULL);
   ```

8. Per-NS property store (Android init populates it):
   ```c
   mkdir("/android/dev/__properties__", 0755);
   mount("tmpfs", "/android/dev/__properties__", "tmpfs", 0, "mode=755");
   ```

9. GPU + DMA-BUF + input + audio passthrough — rbind from host so Android composer sees the same kernel objects OHOS does:
   ```c
   touch("/android/dev/mali0");
   mount("/dev/mali0",    "/android/dev/mali0",    NULL, MS_BIND, NULL);
   mount("/dev/dri",      "/android/dev/dri",      NULL, MS_BIND | MS_REC, NULL);
   mount("/dev/dma_heap", "/android/dev/dma_heap", NULL, MS_BIND | MS_REC, NULL);
   /* /dev/input not strictly needed for graphics; add when sensors arrive. */
   ```

10. `proc`, `sys`:
    ```c
    mount("proc",   "/android/proc", "proc",  MS_NODEV|MS_NOEXEC|MS_NOSUID, NULL);
    mount("sysfs",  "/android/sys",  "sysfs", MS_NODEV|MS_NOEXEC|MS_NOSUID, NULL);
    ```

11. Per-NS `/data` for Android (subdir of OHOS userdata, *not* `/data` directly — avoids namespace traversal bleed):
    ```c
    mkdir_p("/data/android", 0771);
    mount("/data/android", "/android/data", NULL, MS_BIND, NULL);
    ```

12. Pre-seed boot environment via env vars (Android init maps `androidboot.*` env to `ro.boot.*` props):
    ```c
    setenv("ANDROID_ROOT",   "/system", 1);
    setenv("ANDROID_DATA",   "/data",   1);
    setenv("ANDROID_VENDOR", "/vendor", 1);
    setenv("androidboot.hardware",        "mt6789",    1);
    setenv("androidboot.selinux",         "permissive",1);
    setenv("androidboot.veritymode",      "disabled",  1);
    setenv("androidboot.verifiedbootstate","orange",   1);
    ```

13. `pivot_root` via direct syscall (musl doesn't wrap it):
    ```c
    mkdir("/android/old_root", 0755);
    chdir("/android");
    syscall(SYS_pivot_root, ".", "old_root");
    chdir("/");
    umount2("/old_root", MNT_DETACH);
    rmdir("/old_root");
    ```

14. Exec Halium init. Halium 12's init binary lives at `/system/bin/init` in `halium_system_a`. The boot.img ramdisk's `/init` is not transplanted (we don't need stage-1 boot logic — partitions are already mounted by the chainload):
    ```c
    execl("/system/bin/init", "init", NULL);
    ```

### Non-obvious bits

- **`BINDER_CTL_ADD` ioctl number hard-coded** — `_IOWR('b', 1, struct binderfs_device)` — so we don't depend on a particular `linux/android/binderfs.h` being in the OHOS sysroot.
- **`pivot_root` via `syscall(SYS_pivot_root, ".", "old_root")`** because musl doesn't expose a wrapper. `chdir(".")` before the syscall is mandatory — the kernel resolves both paths relative to the new namespace's CWD.
- **No CLONE_NEWUSER** — root maps cleanly; userns would add uid-mapping complexity without buying anything for a static HAL set.
- **EEXIST on `BINDER_CTL_ADD` = success** — handles launcher restart-after-crash without needing OHOS-side cleanup.

---

## N4.3 — Service registration

### `launcher/androidd.cfg`

```json
{
    "services" : [{
        "name" : "androidd",
        "path" : [ "/system/bin/androidd" ],
        "uid"  : "root",
        "gid"  : [ "root" ],
        "caps" : [ "SYS_ADMIN", "MKNOD", "NET_ADMIN", "DAC_OVERRIDE",
                   "SYS_RESOURCE", "SYS_PTRACE", "CHOWN", "FOWNER",
                   "FSETID", "KILL", "NET_RAW" ],
        "start-mode" : "boot",
        "importance" : -10,
        "ondemand"   : false,
        "sandbox"    : 0,
        "critical"   : [ 1, 5, 60 ]
    }],
    "jobs" : [{
        "name" : "post-fs-data",
        "cmds" : [
            "mkdir /data/android 0771 root root",
            "start androidd"
        ]
    }]
}
```

- `sandbox: 0` — `androidd` is the sandbox; OHOS init's `mksandbox` must not unshare under it.
- `caps` include `SYS_PTRACE` so we can `nsenter -t $(pidof androidd) -m -p` from `hdc shell` during bring-up. Strip for production.
- `start-mode: boot` + post-fs-data start cmd: started early, but only after `/data` is mounted (so `/data/android` mkdir works).
- `critical [1, 5, 60]`: kill the box if androidd restarts 5+ times in 60 s (something is fundamentally wrong).

### `launcher/BUILD.gn`

```gn
import("//build/ohos.gni")

ohos_executable("androidd") {
  sources  = [ "androidd.c" ]
  cflags   = [ "-Wall", "-Wextra", "-Wno-unused-parameter", "-O2" ]
  install_images = [ "system" ]   # /system/bin/androidd
  part_name      = "device_hybris_generic"
  subsystem_name = "device_hybris_generic"
}

ohos_prebuilt_etc("androidd_cfg") {
  source = "./androidd.cfg"
  install_images       = [ "system" ]   # /system/etc/init/androidd.cfg
  relative_install_dir = "init"
  part_name            = "device_hybris_generic"
}

group("androidd_group") {
  deps = [ ":androidd", ":androidd_cfg" ]
}
```

Hook into `device/board/oniro/hybris_generic/BUILD.gn`'s `hybris_generic_group`:

```gn
deps += [ "launcher:androidd_group" ]
```

---

## N4.4 — Composer-readiness gate

`composer_host` and `allocator_host` (Phase 6 VDIs) must not start until the Halium composer service is registered with hwservicemanager. The mechanism:

1. After `clone(2)`, the **parent** of `androidd` (running in the OHOS root NS) polls for the HIDL composer service.
2. Once registered, parent calls `SystemSetParameter("android.composer.ready", "1")`.
3. `composer_host.cfg` carries `start-mode: condition` + `condition: param:android.composer.ready=1`.

### Polling implementation choices (defer concrete code to first bring-up)

- **Easiest:** parent forks a short-lived helper that lives in the Halium NS (`nsenter -t $child -m -p`), runs `lshal` or `getservice` via a tiny HIDL caller, exits 0 when composer registers, exits 1 otherwise. Loop the helper from the parent until exit 0. ~30 lines C.
- **Cleaner:** the OHOS parent itself uses libhidl to call `IComposer::getService("default")` once it appears. Requires linking libhidl in `androidd` — adds binary size; defer.

For Milestone 2 (first bring-up), either works. The polling helper is the smaller commitment.

### `cfg/z_composer_host_gate.cfg` (Phase N8 deliverable)

Shipped under Phase N8, not N4. Adds the `start-mode: condition` overlay to `composer_host` and `allocator_host`. See `phase_n8_graphics_native.md`.

---

## N4.5 — Cgroup constraints — defer

`init_cgroup.c` supports per-service memcg/cpuset blocks. Putting `androidd` in a 512 MB memcg would protect OHOS from a runaway Halium HAL. **Not** blocking for bring-up; add post-Milestone 3 once stability is confirmed.

---

## N4 deliverables

| Item | Path | Status |
|---|---|---|
| Launcher source | `device/board/oniro/hybris_generic/launcher/androidd.c` | ✅ ~370 LOC, libc only |
| Service cfg | `device/board/oniro/hybris_generic/launcher/androidd.cfg` | ✅ `start-mode: boot`, `sandbox: 0`, critical kill on 5+ restarts/60 s |
| Build wiring | `device/board/oniro/hybris_generic/launcher/BUILD.gn` | ✅ `ohos_executable` + `ohos_prebuilt_etc` |
| Group integration | `device/board/oniro/hybris_generic/BUILD.gn` (`hybris_generic_group` deps) | ✅ `launcher:androidd_group` added |
| Composer-ready watchdog | inline in `androidd.c` (parent path) | ✅ setns + fork + `lshal --neat | grep IComposer`, calls `/system/bin/param set` on success |

## Bring-up checklist

Once N4 + N5 are deployed, from `hdc shell`:

```sh
# 1. Verify androidd is alive
pidof androidd

# 2. Verify android-binder exists
ls -l /dev/binderfs/android-binder

# 3. nsenter into the Halium NS, check HIDL registry
strace -f -p $(pidof androidd)  # while observing — alternative
nsenter -t $(pidof androidd) -m -p -- /system/bin/lshal | grep -E "composer|allocator"
# Expected:
#  android.hardware.graphics.composer@2.1::IComposer/default     (...)
#  android.hardware.graphics.allocator@4.0::IAllocator/default   (...)

# 4. Verify the OHOS-side param flipped
param get android.composer.ready
# Expected: 1
```

If step 3 lists composer but step 4 stays empty: the watchdog poll loop didn't fire — debug there.
If step 3 is empty: Halium init failed to start the HAL services — debug from `/android/data/cache/*.log` (Halium logs) or stderr captured by `androidd`'s pipe.

## Plan adjustments vs prior draft

1. **Launcher had not been built.** Earlier doc said "Authored 2026-04-30" but `device/board/oniro/hybris_generic/launcher/` only contains `init-chainload.sh`. Restart from scratch.
2. **No loop-mount path.** Phase N5 now ships Halium content via super.img partitions (mounted by the chainload). `androidd` does pure bind-rebinds — no `LOOP_CTL_GET_FREE` dance.
3. **`/system/bin/init`, not `/init`.** Halium 12's stage-2 init binary is at `/system/bin/init` inside `system_a`. The boot.img ramdisk `/init` is a separate binary we don't transplant. Our launcher does what the chainload does for OHOS — sets up mounts, then `execl` the init binary at its real path.
4. **Composer-ready watchdog lives in the parent.** Earlier drafts vaguely said "androidd notifies somehow". Now: explicit, the OHOS-side parent of `androidd` polls + setparam. Child does its own work.
5. **OHOS uses default `/dev/binder`** (= `/dev/binderfs/binder`), not `ohos-binder`. Native OHOS owns PID 1; samgr registers as the binder context manager first; Android gets `android-binder`. Simpler than the LXC build.

---

## Hard-won lessons

### `nodev` on binderfs silently blocks all device-node opens

Even for uid 0 with all caps.  Returns `EACCES` (not the more honest
`EPERM`/`EACCES` distinction you'd see for a missing cap).  The kernel
binderfs control device is a real char-device (major 1, minor variable)
and `nodev` blocks it.  Drop `nodev` from the mount options for
`binderfs` (we keep `noexec,nosuid` — those are fine).  Cost: hours of
"but I am root" diagnosing.  Removed from `init.x23.cfg`.

### `pivot_root`'s put-old dir must be on a writable filesystem

The kernel materialises a mount-point dentry at the put-old path.
`/android/system/old_root` lives on the RO halium ext4 → `EROFS`.  Use
`/data/old_root` inside the per-NS tmpfs we mount at
`/android/system/data` for this.

### `/data` is RO on native (no userdata partition mounted)

`fstab.x23` only mounts misc + persist, and the kernel cmdline's
`ohos.required_mount.*` doesn't cover userdata.  Anything that needs
writable `/data` (Halium init's per-NS data, OHOS services' state) has
to be backed by a tmpfs or by bringing userdata into fstab.  Not a
blocker for HAL bring-up; will be one for any persistent OHOS app
state.  The launcher backs `/android/system/data` with a tmpfs.

---

## 2026-05-13 PM — Halium init runs; HAL services SEGV on start

**Status:** Source-side fix landed and verified on Volla X23.  Halium
init now reaches `second_stage`, processes `early-init`, `fs`,
`post-fs`, and `init` actions, runs `linkerconfig`, runs
`apexd-bootstrap`, parses every `.rc` file under `/system/etc/init/`,
`/vendor/etc/init/`, etc.  `/linkerconfig/` is fully populated with
proper namespace config (`[system]` section with `additional.namespaces
= com_android_adbd, com_android_art, …`, and per-apex
`ld.config.txt`).  `/apex/` is populated with all 19+ APEX modules
(both via our pre-binds and init's own ActivateFlattenedApexesIfPossible).
`/mnt/{user,installer,androidwritable}` set up by init.

**Remaining blocker:** every HAL service that Halium init tries to
start SIGSEGVs at startup:

```
init: Service 'servicemanager' (pid N) received signal 11
init: Service 'hwservicemanager' (pid N) received signal 11
init: Service 'vndservicemanager' (pid N) received signal 11
init: Service 'vendor.gralloc-4-0' (pid N) received signal 11
init: Service 'vendor.hwcomposer-2-3' (pid N) received signal 11
init: Service 'logd' (pid N) received signal 11
init: Service 'bluetooth-1-1' (pid N) received signal 11
init: Service 'vendor.cas-hal-1-2' (pid N) received signal 11
init: Service 'vendor.drm-clearkey-hal-1-4' (pid N) received signal 11
... (basically every service from /system/etc/init/ and /vendor/etc/init/)
```

Interesting diagnostic: `exec_start vndk-detect` (synchronous one-shot
exec from `on early-init`) runs successfully and exits 0.  But
`exec_start chipinfo` (also one-shot, but at `/vendor/bin/`) SEGVs.
So the SEGV correlates loosely with vendor binaries, but not perfectly
— pure `/system/bin/` services also SEGV.

**Likely candidates for the SEGV (next-session investigation):**

1. **Our kernel has no SELinux** (`/sys/fs/selinux` doesn't exist,
   `/proc/filesystems` has no `selinuxfs`).  Halium 12 init's
   `setcon()` calls for `seclabel` directives in `.rc` files may
   return errors that bionic libc converts into aborts at service
   startup.  The fact that ueventd already reports `Cannot get
   SELinux label on '/dev/...' device: Operation not supported on
   transport endpoint` confirms SELinux is partially broken.
   Fix: either add SELinux support to our X23 kernel, or strip
   `seclabel` from Halium's `.rc` files via a per-service overlay.
2. **Bionic `/proc/self/exe` check.**  In a `chroot $ROOT
   /system/bin/sh` test (chroot without /proc bound) bionic libc
   aborts with `unable to stat "/proc/self/exe": SIGABRT`.  Inside
   our namespace `/proc` IS mounted (`/proc/<halium_pid>/mountinfo`
   confirms a fresh proc fs), so this should not fire — but the
   exact same signature might be hitting under a different cause
   (selinux denial making stat return EACCES on its own /proc/self/exe?).
3. **Seccomp policy enforcement.**  Many Halium services have
   `seccomp_policy /system/etc/seccomp_policy/X.policy` directives.
   If init successfully loads the BPF filter and the binary then
   makes an unallowed syscall during bionic startup, the kernel
   sends SIGSYS — but our reports show SIGSEGV.  So probably not
   seccomp itself.
4. **`writepid /dev/cpuset/...` directives** fail because our /dev
   is a fresh tmpfs without cpuset.  These failures are logged as
   `Unable to write to file`, not fatal.  Ruled out for the SEGV.

**Next-session debug path:** instead of trying to instrument all the
services, pick ONE (e.g. `servicemanager`) and:
- Add a custom `.rc` overlay (binding over `/system/etc/init/servicemanager.rc`
  via androidd's pre-init mount setup) that adds `setenv LD_DEBUG all`
  and `stdio_to_kmsg` so bionic linker prints what it's doing.
- Or run `crash_dump` and check tombstones in `/data/tombstones/` (we have
  /data as tmpfs).
- Or kernel-side: enable CONFIG_SECURITY_SELINUX in the X23 kernel,
  rebuild boot.img.

### Older root cause (already fixed — kept for record)

The linkerconfig + apex-bind fix described below is in tree and
verified on-device: Halium init reaches its second_stage, parses
init.environ.rc, sets `ro.product.*` props, runs `restorecon`, opens
the `property_service` socket — i.e. it gets past the pre-logging SEGV
window.

The new blocker is one step further in: `SetupMountNamespaces()`
issues `mount(NULL, "/apex", NULL, MS_PRIVATE, NULL)` to set up the
APEX mount-namespace propagation, which fails with `EINVAL` because
`/apex` is just a dir on `halium_system_a`, not a mount point.  Init
treats it as fatal and calls `InitFatalReboot(signal 6)`.  Stack
trace from kmsg:

```
init: Failed to remount /apex as 40000: Invalid argument
init: SetupMountNamespaces failed: Invalid argument
init: InitFatalReboot: signal 6
#04 SecondStageMain(...) +13020
```

(MS_PRIVATE = 0x40000.)

**Fix in flight (2026-05-13 PM/2):** mount tmpfs at `/apex` in
`androidd` before the apex_bind calls so init's
`mount(MS_PRIVATE) /apex` finds a real mount point.  Also expanded
the apex_bind list to cover the full set Halium ships
(`adbd/media/media.swcodec/resolv/neuralnetworks/tethering/wifi`)
beyond the runtime-namespace minimum.

### Original root cause (now fixed — kept for record)

The launcher is wired and reaches `execv("/system/bin/init", ...)` of
Halium's stage-2 init inside the new PID/mount/UTS namespace.  The
**immediate failure** is that Halium init `SIGSEGV`s at startup before
producing any output.  Independent confirmation: from `hdc shell`,
`chroot /android/system /system/bin/init second_stage` exits with
"Signal 11", no stdout/stderr.

### Root cause (confirmed 2026-05-13, AOSP source review)

We exec `init second_stage` directly, **skipping first_stage_init**.
First-stage init is what creates `/linkerconfig` as a tmpfs and runs
`/system/bin/bootstrap/linkerconfig --target /linkerconfig/bootstrap`
(see AOSP `system/core/rootdir/init.rc` `on early-init`).  Without
that, when the bionic linker loads `/system/bin/init`'s non-bootstrap
`DT_NEEDED` libs, it tries to read `/linkerconfig/<section>/ld.config.txt`,
finds an empty `/linkerconfig` directory, NULL-derefs a section pointer,
and SEGVs before init's `SecondStageMain` ever runs.

On-device evidence matching the hypothesis:
- `/android/system/linkerconfig/` is an empty dir on `halium_system_a` (the
  ext4 image ships it as a mount point for the runtime tmpfs).
- `/android/system/apex/` is empty too — the flattened APEX modules sit
  under `/android/system/system/apex/<name>/` (e.g.
  `com.android.runtime/`, `com.android.art/`).  Even if the linker gets
  past `/linkerconfig`, bionic resolves runtime-namespace libs through
  `/apex/com.android.runtime/lib64/` which is empty.
- `/android/system/system/etc/linker.config.pb` exists (984 bytes) but is
  consumed by `linkerconfig` to generate `ld.config.txt`, not by the
  linker directly.

### Final fix (deployed 2026-05-13 PM)

`androidd.c` `child_main()` — after `pivot_root` + stdio→/dev/kmsg
redirect, **before** `execv("/system/bin/init", ...)`:

1. **`/apex` tmpfs** — `mount("tmpfs", "/apex", "tmpfs", 0,
   "mode=0755,uid=0,gid=0")`.  `SetupMountNamespaces()` in AOSP init
   does `mount(NULL, "/apex", NULL, MS_PRIVATE)` and aborts with
   `EINVAL` if /apex isn't a mount.
2. **`apex_bind()` each flattened APEX** — for runtime, art, i18n,
   conscrypt, os.statsd, tzdata, adbd, media, media.swcodec, resolv,
   neuralnetworks, tethering, wifi, extservices, ipsec, mediaprovider,
   permission, sdkext, vndk.current.  Each call binds
   `/system/apex/<name>/` → `/apex/<name>/`.
3. **`com.android.vndk.v32` versioned bind** — additional bind of
   `/system/apex/com.android.vndk.current` to
   `/apex/com.android.vndk.v32` because `linkerconfig`'s VNDK loader
   reads `/apex/com.android.vndk.v$VENDOR_VNDK_VERSION/etc/*.libraries.32.txt`
   and Halium 12 = VNDK 32.
4. **`apex_info_list_write()`** — write `/apex/apex-info-list.xml`
   listing every bound apex (linkerconfig consumes this to enumerate
   namespaces).  Init's own apexd later overwrites this with the
   canonical version; ours is fine as a bootstrap.
5. **`/mnt` tmpfs + subdirs** — `SetupMountNamespaces` does
   `mkdir_recursive("/mnt/{user,installer,androidwritable}")` which
   hits EROFS on the RO halium_system_a `/mnt` dir.  Tmpfs + the
   three subdirs.
6. **`/linkerconfig` tmpfs only** — we do NOT run linkerconfig
   ourselves; init runs it later when `ro.vndk.version=32` (read
   from /vendor/build.prop by PropertyLoadBootDefaults) is set.
   Running linkerconfig before init aborts with
   `Check failed: !"undefined var" SANITIZER_DEFAULT_VENDOR is not
   defined` since the VNDK loader reads `ro.vndk.version` to find the
   right APEX path.

Each step is non-fatal so any later regression still produces a clean
kmsg signature instead of a silent SEGV.

### Deferred-but-related observation

The currently-deployed binary (built before 2026-05-13 08:04) has
slightly different paths:

- `mkdir(/android/system/old_root)` (older code) fails with `EROFS`
  because `/android/system` is a RO ext4.  Newer committed source uses
  `/android/system/data/old_root` *after* mounting tmpfs at
  `/android/system/data`, which avoids the EROFS.  The current build
  cycle (the same one that adds the linkerconfig fix above) deploys the
  new path.

### Previously-considered hypotheses (now ruled out)

- **`/init.environ.rc` missing** — exists at `/android/system/init.environ.rc`
  and is reachable post-pivot at `/init.environ.rc`.  Read by `LoadBootScripts()`,
  which runs after `InitKernelLogging` — so a failure there would NOT
  be silent.
- **SELinux policy load** — also runs after `InitKernelLogging`; not
  silent.  And `androidboot.selinux=permissive` is already set.
- **`/proc/self/exe` resolvability** — works (we mount proc fresh in
  child NS).
- **Property service init** — needs `/dev/__properties__` (we have the
  tmpfs); failure would still be post-kmsg-init.

Candidates for the SEGV (ordered by likelihood):

1. **Missing `/init.environ.rc`** — Android init parses this very
   early.  The Halium image *has* `/android/system/init.environ.rc`,
   but after our pivot we need that path resolvable.  Verify with
   `ls /android/system/init.environ.rc`.
2. **SELinux policy load** — Even when `androidboot.selinux=permissive`,
   Halium init still tries to load `/sepolicy` (or `/file_contexts.bin`)
   and may segfault if the binary policy isn't compatible with the
   running kernel.  Halium 12's policy was compiled for the Halium
   5.10 kernel — which IS our kernel, so this might actually work.
3. **Bionic linkerconfig** — Halium 12 expects `/linkerconfig/ld.config.txt`.
   The android-rootfs.img has `/linkerconfig/` as an empty dir; the
   contents are normally generated by `linkerconfig` at first boot,
   read by the dynamic linker.  Without it, libraries may not load.
4. **Property service init** — needs `/dev/__properties__` tmpfs (we
   have that) and `/proc/self/exe` resolvable (should be).
5. **`/dev/hwbinder`/`/dev/binder` context register failure** — the
   binder context manager call assumes single-registration; we use
   `android-binder` for Android's `/dev/binder` so this should be
   fine.

### Next-session debugging path (only if the fix above doesn't land)

If Halium init still SEGVs after the linkerconfig + apex fix:

- **a.** strace-equivalent: build a tiny C wrapper that exec's init
  and uses `ptrace(PTRACE_TRACEME)` to catch the first signal.  Or
  cross-compile `strace` for aarch64-musl and ship it via `hdc file
  send`.  Either gives us the syscall that returned the address that
  caused the segfault.  **Caveat hit on 2026-05-13:** `hdc file send`
  on this build only transfers files up to ~50 KB reliably — larger
  binaries (statically-linked aarch64 strace is 8.5 MB) land as 0-byte
  files even though the command reports success.  Alternative: ship
  the debug tool as an `ohos_executable` baked into `system.img` so
  it goes via super (no hdc transfer needed).
- **b.** Stripped binary — Halium's init binary IS stripped; we won't
  get useful symbols from a core dump.  Pull `linker64`'s mmap log
  via `LD_DEBUG=all` (bionic env var) to see if early loads work.
- **c.** Walk Halium init's source from AOSP (
  `system/core/init/main.cpp::SecondStageMain`) and check each
  pre-`InitLogging` step — most likely candidates are
  `MountKernelFileSystems` and `LoadKernelModules`.
- **d.** Bind-mount **OHOS's** `init` over Halium's `/system/bin/init`
  as a sanity check that the bionic environment can load anything at
  all (it won't, because OHOS uses musl — but the SEGV signature would
  shift, isolating where the problem is).

---

## Hard-won lessons (continued, 2026-05-13)

### `hdc file send` silently truncates large transfers to 0 bytes on
v3.2.0c (aarch64 musl client → OHOS hdcd over USB)

Small text files (16 B) and small binaries (50 KiB) transfer cleanly
with `FileTransfer finish, Size:N, …` confirmation in stderr.  Files
of 100 KiB+ produce the same `FileTransfer finish` confirmation OR an
`[Empty]` line OR `[Fail]ExecuteCommand need connect-key?` — and the
destination ends up as a 0-byte placeholder.  Sometimes the channel
itself locks up afterwards (`The communication channel is being
established`) until a `hdc kill` + reboot.  Workaround: build any
debug binary you need into the OHOS image (super.img), not as an
ad-hoc transfer.  Cost: half a session of trying to ship strace.

### Repeated `chroot /android/system /system/bin/init second_stage`
calls from `hdc shell` can wedge the OHOS hdcd

Running the SEGV repro from a shell several times in succession caused
the device to stop responding to *all* hdc calls.  `hdc list targets`
still listed the device, but every `shell`/`file` invocation returned
`need connect-key`.  Recovery required a long-press power-button
reboot (no fastboot or USB hardware reset path was effective).  Best
practice: reproduce the SEGV once, capture the dmesg, then move to
source-side fixes rather than poking the same crash repeatedly.

---

## 2026-05-14 — HAL service SEGV root-caused: `/dev/null` mode 0644

### Root cause (one sentence)

`mknod_min(/dev/null, 0666, …)` in `child_main()` ran with the inherited
service umask of `022`, so the device node was created with mode `0644`;
when Halium init forks a HAL service and `setresuid()`s it to a non-root
account (`system` 1000, `logd` 1036, etc.), the bionic linker's
`__libc_init_AT_SECURE` immediately calls `open("/dev/null", O_RDWR)` —
which returns `EACCES` for any non-root uid against an `0644` node — and
then calls a parameterised abort helper (`abort_with_code(160)`) that
deliberately stores to a small-int address so the crash signature
(`pc=…+0xe6c, x8=0xa0`) uniquely identifies the call site for
debuggerd.  Every Halium HAL service hit the same path; init never
reached `on boot` because critical services crash-looped.

### The fix

In `androidd.c` `child_main()`, right before the first `mknod_min(…
ANDROID_ROOT "/dev/null" …)`:

```c
umask(0);
```

That single line removes the masking so `0666` stays `0666` and the
device node is world-RW the way Halium init expects.

Verified post-fix:

- `dmesg | grep -c "signal 11"` → 0 over a 25 s window after
  `begetctl service_control start androidd`.
- A static aarch64 diagnostic binary (probe v2 — see "Debug probe
  workflow" below) forks a child, `setresuid(1000,1000,1000)`s it, and
  reports `open(/dev/null,O_RDWR) rc=4` (was rc=-13 EACCES pre-fix).
- The child then `execve("/system/bin/sh", …)` runs sh's bionic
  linker through to clean `_exit(0)` (`wait2 status=0x0`) instead of
  the previous `0xb7f` (`WIFSTOPPED + SIGSEGV`).

Halium init now reaches `late-fs` cleanly (`BOOTPROF: INIT:late-fs` is
the last visible BOOTPROF — `boot` and `class_start core` lines roll
out of the kernel ring buffer behind battery-driver spam before we can
sample, but no crash signatures appear in the buffer).

### Why this signature is uniquely linker-internal

`linker64`'s abort helper at file offset 0xf8e5c is a four-instruction
prologue:

```
f8e5c: stp x29, x30, [sp, #-16]!
f8e60: mov x29, sp
f8e64: mov w8, w0           ; x8 = caller-supplied code
f8e68: mov w0, #0x1
f8e6c: str wzr, [x8]        ; deliberate fault — x8 is in unmapped low memory
f8e70: bl 0x10d5e0          ; (unreachable) call _exit-equivalent
```

Each caller passes a constant in `w0` *before* the `bl 0xf8e5c`.
Direct-disassembly grep against `linker64` shows five callers using
codes `0x14e` (assertion-style, twice), `0xc3`, `0xb9`, and the one
that fired here: **`0xa0` (160) at file offset 0xf8c34** — entered
when an `open(/dev/null, O_RDWR)` returns -1 with `errno != EINTR`.

So whenever the bionic linker SEGVs with `pc=…+0xe6c, x8=0xa0` on a
non-root halium process, the cause is the same: /dev/null isn't
openable RW from that uid.  Worth bookmarking — searching the bionic
source tree for the abort code mapping is harder than it sounds.

### Debug probe workflow

The discovery path used a self-contained static aarch64 binary
(no libc, raw syscalls) bind-mounted into the Halium NS via a debug
overlay mechanism inside `androidd`.  This is now a permanent part of
`androidd.c` and the source tree:

- **Overlay mount**: if `/module_update/halium-debug/` exists on the
  OHOS side, `androidd` binds it into the Halium NS at
  `/data/halium-debug/`, then `MS_REMOUNT|MS_BIND`s it to clear the
  `noexec/nosuid` flags inherited from `/module_update` (otherwise
  pushed binaries can't be executed).
- **Manifest replay**: post-pivot, `androidd` reads
  `/data/halium-debug/overlay.txt` and bind-mounts each `<src> <dst>`
  pair (used during diagnosis to swap `/system/etc/init/init.disabled.rc`
  for a custom .rc).
- **Pre-init probe fork**: if `/data/halium-debug/probe` is present and
  executable, `androidd` `fork()`+`execv`s it once before the main
  `execv("/system/bin/init", …)`.  Output (status + register snapshot
  + `/proc/PID/maps` at PTRACE stops) lands in
  `/module_update/halium-debug/probe2.log`, which is readable from
  OHOS without going through hdc.

To diagnose any future Halium-binary crash:

1. Write a probe variant (see source under
   `device/board/oniro/hybris_generic/launcher/probe/` — to be
   committed) that forks, optionally drops priv, optionally PTRACE_TRACEMEs
   then execv's the target binary.  Static-link with
   `aarch64-linux-gnu-gcc -static -nostdlib -nostartfiles`.
2. `hdc file send` it to `/module_update/halium-debug/probe`.
3. `begetctl service_control stop androidd && begetctl service_control
   start androidd`.
4. `cat /module_update/halium-debug/probe2.log` on the device gives
   the full pre-/post-execve register state.

### Hard-won lessons (added)

- **Service caps need `SYSLOG`** if you want `androidd` (or any
  inherited Halium service) to write to `/dev/kmsg`.  Without it the
  writes silently `EPERM` and `androidd`'s diagnostic
  `logmsg()`→`/dev/kmsg` calls vanish.  `androidd.cfg` now lists
  `SYSLOG` and `DAC_READ_SEARCH`.
- **`/module_update` is mounted `noexec,nosuid,nodev`** on the host.
  Bind-mounts inherit those flags; if you `mount(src, dst, MS_BIND)`
  and then try to exec from `dst`, the exec gets `EACCES`.  Add
  `mount(NULL, dst, NULL, MS_REMOUNT|MS_BIND, NULL)` (with no flags)
  to clear them — that's what makes the debug-overlay mechanism work.
- **`dmesg_restrict=1` and `/dev/kmsg` write permission**.  Even when
  `dmesg_restrict=0`, writing to `/dev/kmsg` from userspace requires
  `CAP_SYSLOG`.  Adjusting file mode (`chmod 0666 /dev/kmsg`) is not
  enough.  Both the read side (dmesg/`cat /proc/kmsg`) and the write
  side enforce the capability check via the LSM hook.
- **`mknod()` applies umask** to its mode argument.  Always
  `umask(0)` (or `chmod` the resulting node) when you want a
  device node mode to be exactly what you passed.  This affects every
  device node `androidd` creates; the lesson generalises to any code
  that does `mknod` for shared device nodes.
