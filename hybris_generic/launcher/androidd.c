/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * androidd — Halium HAL Guest-Namespace Launcher (Phase N4)
 *
 * Runs as an OHOS init service.  Provisions an `android-binder` device on
 * the host binderfs, clones into a child PID/mount/UTS namespace with the
 * IPC (hwbinder) and network namespaces left shared with OHOS, sets up
 * a Halium-style /dev tree, binds vendor + tmpfs /data, pivot_roots into
 * /android/system (the halium android-rootfs.img root), and `exec`s
 * Halium 12's stage-2 init at /system/bin/init.
 *
 * The parent stays in the OHOS root namespace and runs a watchdog that
 * polls for the Halium HIDL composer service to register; on success it
 * flips the OHOS init parameter `android.composer.ready` to "1",
 * unblocking `composer_host` / `allocator_host` (gated by
 * cfg/z_composer_host_gate.cfg).
 *
 * Why a custom launcher instead of LXC:
 *  - One static container, started once: LXC's dynamic / multi-tenant
 *    infrastructure (apparmor, seccomp, dynamic cgroup config, ...) is
 *    pure cost for our use.
 *  - Shared IPC/net NS with OHOS — Halium HALs talk hwbinder to OHOS-side
 *    VDIs and WiFi/RIL share OHOS's network ns.  LXC's NS-share knobs
 *    work but add config complexity over a few extra clone-flag bits.
 *  - We get to keep the binary <40 KB and depend on libc only.
 *
 * The bring-up checklist lives in
 *   device/board/oniro/docs/hybris_generic/native_boot_plan/phase_n4_androidd.md
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/android/binderfs.h>

/*
 * The Halium android-rootfs.img is shaped like a *full* Android root
 * (not just /system content): the partition's root has `/system`,
 * `/vendor`, `/init`, `/data`, etc., and `/bin -> /system/bin` as
 * absolute symlinks.  When we mount halium_system_a at
 * /android/system from OHOS init's PoV, the halium rootfs root is
 * therefore at /android/system/ (NOT /android/).  We pivot into
 * /android/system so Halium init sees its expected layout — /vendor
 * for halium_vendor_a, /system for the in-tree halium /system dir,
 * etc.
 *
 * /android (the OHOS-visible mount-points dir) is used by OHOS-side
 * libhybris callers via env vars (HYBRIS_LD_LIBRARY_PATH=
 * /android/vendor/lib64:/android/system/lib64) — those keep working
 * from the OHOS namespace because the mounts remain visible there.
 * The pivot here is only for the Halium guest's view.
 */
/* ANDROID_ROOT is the OUTER halium_system_a partition root (acct/,
 * apex/, bin/, system/, ...) — the dynamic-partition image's literal
 * top-level FHS.  We pivot the Halium NS into ANDROID_ROOT so Halium
 * init finds itself at /system/bin/init post-pivot (where the inner
 * system/ subdir becomes /system).  All Halium-NS mount setup (/dev,
 * /vendor, /data, ...) happens under ANDROID_ROOT pre-pivot.
 *
 * Pre-2026-05-14 this was "/android/system" — the chainload mounted
 * halium_system_a directly there.  We now bind the inner Android
 * /system content over /android/system (so the OHOS-side libhybris
 * sees its hardcoded /android/system/lib64 etc., matching the LXC
 * build convention), and keep the outer partition root mounted at
 * /halium-system for Halium-NS pivot use.  See init-chainload.sh
 * Stage 3b for the mount layout. */
#define ANDROID_ROOT       "/halium-system"
#define OHOS_HALIUM_VENDOR "/android/vendor"
#define BINDERFS_CONTROL   "/dev/binderfs/binder-control"
#define ANDROID_BINDER     "/dev/binderfs/android-binder"
#define HWBINDER           "/dev/binderfs/hwbinder"
#define VNDBINDER          "/dev/binderfs/vndbinder"

#define CHILD_STACK_SIZE  (1 * 1024 * 1024)

/* Composer-ready watchdog tunables.  Halium init takes ~10 s to come up
 * and another 10–20 s for the HAL services in `class hal`; we poll for
 * up to 5 minutes total before giving up. */
#define WATCHDOG_INITIAL_DELAY_SEC  10
#define WATCHDOG_POLL_INTERVAL_SEC  5
#define WATCHDOG_TIMEOUT_SEC        300
#define COMPOSER_READY_PARAM        "android.composer.ready"

static void logmsg(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    /* /dev/kmsg is the preferred channel — its writes need CAP_SYSLOG, which
     * is in our caps list.  But for diagnostic resilience also mirror to a
     * file in /module_update/ (OHOS tmpfs, RW), where reads don't require
     * any privilege.  Both paths are silent on failure. */
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        char line[600];
        int ln = snprintf(line, sizeof line, "androidd: %s\n", buf);
        if (ln > 0) (void)!write(fd, line, ln);
        close(fd);
    }
    int ffd = open("/module_update/androidd.log",
                   O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (ffd >= 0) {
        char line[600];
        int ln = snprintf(line, sizeof line, "%s\n", buf);
        if (ln > 0) (void)!write(ffd, line, ln);
        close(ffd);
    }
    /* Mirror to stderr too — captured by init in some configurations. */
    dprintf(2, "[androidd] %s\n", buf);
}

