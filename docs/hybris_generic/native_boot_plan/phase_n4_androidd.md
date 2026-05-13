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

## Current blocker (2026-05-12 PM)

The launcher is wired and reaches `execv("/system/bin/init", ...)` of
Halium's stage-2 init inside the new PID/mount/UTS namespace.  The
**immediate failure** is that Halium init `SIGSEGV`s at startup before
producing any output.  Independent confirmation: from `hdc shell`,
`chroot /android/system /system/bin/init second_stage` exits with
"Signal 11", no stdout/stderr.

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

### Next-session debugging path (in order of cost / diagnostic value)

- **a.** strace-equivalent: build a tiny C wrapper that exec's init
  and uses `ptrace(PTRACE_TRACEME)` to catch the first signal.  Or
  cross-compile `strace` for aarch64-musl and ship it via `hdc file
  send`.  Either gives us the syscall that returned the address that
  caused the segfault.
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