#define die(fmt, ...) do { logmsg(fmt, ##__VA_ARGS__); _exit(1); } while (0)

static int touch_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

/* mknod that tolerates EEXIST.  Used for per-NS /dev nodes. */
static int mknod_min(const char *path, mode_t mode, dev_t dev)
{
    if (mknod(path, mode, dev) == 0) return 0;
    return errno == EEXIST ? 0 : -1;
}

/* Track which apex modules we successfully bound, so apex_info_list_write()
 * below can emit a matching apex-info-list.xml.  Linkerconfig reads that
 * XML to discover the apex namespaces it needs to emit in ld.config.txt;
 * without it, the generated config has no apex namespaces and every
 * non-bootstrap HAL service SEGVs in its dynamic linker. */
#define MAX_APEX_BOUND 32
static const char *g_bound_apex[MAX_APEX_BOUND];
static int g_bound_apex_n = 0;

/* Bind /system/apex/<name> over /apex/<name> if the source exists and is a
 * directory.  Halium 12's `system_a` ships flattened APEX modules under
 * /system/apex/<name>/ (not capsule files), so a bind is exactly what the
 * bionic linker namespace resolver needs to satisfy /apex/<name>/lib64/
 * lookups for libc++/libsigchain/libnativebridge/etc.
 *
 * Caller is responsible for being in the Halium guest mount NS — paths are
 * post-pivot (so /system/apex/ resolves to halium's system_a).
 *
 * Idempotent: ENOENT on the source and EBUSY/EEXIST on the dest are not
 * fatal; we want this to be best-effort during bring-up. */
static void apex_bind(const char *name)
{
    char src[PATH_MAX], dst[PATH_MAX];
    snprintf(src, sizeof src, "/system/apex/%s", name);
    snprintf(dst, sizeof dst, "/apex/%s",        name);
    struct stat st;
    if (stat(src, &st) < 0 || !S_ISDIR(st.st_mode)) return;
    if (mkdir(dst, 0755) < 0 && errno != EEXIST) {
        logmsg("apex_bind: mkdir %s: %s", dst, strerror(errno));
        return;
    }
    if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL) < 0) {
        logmsg("apex_bind: bind %s -> %s: %s", src, dst, strerror(errno));
        return;
    }
    if (g_bound_apex_n < MAX_APEX_BOUND)
        g_bound_apex[g_bound_apex_n++] = name;
}

/* Write a minimal /apex/apex-info-list.xml describing every apex we bound
 * via apex_bind() above.  This is what `linkerconfig` consumes
 * (system/linkerconfig/modules/apex.cc::ScanActiveApexes) to discover the
 * apex namespaces it needs to emit in /linkerconfig/<section>/ld.config.txt.
 *
 * Schema: system/apex/apexd/aidl/android/apex/ApexInfo.aidl + the matching
 * XML serializer in apexd_session.cpp.  We omit optional fields (partition,
 * provideSharedApexLibs, etc.) — linkerconfig only requires moduleName +
 * modulePath + preinstalledModulePath + isActive.  versionCode is required
 * by the schema but a stub `1` is accepted in practice.
 *
 * On a stock Android boot, apexd writes this file after mounting each
 * apex; here we skip apexd's role entirely since the binds are stable
 * and apexd would just bail out on "This device does not support
 * updatable APEX" anyway. */
static void apex_info_list_write(void)
{
    int fd = open("/apex/apex-info-list.xml",
                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        logmsg("open /apex/apex-info-list.xml: %s", strerror(errno));
        return;
    }
    dprintf(fd, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    dprintf(fd, "<apex-info-list>\n");
    for (int i = 0; i < g_bound_apex_n; ++i) {
        const char *n = g_bound_apex[i];
        dprintf(fd,
            "  <apex-info moduleName=\"%s\""
            " modulePath=\"/apex/%s\""
            " preinstalledModulePath=\"/system/apex/%s\""
            " versionCode=\"1\""
            " versionName=\"\""
            " isFactory=\"true\""
            " isActive=\"true\""
            " provideSharedApexLibs=\"false\""
            " />\n",
            n, n, n);
    }
    dprintf(fd, "</apex-info-list>\n");
    close(fd);
    logmsg("apex_info_list_write: %d apexes bound", g_bound_apex_n);
}

/* Provision a binderfs device.  Idempotent: EEXIST is success so a
 * launcher restart works without OHOS-side cleanup. */
static int create_binderfs_device(const char *name)
{
    int fd = open(BINDERFS_CONTROL, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        logmsg("open %s: %s", BINDERFS_CONTROL, strerror(errno));
        return -1;
    }
    struct binderfs_device dev;
    memset(&dev, 0, sizeof dev);
    snprintf(dev.name, sizeof dev.name, "%s", name);
    int rc = ioctl(fd, BINDER_CTL_ADD, &dev);
    int saved = errno;
    close(fd);
    if (rc < 0 && saved == EEXIST) return 0;
    if (rc < 0) {
        logmsg("BINDER_CTL_ADD(%s): %s", name, strerror(saved));
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Child: Halium-NS setup + exec /system/bin/init
 * -------------------------------------------------------------------------
 * Runs in the child after clone(2).  Has its own PID/mount/UTS NSes but
 * inherits OHOS's IPC, network, and user NSes (intentional — hwbinder
 * and WiFi must cross).
 */

static int child_main(void *arg)
{
    (void)arg;

    /* Make the parent mount tree private so the new bind mounts we're
     * about to add don't propagate back into OHOS's mount table. */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        die("mount(/, rprivate): %s", strerror(errno));

    /* /android/dev: fresh tmpfs.  We deliberately do NOT bind the host's
     * /dev because Android init treats /dev as writable and would race
     * with OHOS's ueventd on the shared host /dev. */
    if (mkdir_p(ANDROID_ROOT "/dev", 0755) < 0)
        die("mkdir %s/dev: %s", ANDROID_ROOT, strerror(errno));
    if (mount("tmpfs", ANDROID_ROOT "/dev", "tmpfs", 0,
              "size=8M,mode=755") < 0)
        die("mount tmpfs on %s/dev: %s", ANDROID_ROOT, strerror(errno));

    /* Clear umask BEFORE mknod_min — mknod(2) applies the umask to its mode
     * argument, so with the inherited umask 022 our `0666` becomes `0644`.
     * Bionic linker's `open("/dev/null", O_RDWR)` in __libc_init_AT_SECURE
     * then fails with EACCES for any non-root service (uid 1000, etc.) and
     * the linker aborts with abort_with_code(160) — the exact crash
     * signature we observed for every Halium HAL service. */
    umask(0);

    /* Minimal device nodes Halium init expects to find. */
    mknod_min(ANDROID_ROOT "/dev/null",    S_IFCHR | 0666, makedev(1, 3));
    mknod_min(ANDROID_ROOT "/dev/zero",    S_IFCHR | 0666, makedev(1, 5));
    mknod_min(ANDROID_ROOT "/dev/random",  S_IFCHR | 0666, makedev(1, 8));
    mknod_min(ANDROID_ROOT "/dev/urandom", S_IFCHR | 0666, makedev(1, 9));
    mknod_min(ANDROID_ROOT "/dev/tty",     S_IFCHR | 0666, makedev(5, 0));
    mknod_min(ANDROID_ROOT "/dev/console", S_IFCHR | 0600, makedev(5, 1));
    /* /dev/kmsg = our diagnostic channel.  Halium init writes its
     * progress to /dev/kmsg by default; without this node, any logging
     * gets silently dropped, making post-mortem of init failures
     * impossible.  Bind the host's /dev/kmsg in directly so we don't
     * depend on the per-NS tmpfs honouring mknod(maj=1,min=11). */
    if (touch_file(ANDROID_ROOT "/dev/kmsg") < 0)
        die("touch kmsg: %s", strerror(errno));
    if (mount("/dev/kmsg", ANDROID_ROOT "/dev/kmsg",
              NULL, MS_BIND, NULL) < 0)
        die("bind /dev/kmsg: %s", strerror(errno));
    mkdir_p(ANDROID_ROOT "/dev/socket",    0755);
    mkdir_p(ANDROID_ROOT "/dev/binderfs",  0755);

    /* Bind the three binder devices into Android's /dev.  Android's init
     * opens /dev/binder unconditionally; we bind our `android-binder`
     * (provisioned by the parent) to that path so Android sees its own
     * context.  hwbinder and vndbinder are SHARED with OHOS — that's
     * the entire point of this architecture.
     */
    touch_file(ANDROID_ROOT "/dev/binder");
    touch_file(ANDROID_ROOT "/dev/hwbinder");
    touch_file(ANDROID_ROOT "/dev/vndbinder");
    if (mount(ANDROID_BINDER, ANDROID_ROOT "/dev/binder",
              NULL, MS_BIND, NULL) < 0)
        die("bind %s -> %s/dev/binder: %s",
            ANDROID_BINDER, ANDROID_ROOT, strerror(errno));
    if (mount(HWBINDER, ANDROID_ROOT "/dev/hwbinder",
              NULL, MS_BIND, NULL) < 0)
        die("bind hwbinder: %s", strerror(errno));
    if (mount(VNDBINDER, ANDROID_ROOT "/dev/vndbinder",
              NULL, MS_BIND, NULL) < 0)
        die("bind vndbinder: %s", strerror(errno));

    /* binderfs creates device nodes mode 0600 root:root.  Halium's
     * init.rc tries to chmod 0666 /dev/binderfs/{binder,hwbinder,vndbinder},
     * but that path doesn't exist inside our NS (we only bind the
     * individual devices into /dev/{binder,hwbinder,vndbinder}).  Without
     * 0666, any service running as a non-root uid (servicemanager and
     * hwservicemanager both run as system=1000) gets EACCES opening
     * binder and aborts via libbinder's CHECK in initialize().
     * chmod here propagates through the bind to the underlying inode. */
    chmod(ANDROID_ROOT "/dev/binder",    0666);
    chmod(ANDROID_ROOT "/dev/hwbinder",  0666);
    chmod(ANDROID_ROOT "/dev/vndbinder", 0666);

    /* Per-NS property store.  Halium init populates /dev/__properties__
     * with the property-area files; we just provide the empty tmpfs. */
    if (mkdir(ANDROID_ROOT "/dev/__properties__", 0755) < 0 && errno != EEXIST)
        die("mkdir __properties__: %s", strerror(errno));
    if (mount("tmpfs", ANDROID_ROOT "/dev/__properties__", "tmpfs", 0,
              "mode=755") < 0)
        die("mount tmpfs on __properties__: %s", strerror(errno));

    /* GPU + DMA-BUF + DRM passthrough — bind the host kernel objects so
     * Halium's composer sees the same Mali / DMA-BUF nodes OHOS does.
     * /dev/mali0 is a single character device; /dev/dri and /dev/dma_heap
     * are directories, so MS_REC.
     */
    if (touch_file(ANDROID_ROOT "/dev/mali0") < 0)
        die("touch mali0: %s", strerror(errno));
    if (mount("/dev/mali0", ANDROID_ROOT "/dev/mali0", NULL, MS_BIND, NULL) < 0)
        logmsg("bind /dev/mali0 failed (non-fatal): %s", strerror(errno));
    if (mkdir(ANDROID_ROOT "/dev/dri", 0755) < 0 && errno != EEXIST) { }
    if (mount("/dev/dri", ANDROID_ROOT "/dev/dri",
              NULL, MS_BIND | MS_REC, NULL) < 0)
        logmsg("bind /dev/dri failed (non-fatal): %s", strerror(errno));
    if (mkdir(ANDROID_ROOT "/dev/dma_heap", 0755) < 0 && errno != EEXIST) { }
    if (mount("/dev/dma_heap", ANDROID_ROOT "/dev/dma_heap",
              NULL, MS_BIND | MS_REC, NULL) < 0)
        logmsg("bind /dev/dma_heap failed (non-fatal): %s", strerror(errno));

    /* proc + sysfs inside the new PID/mount NS.  proc must be a fresh
     * mount in the child NS so /proc reflects the child's PID view. */
    if (mkdir_p(ANDROID_ROOT "/proc", 0755) < 0) { }
    if (mount("proc", ANDROID_ROOT "/proc", "proc",
              MS_NODEV | MS_NOEXEC | MS_NOSUID, NULL) < 0)
        die("mount proc on %s/proc: %s", ANDROID_ROOT, strerror(errno));
    if (mkdir_p(ANDROID_ROOT "/sys", 0755) < 0) { }
    if (mount("sysfs", ANDROID_ROOT "/sys", "sysfs",
              MS_NODEV | MS_NOEXEC | MS_NOSUID, NULL) < 0)
        die("mount sysfs: %s", strerror(errno));

    /* Bind halium_vendor_a (currently mounted at /android/vendor from
     * OHOS PoV — see chainload Stage 3b) ONTO the halium rootfs's
     * /vendor.  After pivot_root into ANDROID_ROOT (= /android/system),
     * the halium guest will see this as its /vendor — exactly what
     * the rc files under /system/etc/init/ reference (/vendor/lib64,
     * /vendor/bin/hw, ...). */
    if (mount(OHOS_HALIUM_VENDOR, ANDROID_ROOT "/vendor",
              NULL, MS_BIND | MS_REC, NULL) < 0)
        die("bind %s -> %s/vendor: %s",
            OHOS_HALIUM_VENDOR, ANDROID_ROOT, strerror(errno));

    /* Per-NS /data for Android — fresh tmpfs.  OHOS doesn't currently
     * have a separate userdata partition mounted (fstab.x23 only mounts
     * misc + persist), so /data on OHOS is just an RO subdir of
     * system_a.  Halium init expects /data writable, so we back it
     * with a tmpfs that lives only for the lifetime of this NS.
     * Trade-off: anything Halium writes to /data is lost across
     * reboots — fine for HALs, would matter only if we wanted Android
     * userspace apps (we don't). */
    if (mkdir(ANDROID_ROOT "/data", 0771) < 0 && errno != EEXIST) { }
    if (mount("tmpfs", ANDROID_ROOT "/data", "tmpfs", 0,
              "size=64M,mode=771,uid=0,gid=0") < 0)
        die("mount tmpfs on %s/data: %s", ANDROID_ROOT, strerror(errno));

    /* Debug overlay: if the OHOS-side directory /module_update/halium-debug/
     * exists, bind it into the Halium NS at /data/halium-debug/ so we can
     * push debug payloads from outside (hdc file send) without rebuilding
     * androidd.  /module_update is the only writable tmpfs on OHOS native
     * boot.  Post-pivot we read /data/halium-debug/overlay.txt for a list
     * of "src dst" pairs to bind-mount over Halium paths (eg replace
     * /system/etc/init/servicemanager.rc with a debug version, or
     * /system/bin/servicemanager with a wrapper script).
     *
     * /module_update is mounted nosuid,noexec,nodev on the host, and bind
     * mounts inherit those flags.  Remount the bind as suid+exec+dev so
     * Halium init can exec scripts/binaries from the overlay. */
    {
        struct stat st;
        if (stat("/module_update/halium-debug", &st) == 0 && S_ISDIR(st.st_mode)) {
            if (mkdir(ANDROID_ROOT "/data/halium-debug", 0755) < 0 && errno != EEXIST) { }
            if (mount("/module_update/halium-debug",
                      ANDROID_ROOT "/data/halium-debug",
                      NULL, MS_BIND | MS_REC, NULL) < 0)
                logmsg("bind halium-debug: %s (non-fatal)", strerror(errno));
            else if (mount(NULL, ANDROID_ROOT "/data/halium-debug", NULL,
                           MS_REMOUNT | MS_BIND, NULL) < 0)
                logmsg("remount halium-debug exec: %s (non-fatal)",
                       strerror(errno));
            else
                logmsg("halium-debug overlay attached (exec-enabled)");
        }
    }

    /* Seed Halium boot env.  Android init maps androidboot.* env vars to
     * ro.boot.* properties at second-stage start. */
    setenv("ANDROID_ROOT",   "/system", 1);
    setenv("ANDROID_DATA",   "/data",   1);
    setenv("ANDROID_VENDOR", "/vendor", 1);
    setenv("androidboot.hardware",          "mt6789",     1);
    setenv("androidboot.selinux",           "permissive", 1);
    setenv("androidboot.veritymode",        "disabled",   1);
    setenv("androidboot.verifiedbootstate", "orange",     1);
    setenv("androidboot.slot_suffix",       "_a",         1);

    /* pivot_root into /android/system.  musl doesn't expose a wrapper;
     * use the raw syscall.  put-old must be a subdir of the new root,
     * and must be on a writable filesystem (since the kernel creates a
     * mount-point dentry there for the old root).  /android/system
     * itself is RO (halium_system_a ext4) so we use /data/old_root —
     * the per-NS tmpfs we just mounted at ANDROID_ROOT/data.  chdir(".")
     * before the syscall is mandatory — the kernel resolves both args
     * relative to the calling task's CWD. */
    if (mkdir(ANDROID_ROOT "/data/old_root", 0755) < 0 && errno != EEXIST)
        die("mkdir /data/old_root: %s", strerror(errno));
    if (chdir(ANDROID_ROOT) < 0)
        die("chdir %s: %s", ANDROID_ROOT, strerror(errno));
    if (syscall(SYS_pivot_root, ".", "data/old_root") < 0)
        die("pivot_root: %s", strerror(errno));
    if (chdir("/") < 0)
        die("chdir /: %s", strerror(errno));
    /* Detach the old root so OHOS-side mounts aren't visible from inside
     * Halium.  MNT_DETACH because some of OHOS's mounts (e.g. /system)
     * are in active use and EBUSY would fire on plain umount. */
    if (umount2("/data/old_root", MNT_DETACH) < 0)
        logmsg("umount2 /data/old_root: %s (non-fatal)", strerror(errno));
    rmdir("/data/old_root");

    /* Redirect stdout/stderr to /dev/kmsg so Halium init's early
     * messages (before init.rc opens its own log) reach our dmesg.
     * Done *before* the linkerconfig + apex setup below so any errors
     * from those steps land in kmsg too. */
    int kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (kfd >= 0) {
        (void)dup2(kfd, 1);
        (void)dup2(kfd, 2);
        if (kfd != 1 && kfd != 2) close(kfd);
    }

    /* ---------------------------------------------------------------------
     * /apex first — `linkerconfig` (run further down) reads /apex/
     * to discover runtime namespaces and emits matching ld.config.txt
     * stanzas; without the binds in place the generated config has no
     * com.android.runtime entry and every non-bootstrap binary (every
     * HAL service) SEGVs in its linker.
     *
     * Three layered concerns:
     *  (a) `SetupMountNamespaces()` in AOSP init does
     *      `mount(NULL, "/apex", NULL, MS_PRIVATE, NULL)` — fails with
     *      EINVAL unless /apex is a mount point.
     *  (b) Bionic resolves runtime-namespace libs (libc++ etc.) via
     *      /apex/com.android.runtime/lib64/.
     *  (c) `linkerconfig` enumerates /apex/<name>/apex_manifest.pb to
     *      build the namespace list.
     *
     * Halium 12 ships flattened APEX modules as directories under
     * /system/apex/<name>/ (post-pivot path).  Tmpfs over /apex then
     * bind those subdirs in.  Init's apexd would later replace these
     * binds with the canonical layout, but the binds are sufficient
     * for the linker and for `SetupMountNamespaces` to succeed.
     * ------------------------------------------------------------------ */
    if (mount("tmpfs", "/apex", "tmpfs", 0,
              "mode=0755,uid=0,gid=0") < 0)
        logmsg("mount tmpfs on /apex: %s (non-fatal — may already be tmpfs)",
               strerror(errno));
    apex_bind("com.android.runtime");
    apex_bind("com.android.art");
    apex_bind("com.android.i18n");
    apex_bind("com.android.conscrypt");
    apex_bind("com.android.os.statsd");
    apex_bind("com.android.tzdata");
    apex_bind("com.android.adbd");
    apex_bind("com.android.media");
    apex_bind("com.android.media.swcodec");
    apex_bind("com.android.resolv");
    apex_bind("com.android.neuralnetworks");
    apex_bind("com.android.tethering");
    apex_bind("com.android.wifi");
    apex_bind("com.android.extservices");
    apex_bind("com.android.ipsec");
    apex_bind("com.android.mediaprovider");
    apex_bind("com.android.permission");
    apex_bind("com.android.sdkext");
    apex_bind("com.android.vndk.current");

    /* The VNDK apex ships its lib lists named *.libraries.<vndk_ver>.txt
     * (e.g. llndk.libraries.32.txt for Halium 12).  Linkerconfig also
     * expects the apex to be reachable at /apex/com.android.vndk.v<ver>/
     * (versioned name).  Without the versioned bind, the variable loader
     * fails to open the per-version VNDK libraries files and aborts
     * every VNDK lookup with "SANITIZER_DEFAULT_VENDOR is not defined".
     *
     * Bind the same /system/apex/com.android.vndk.current under the
     * versioned path too.  Halium 12 = Android 12 = VNDK 32; bump this
     * when porting to a newer Halium. */
    {
        const char *src = "/system/apex/com.android.vndk.current";
        struct stat st;
        if (stat(src, &st) == 0 && S_ISDIR(st.st_mode)) {
            mkdir("/apex/com.android.vndk.v32", 0755);
            if (mount(src, "/apex/com.android.vndk.v32",
                      NULL, MS_BIND | MS_REC, NULL) < 0)
                logmsg("bind vndk.current -> vndk.v32: %s", strerror(errno));
            else if (g_bound_apex_n < MAX_APEX_BOUND)
                g_bound_apex[g_bound_apex_n++] = "com.android.vndk.v32";
        }
    }

    /* Write the apex-info-list.xml that linkerconfig consumes (below).
     * Without it, linkerconfig prints
     *  `Failed to scan APEX modules : Can't read /apex/apex-info-list.xml`
     * and emits a config with NO apex namespaces — every non-bootstrap
     * binary that DT_NEEDs an apex-namespace lib (libc++ via runtime,
     * etc.) then SEGVs in its linker on load.
     *
     * Init's own apexd would normally generate this file as part of
     * apex mount, but on a non-updatable-apex device it exits early
     * with "This device does not support updatable APEX" and writes
     * nothing.  So we generate it ourselves. */
    apex_info_list_write();

    /* ---------------------------------------------------------------------
     * /mnt — `SetupMountNamespaces()` mkdir_recursive's
     * /mnt/{user,installer,androidwritable} early.  Halium 12's
     * `system_a` ships `/mnt` as an empty dir on the RO ext4, so the
     * mkdirs hit EROFS and init aborts with
     *   `SetupMountNamespaces failed: Read-only file system`.
     *
     * Fix: tmpfs over /mnt, then pre-create the three subdirs init
     * looks for (mode 0755 to match what mkdir_recursive uses).
     * ------------------------------------------------------------------ */
    if (mount("tmpfs", "/mnt", "tmpfs", 0,
              "mode=0755,uid=0,gid=0") < 0) {
        logmsg("mount tmpfs on /mnt: %s (non-fatal — may already be tmpfs)",
               strerror(errno));
    } else {
        mkdir("/mnt/user",            0755);
        mkdir("/mnt/installer",       0755);
        mkdir("/mnt/androidwritable", 0755);
    }

    /* ---------------------------------------------------------------------
     * /linkerconfig — tmpfs only; let init's own init.rc do the actual
     * `linkerconfig` invocation.  We tried running it pre-emptively
     * here, but on a clean boot `ro.vndk.version` isn't a property yet
     * and the binary aborts with "SANITIZER_DEFAULT_VENDOR is not
     * defined" (it expects `ro.vndk.version` set to look up the VNDK
     * APEX libs lists).  Init's init.rc runs linkerconfig later, when
     * `init.environ.rc` has set the prop, so it just works.  Halium
     * ships /linkerconfig as an empty dir; we just provide a writable
     * tmpfs.
     * ------------------------------------------------------------------ */
    mkdir("/linkerconfig", 0755);
    if (mount("tmpfs", "/linkerconfig", "tmpfs", 0,
              "mode=0755,uid=0,gid=0") < 0) {
        logmsg("mount tmpfs on /linkerconfig: %s (non-fatal — may already be tmpfs)",
               strerror(errno));
    }

    /* Apply debug-overlay manifest if present.  Each non-empty, non-comment
     * line is "<src-path> <dst-path>" — src is post-pivot (typically under
     * /data/halium-debug/, populated via the OHOS-side /module_update bind
     * above), dst is the Halium path to bind over.  This lets us swap a
     * single .rc or binary for instrumentation without a full rebuild. */
    {
        FILE *fp = fopen("/data/halium-debug/overlay.txt", "r");
        if (fp) {
            char line[512];
            int n_applied = 0;
            while (fgets(line, sizeof line, fp)) {
                size_t L = strlen(line);
                while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r' ||
                                 line[L-1] == ' '  || line[L-1] == '\t'))
                    line[--L] = 0;
                char *p = line;
                while (*p == ' ' || *p == '\t') ++p;
                if (*p == 0 || *p == '#') continue;
                char *sp = strchr(p, ' ');
                if (!sp) { logmsg("overlay: bad line: %s", p); continue; }
                *sp++ = 0;
                while (*sp == ' ' || *sp == '\t') ++sp;
                if (mount(p, sp, NULL, MS_BIND, NULL) < 0)
                    logmsg("overlay bind %s -> %s: %s", p, sp, strerror(errno));
                else {
                    logmsg("overlay bind %s -> %s OK", p, sp);
                    ++n_applied;
                }
            }
            fclose(fp);
            logmsg("overlay: %d binds applied", n_applied);
        }
    }

    /* Diagnostic: if /data/halium-debug/probe exists (deposited by the
     * overlay), fork+exec it once before exec'ing Halium init.  This lets
     * us run a known-good static binary inside the Halium NS to confirm
     * that static binaries work — isolating any "all Halium binaries
     * SEGV" failure to the dynamic linker / libc path. */
    {
        struct stat st;
        if (stat("/data/halium-debug/probe", &st) == 0 && (st.st_mode & 0111)) {
            logmsg("forking probe for pre-init diagnostic");
            pid_t pp = fork();
            if (pp == 0) {
                char *probe_argv[] = { (char *)"probe", NULL };
                execv("/data/halium-debug/probe", probe_argv);
                logmsg("exec probe: %s", strerror(errno));
                _exit(127);
            } else if (pp > 0) {
                int s;
                waitpid(pp, &s, 0);
                logmsg("probe exited (status 0x%x WEXITSTATUS=%d WTERMSIG=%d)",
                       s, WEXITSTATUS(s), WTERMSIG(s));
            } else {
                logmsg("fork for probe: %s", strerror(errno));
            }
        }
    }

    /* /system/bin/init is Halium's stage-2 init binary.  Halium 12's
     * boot.img ramdisk's /init is a separate stage-1 (not used here —
     * we've already done partition/mount setup via the chainload). */
    char *argv[] = { (char *)"init", (char *)"second_stage", NULL };
    execv("/system/bin/init", argv);
    /* On exec failure logmsg falls back to the parent's /dev/kmsg path
     * (different mount NS, but it can still open() the kernel device). */
    die("exec /system/bin/init: %s", strerror(errno));
    return 1;
}

/* -------------------------------------------------------------------------
 * Parent: composer-ready watchdog
 * -------------------------------------------------------------------------
 * Periodically setns()es into the child's PID + mount NSes and runs
 * /system/bin/lshal | grep IComposer/default.  On success, flips the
 * OHOS init parameter via /system/bin/param.
 *
 * Rationale for the polling approach:
 *  - Direct hwbinder transaction would be the cleanest signal but
 *    requires linking libhidl + speaking the HIDL wire protocol in C.
 *  - lshal already does exactly this query and ships with Halium.
 *  - The setns-then-fork pattern is needed because CLONE_NEWPID only
 *    affects newly forked children of the setns'er; the setns()er
 *    itself stays in its current PID NS.
 */
static int probe_composer(pid_t child_pid)
{
    char ns_pid[64], ns_mnt[64];
    snprintf(ns_pid, sizeof ns_pid, "/proc/%d/ns/pid", child_pid);
    snprintf(ns_mnt, sizeof ns_mnt, "/proc/%d/ns/mnt", child_pid);

    int fd_pid = open(ns_pid, O_RDONLY | O_CLOEXEC);
    int fd_mnt = open(ns_mnt, O_RDONLY | O_CLOEXEC);
    if (fd_pid < 0 || fd_mnt < 0) {
        if (fd_pid >= 0) close(fd_pid);
        if (fd_mnt >= 0) close(fd_mnt);
        return -1;
    }

    pid_t probe = fork();
    if (probe < 0) { close(fd_pid); close(fd_mnt); return -1; }

    if (probe == 0) {
        /* setns(CLONE_NEWNS) fails with EINVAL when the calling task's
         * fs_struct has more than one user — and bionic's pthreads or
         * a clone()'d child can keep it shared.  unshare(CLONE_FS) is
         * the bionic-friendly way to detach, but musl is fine without.
         * Belt-and-braces: call it before the setns. */
        if (unshare(CLONE_FS) < 0)
            logmsg("probe: unshare(CLONE_FS): %s", strerror(errno));

        /* Enter Halium NSes.  Order: mount NS first (so /proc remounts
         * correctly), then PID NS for the subsequent fork.
         *
         * Note: setns(CLONE_NEWNS) swaps the *mount namespace* but does
         * NOT change the calling task's fs_struct root or cwd.  After
         * Halium init pivot_roots into /root inside the new mount NS, the
         * NS still has the underlying initramfs visible at "/" but
         * Halium's effective root (where /system/bin/sh and lshal live)
         * is `/root`.  We have to chroot/chdir into it explicitly, else
         * `/system/bin/sh` resolves to OHOS's sh and `/system/bin/lshal`
         * isn't found at all. */
        if (setns(fd_mnt, CLONE_NEWNS)  < 0) {
            logmsg("probe: setns mnt: %s", strerror(errno));
            _exit(3);
        }
        if (setns(fd_pid, CLONE_NEWPID) < 0) {
            logmsg("probe: setns pid: %s", strerror(errno));
            _exit(4);
        }
        close(fd_pid); close(fd_mnt);

        /* Forked grandchild lives in the new PID NS — necessary for any
         * binder/hwservicemanager call that walks /proc/self. */
        pid_t grand = fork();
        if (grand < 0) _exit(5);
        if (grand == 0) {
            if (chroot("/root") < 0) _exit(8);
            if (chdir("/")     < 0) _exit(9);
            execl("/system/bin/sh", "sh", "-c",
                  "/system/bin/lshal --neat 2>/dev/null"
                  " | grep -Eq '@2[.][0-9]+::IComposer/default'",
                  (char *)NULL);
            _exit(127);
        }
        int s;
        if (waitpid(grand, &s, 0) < 0) _exit(6);
        _exit(WIFEXITED(s) ? WEXITSTATUS(s) : 7);
    }

    close(fd_pid); close(fd_mnt);
    int s;
    if (waitpid(probe, &s, 0) < 0) return -1;
    int rc = WIFEXITED(s) ? WEXITSTATUS(s) : -1;
    static int last_rc = -2;
    if (rc != last_rc) {
        logmsg("probe_composer: rc=%d (status=0x%x)", rc, s);
        last_rc = rc;
    }
    return rc;
}

static void watchdog(pid_t child_pid)
{
    logmsg("watchdog: sleeping %ds for Halium init",
           WATCHDOG_INITIAL_DELAY_SEC);
    sleep(WATCHDOG_INITIAL_DELAY_SEC);

    time_t deadline = time(NULL) + WATCHDOG_TIMEOUT_SEC;
    int iter = 0;
    while (time(NULL) < deadline) {
        /* If the child died, the binder NS is gone; abort. */
        if (kill(child_pid, 0) < 0 && errno == ESRCH) {
            logmsg("watchdog: child exited before composer registered");
            return;
        }

        int rc = probe_composer(child_pid);
        if (rc == 0) {
            logmsg("watchdog: IComposer registered (iter %d)", iter);
            /* Flip OHOS init param via /system/bin/param.  Linking
             * libbegetutil is cleaner but pulls in C++ symbols; the
             * subprocess cost is negligible (one-shot). */
            pid_t p = fork();
            if (p == 0) {
                execl("/system/bin/param", "param",
                      "set", COMPOSER_READY_PARAM, "1", (char *)NULL);
                _exit(127);
            }
            int s;
            waitpid(p, &s, 0);
            if (!WIFEXITED(s) || WEXITSTATUS(s) != 0)
                logmsg("watchdog: `param set` failed (status %d)", s);
            return;
        }
        sleep(WATCHDOG_POLL_INTERVAL_SEC);
        ++iter;
    }
    logmsg("watchdog: timed out after %ds; composer never registered",
           WATCHDOG_TIMEOUT_SEC);
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    logmsg("startup — pid %d, uid %d, euid %d",
           (int)getpid(), (int)getuid(), (int)geteuid());

    /* Pre-flight: binderfs must already be mounted (handled by
     * init.x23.cfg pre-init).  If not, fail fast — there's no clean way
     * to bring it up from inside a service. */
    if (access(BINDERFS_CONTROL, F_OK) < 0)
        die("%s missing — is binderfs mounted? "
            "(check init.x23.cfg pre-init)", BINDERFS_CONTROL);

    /* Halium content must be present.  Check the real Halium init at
     * /android/system/system/bin/init (NOT /android/system/bin/init,
     * which is a halium-internal symlink whose `/system/bin/` target
     * resolves to OHOS's init when read from outside the chroot).
     * Without halium, exec'ing init would just fail; failing here
     * gives a cleaner error. */
    if (access(ANDROID_ROOT "/system/bin/hwservicemanager", X_OK) < 0)
        die("%s/system/bin/hwservicemanager missing — did the chainload "
            "mount halium_system_a?", ANDROID_ROOT);

    /* Create the dedicated Android binder context.  hwbinder/vndbinder
     * already exist (mounted by init.x23.cfg pre-init); we only need
     * to add our `android-binder`. */
    if (create_binderfs_device("android-binder") < 0)
        die("provisioning android-binder failed");

    void *stack = mmap(NULL, CHILD_STACK_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                       -1, 0);
    if (stack == MAP_FAILED)
        die("mmap stack: %s", strerror(errno));

    /* No CLONE_NEWIPC — hwbinder must cross.
     * No CLONE_NEWNET — WiFi (Phase 10) + future RIL share OHOS net ns.
     * No CLONE_NEWUSER — root maps cleanly; userns would add uid-mapping
     *                   complexity without buying us anything. */
    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD;
    pid_t child = clone(child_main,
                        (char *)stack + CHILD_STACK_SIZE,
                        flags, NULL);
    if (child < 0)
        die("clone: %s", strerror(errno));

    logmsg("Halium NS launched as host PID %d", child);

    /* Watchdog runs synchronously in the parent.  After it returns
     * (success or timeout), we just waitpid the child — if Halium init
     * dies we exit and OHOS init's critical-restart policy takes over. */
    watchdog(child);

    int status;
    if (waitpid(child, &status, 0) < 0)
        die("waitpid(%d): %s", child, strerror(errno));
    logmsg("Halium NS exited with status 0x%x", status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
