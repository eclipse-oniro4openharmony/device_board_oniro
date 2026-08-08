/*
 * Copyright (C) 2026 Oniro / Hybris Generic.
 * Licensed under the Apache License, Version 2.0 (the "License").
 *
 * waydroidd — Waydroid Android-app container launcher (Phase W0+)
 *
 * Sibling of androidd: runs the stock Waydroid LXC image (LineageOS 20 /
 * Android 13) as a SECOND container next to the Halium HAL container,
 * with no LXC, no Python and no D-Bus.  The upstream host tool's LXC
 * config (waydroid/tools/helpers/lxc.py + data/configs/config_*) is the
 * checklist this launcher reimplements with raw clone(2) + mount(2).
 *
 * Differences from androidd:
 *  - Full namespace isolation: PID, mount, UTS, IPC, NET and cgroup are
 *    all new.  Nothing in this container talks hwbinder to OHOS VDIs;
 *    the only shared binder is /dev/host_hwbinder (the Halium host-HAL
 *    passthrough door, gated by a trimmed hosthals.xml).
 *  - Binder devices are the static `anbox-*` trio the kernel already
 *    provides (halium.config's literal "# Waydroid" section).
 *  - /data is a real ext4 loop image (app installs must persist);
 *    system.img / vendor.img are the sha256-pinned Waydroid OTA images.
 *  - The Halium /vendor (Android 14 MTK blobs) is rbind-mounted at
 *    /vendor_extra — the HALIUM_* vendor image's linker config pulls
 *    GPU/codec libs from there.
 *
 * The bring-up plan lives in
 *   device/board/oniro/docs/hybris_generic/android_app_compat_plan.md
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dirent.h>
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
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/loop.h>

#define WAYDROID_DIR   "/data/waydroid"
#define ROOTFS         WAYDROID_DIR "/rootfs"
#define SYSTEM_IMG     WAYDROID_DIR "/images/system.img"
#define VENDOR_IMG     WAYDROID_DIR "/images/vendor.img"
#define DATA_IMG       WAYDROID_DIR "/data.img"
#define PROP_FILE      WAYDROID_DIR "/waydroid.prop"
#define HOSTHALS_FILE  WAYDROID_DIR "/hosthals.xml"
#define PID_FILE       WAYDROID_DIR "/waydroidd.pid"

/* waydroid_compositor's XDG_RUNTIME_DIR; bound into the container at
 * /run/xdg (waydroid.xdg_runtime_dir / waydroid.wayland_display in the
 * prop file point the hwcomposer there). */
#define XDG_DIR        WAYDROID_DIR "/run/xdg"
#define WL_SOCKET      XDG_DIR "/wayland-0"
#define WL_SOCKET_WAIT_SEC 30

#define DATA_IMG_BYTES (6LL * 1024 * 1024 * 1024)

/* Halium world as mounted by the chainload (OHOS-namespace view). */
#define HALIUM_VENDOR  "/android/vendor"
#define HALIUM_ODM     "/android/odm"

#define ANBOX_BINDER    "/dev/binderfs/anbox-binder"
#define ANBOX_HWBINDER  "/dev/binderfs/anbox-hwbinder"
#define ANBOX_VNDBINDER "/dev/binderfs/anbox-vndbinder"
#define HOST_HWBINDER   "/dev/binderfs/hwbinder"

#define CHILD_STACK_SIZE (1 * 1024 * 1024)

static void logmsg(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    fprintf(stderr, "waydroidd: %s\n", buf);
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        char line[600];
        int m = snprintf(line, sizeof line, "waydroidd: %s\n", buf);
        if (m > 0) (void)!write(fd, line, (size_t)m);
        close(fd);
    }
}

static void die(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logmsg("FATAL: %s", buf);
    _exit(1);
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

static int touch_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

static int mknod_min(const char *path, mode_t mode, dev_t dev)
{
    if (mknod(path, mode, dev) < 0 && errno != EEXIST) return -1;
    return 0;
}

static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) { close(in); return -1; }
    char buf[8192];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) { rc = -1; break; }
            off += w;
        }
        if (rc < 0) break;
    }
    if (n < 0) rc = -1;
    close(in);
    close(out);
    return rc;
}

/* Bind a single host file into the container /dev; missing sources are
 * skipped (mirrors upstream's `optional` LXC mount flag). */
static void bind_node(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) < 0) return;
    /* Only create the mountpoint when it is missing — binding OVER an
     * existing file (e.g. a patched binary on the ro vendor image) must
     * not try to write to it. */
    if (stat(dst, &st) < 0 && touch_file(dst) < 0) {
        logmsg("touch %s: %s (skipped)", dst, strerror(errno));
        return;
    }
    if (mount(src, dst, NULL, MS_BIND, NULL) < 0)
        logmsg("bind %s -> %s: %s (skipped)", src, dst, strerror(errno));
}

static void bind_dir(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) < 0 || !S_ISDIR(st.st_mode)) return;
    if (mkdir_p(dst, 0755) < 0) {
        logmsg("mkdir %s: %s (skipped)", dst, strerror(errno));
        return;
    }
    if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL) < 0)
        logmsg("rbind %s -> %s: %s (skipped)", src, dst, strerror(errno));
}

/* ------------------------------------------------------------------------
 * Loop devices (androidd's scan-by-backing-file pattern: loop devices are
 * global, so a restart must reuse the previous attachment instead of
 * leaking a new one; androidd's NV loops and the waydroid images share
 * the same pool).
 */

static int loop_devno(int index, dev_t *out)
{
    char attr[PATH_MAX], val[64];
    snprintf(attr, sizeof attr, "/sys/block/loop%d/dev", index);
    int fd = open(attr, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, val, sizeof val - 1);
    close(fd);
    if (r <= 0) return -1;
    val[r] = '\0';
    unsigned maj = 0, min = 0;
    if (sscanf(val, "%u:%u", &maj, &min) != 2) return -1;
    *out = makedev(maj, min);
    return 0;
}

static int loop_attach(const char *img, int readonly, dev_t *devno)
{
    DIR *d = opendir("/sys/block");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, "loop", 4) != 0) continue;
            char attr[PATH_MAX], val[PATH_MAX];
            snprintf(attr, sizeof attr, "/sys/block/%s/loop/backing_file",
                     e->d_name);
            int fd = open(attr, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            ssize_t r = read(fd, val, sizeof val - 1);
            close(fd);
            if (r <= 0) continue;
            val[r] = '\0';
            char *nl = strchr(val, '\n');
            if (nl) *nl = '\0';
            if (strcmp(val, img) == 0) {
                int index = atoi(e->d_name + 4);
                closedir(d);
                return loop_devno(index, devno) == 0 ? index : -1;
            }
        }
        closedir(d);
    }

    int ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    if (ctl < 0) return -1;
    int index = ioctl(ctl, LOOP_CTL_GET_FREE);
    close(ctl);
    if (index < 0) return -1;
    if (loop_devno(index, devno) < 0) return -1;

    char devp[64];
    snprintf(devp, sizeof devp, "/dev/loop%d", index);
    int dfd = open(devp, O_RDWR | O_CLOEXEC);
    if (dfd < 0) {
        if (mknod_min(devp, S_IFBLK | 0600, *devno) < 0) return -1;
        dfd = open(devp, O_RDWR | O_CLOEXEC);
        if (dfd < 0) return -1;
    }

    int ffd = open(img, (readonly ? O_RDONLY : O_RDWR) | O_CLOEXEC);
    if (ffd < 0) { close(dfd); return -1; }
    int rc = ioctl(dfd, LOOP_SET_FD, ffd);
    if (rc == 0) {
        struct loop_info64 li;
        memset(&li, 0, sizeof li);
        snprintf((char *)li.lo_file_name, sizeof li.lo_file_name, "%s", img);
        if (readonly) li.lo_flags |= LO_FLAGS_READ_ONLY;
        (void)ioctl(dfd, LOOP_SET_STATUS64, &li);
    }
    close(ffd);
    close(dfd);
    return rc == 0 ? index : -1;
}

/* ------------------------------------------------------------------------
 * data.img: ext4 loop image for the container /data (TMPFS_XATTR is off
 * in the ansuz kernel, and Android app installs must persist anyway).
 */

static int run_mke2fs(const char *img)
{
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
        execl("/system/bin/mke2fs", "mke2fs", "-t", "ext4", "-m", "0",
              "-L", "waydroid_data", img, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    if (waitpid(p, &st, 0) < 0) return -1;
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

static int data_img_prepare(void)
{
    struct stat st;
    if (stat(DATA_IMG, &st) == 0 && st.st_size > 0) return 0;
    int fd = open(DATA_IMG, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int rc = ftruncate(fd, (off_t)DATA_IMG_BYTES);
    close(fd);
    if (rc < 0) { unlink(DATA_IMG); return -1; }
    if (run_mke2fs(DATA_IMG) < 0) { unlink(DATA_IMG); return -1; }
    logmsg("created %s (%lld MiB ext4)", DATA_IMG,
           DATA_IMG_BYTES / (1024 * 1024));
    return 0;
}

/* ------------------------------------------------------------------------
 * Child: container-NS setup + exec /init
 */

static dev_t g_sys_dev, g_ven_dev, g_dat_dev;
static int g_sys_loop = -1, g_ven_loop = -1, g_dat_loop = -1;

static void loop_dev_path(int index, char *out, size_t n)
{
    snprintf(out, n, "/dev/loop%d", index);
}

/* Bind whatever /dev/video* nodes exist (MTK 6.1 kernels expose the
 * codec/MDP engines as V4L2 devices; the legacy /dev/Vcodec node is gone). */
static void bind_video_nodes(void)
{
    DIR *d = opendir("/dev");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "video", 5) != 0) continue;
        char src[PATH_MAX], dst[PATH_MAX];
        snprintf(src, sizeof src, "/dev/%s", e->d_name);
        snprintf(dst, sizeof dst, ROOTFS "/dev/%s", e->d_name);
        bind_node(src, dst);
    }
    closedir(d);
}

static int child_main(void *arg)
{
    (void)arg;

    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0)
        logmsg("PR_SET_PDEATHSIG: %s (non-fatal)", strerror(errno));

    if (sethostname("waydroid", 8) < 0)
        logmsg("sethostname: %s (non-fatal)", strerror(errno));

    /* Keep every mount we make private to this namespace. */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        die("mount(/, rprivate): %s", strerror(errno));

    char dev[64];

    /* rootfs = system.img (ro), vendor.img on top (ro). */
    if (mkdir_p(ROOTFS, 0755) < 0)
        die("mkdir %s: %s", ROOTFS, strerror(errno));
    loop_dev_path(g_sys_loop, dev, sizeof dev);
    if (mount(dev, ROOTFS, "ext4", MS_RDONLY | MS_NOATIME, NULL) < 0)
        die("mount system.img: %s", strerror(errno));
    loop_dev_path(g_ven_loop, dev, sizeof dev);
    if (mount(dev, ROOTFS "/vendor", "ext4", MS_RDONLY | MS_NOATIME, NULL) < 0)
        die("mount vendor.img: %s", strerror(errno));

    /* Host GPU userspace into the container's vendor (upstream
     * mount_rootfs binds the host EGL dirs over the shim vendor). */
    bind_dir(HALIUM_VENDOR "/lib/egl",   ROOTFS "/vendor/lib/egl");
    bind_dir(HALIUM_VENDOR "/lib64/egl", ROOTFS "/vendor/lib64/egl");

    /* Halium mode: the whole host vendor at /vendor_extra, odm at
     * /odm_extra — the HALIUM_13 vendor image's linker config resolves
     * Mali/codec deps from there. */
    bind_dir(HALIUM_VENDOR, ROOTFS "/vendor_extra");
    bind_dir(HALIUM_ODM,    ROOTFS "/odm_extra");

    /* Graft dir at the container's (otherwise empty) /odm — first in the
     * HALIUM image's sphal search path.  Carries host libs the A13
     * container lacks but the A14 vendor blobs need (the R1 mismatch),
     * e.g. graphics.common-V5-ndk renamed as -V4 (AIDL NDK interface
     * libs keep their type symbols across versions). */
    bind_dir(WAYDROID_DIR "/graft", ROOTFS "/odm");

    /* Generated prop file over the shim vendor's placeholder. */
    if (mount(PROP_FILE, ROOTFS "/vendor/waydroid.prop",
              NULL, MS_BIND, NULL) < 0)
        logmsg("bind waydroid.prop: %s (container will use image defaults)",
               strerror(errno));

    /* Trimmed host-HAL passthrough list (plan D11). */
    {
        struct stat st;
        if (stat(HOSTHALS_FILE, &st) == 0 &&
            mount(HOSTHALS_FILE, ROOTFS "/system/etc/hosthals.xml",
                  NULL, MS_BIND, NULL) < 0)
            logmsg("bind hosthals.xml: %s (image copy stays)",
                   strerror(errno));
    }

    /* Binary-patched composer service (W3): stock main() does
     * configureRpcThreadpool(4) while the waydroid hwc module's binder
     * thread does configureRpcThreadpool(1) — if the module thread runs
     * after main has started the pool, libhidlbase aborts ("Binder
     * threadpool cannot be shrunk after starting").  Upstream wins that
     * race by luck; this container loses it every boot.  The patch flips
     * main's 4 to 1 (equal values can never shrink), one mov-immediate
     * at 0x7138 — see halium-blobs/waydroid/patches/README. */
    bind_node(WAYDROID_DIR "/patches/composer-service",
              ROOTFS "/vendor/bin/hw/android.hardware.graphics.composer@2.1-service");

    /* Binary-patched libui (W3): the container is A13 (lineage-20) but the
     * host Mali gralloc is A14 (MTK).  A13 libui's Gralloc4 client builds a
     * BufferDescriptorInfo whose usage carries phantom high bits
     * (0x7f00080000) that A13 libui's own validUsageBits mask rejects with
     * -EINVAL, so SurfaceFlinger cannot allocate any buffer and crashloops
     * in RenderEngine.  The patch makes validateBufferDescriptorInfo CLEAR
     * the invalid bits instead of rejecting — yielding the clean descriptor
     * the A14 mapper accepts (proven by libhybris/the W2 harness).  See
     * halium-blobs/waydroid/patches/README. */
    bind_node(WAYDROID_DIR "/patches/libui.so",
              ROOTFS "/system/lib64/libui.so");

    /* The AIDL graphics.allocator VINTF declaration (W3 gate 2) is
     * shipped in the graft dir at etc/vintf/manifest/ (graft is bound at
     * the container's /odm below, an ODM manifest source VINTF reads).
     * /vendor is read-only so a fragment can't be added there; /odm is
     * writable.  The host A14 Mali allocator is AIDL but the Waydroid
     * image only declares the HIDL gbm_mesa allocator, so without this
     * the container's servicemanager rejects the AIDL registration with
     * EX_ILLEGAL_ARGUMENT (-3).  See gralloc_bridge/README. */


    /* /data: the ext4 loop image, rw. */
    loop_dev_path(g_dat_loop, dev, sizeof dev);
    if (mount(dev, ROOTFS "/data", "ext4",
              MS_NOATIME | MS_NOSUID | MS_NODEV, NULL) < 0)
        die("mount data.img: %s", strerror(errno));

    /* Fresh /dev tmpfs + the upstream LXC node list. */
    if (mount("tmpfs", ROOTFS "/dev", "tmpfs", MS_NOSUID,
              "size=16M,mode=755") < 0)
        die("mount tmpfs on /dev: %s", strerror(errno));
    umask(0);
    /* Only the nodes the upstream LXC config provides.  The image's
     * container-patched first-stage init creates /dev/socket, /dev/kmsg,
     * /dev/random and /dev/urandom ITSELF and its CHECKCALL aborts with
     * "Init encountered errors starting first stage" on EEXIST — do not
     * pre-create those (first boot attempt died exactly there).  No
     * /dev/console either (lxc.console.path = none). */
    mknod_min(ROOTFS "/dev/null",    S_IFCHR | 0666, makedev(1, 3));
    mknod_min(ROOTFS "/dev/zero",    S_IFCHR | 0666, makedev(1, 5));
    mknod_min(ROOTFS "/dev/full",    S_IFCHR | 0666, makedev(1, 7));
    mknod_min(ROOTFS "/dev/tty",     S_IFCHR | 0666, makedev(5, 0));
    mknod_min(ROOTFS "/dev/ptmx",    S_IFCHR | 0666, makedev(5, 2));
    mkdir_p(ROOTFS "/dev/input", 0755);    /* hwc's wl_*_events FIFOs */

    bind_node("/dev/ashmem",   ROOTFS "/dev/ashmem");
    bind_node("/dev/fuse",     ROOTFS "/dev/fuse");
    bind_node("/dev/uhid",     ROOTFS "/dev/uhid");
    bind_node("/dev/mali0",    ROOTFS "/dev/mali0");
    bind_node("/dev/mdp_sync", ROOTFS "/dev/mdp_sync");
    bind_node("/dev/pmsg0",    ROOTFS "/dev/pmsg0");
    bind_node("/dev/net/tun",  ROOTFS "/dev/tun");
    bind_video_nodes();
    bind_dir("/dev/dma_heap", ROOTFS "/dev/dma_heap");
    bind_dir("/dev/dri",      ROOTFS "/dev/dri");
    bind_dir("/dev/char",     ROOTFS "/dev/char");

    /* Binder: the static anbox-* trio as the container's own worlds,
     * host hwbinder as the host-HAL passthrough door.  binderfs nodes
     * are 0600 root:root; servicemanager & co. run as system. */
    touch_file(ROOTFS "/dev/binder");
    touch_file(ROOTFS "/dev/vndbinder");
    touch_file(ROOTFS "/dev/hwbinder");
    touch_file(ROOTFS "/dev/host_hwbinder");
    if (mount(ANBOX_BINDER, ROOTFS "/dev/binder", NULL, MS_BIND, NULL) < 0)
        die("bind anbox-binder: %s", strerror(errno));
    if (mount(ANBOX_VNDBINDER, ROOTFS "/dev/vndbinder",
              NULL, MS_BIND, NULL) < 0)
        die("bind anbox-vndbinder: %s", strerror(errno));
    if (mount(ANBOX_HWBINDER, ROOTFS "/dev/hwbinder",
              NULL, MS_BIND, NULL) < 0)
        die("bind anbox-hwbinder: %s", strerror(errno));
    if (mount(HOST_HWBINDER, ROOTFS "/dev/host_hwbinder",
              NULL, MS_BIND, NULL) < 0)
        logmsg("bind host_hwbinder: %s (host-HAL passthrough off)",
               strerror(errno));
    chmod(ROOTFS "/dev/binder",        0666);
    chmod(ROOTFS "/dev/vndbinder",     0666);
    chmod(ROOTFS "/dev/hwbinder",      0666);
    chmod(ROOTFS "/dev/host_hwbinder", 0666);

    mkdir_p(ROOTFS "/dev/pts", 0755);
    if (mount("devpts", ROOTFS "/dev/pts", "devpts",
              MS_NOSUID | MS_NOEXEC, "mode=0620,gid=5,ptmxmode=0666") < 0)
        logmsg("mount devpts: %s (non-fatal)", strerror(errno));
    mkdir_p(ROOTFS "/dev/shm", 0755);
    if (mount("tmpfs", ROOTFS "/dev/shm", "tmpfs",
              MS_NOSUID | MS_NODEV, "mode=1777") < 0)
        logmsg("mount /dev/shm: %s (non-fatal)", strerror(errno));

    /* proc/sys fresh in this NS (lxc.mount.auto = proc sys). */
    if (mount("proc", ROOTFS "/proc", "proc",
              MS_NODEV | MS_NOEXEC | MS_NOSUID, NULL) < 0)
        die("mount proc: %s", strerror(errno));
    /* sysfs read-only (LXC parity: `sys:ro`) — the image's init.rc
     * writes cpufreq/scheduler knobs that must not reach the host
     * kernel; the shared sysfs would let it fight OHOS (e.g. the Mali
     * freq pin).  proc stays rw like upstream. */
    if (mount("sysfs", ROOTFS "/sys", "sysfs",
              MS_RDONLY | MS_NODEV | MS_NOEXEC | MS_NOSUID, NULL) < 0)
        die("mount sysfs: %s", strerror(errno));
    if (mount("debugfs", ROOTFS "/sys/kernel/debug", "debugfs", 0, NULL) < 0)
        logmsg("mount debugfs: %s (non-fatal)", strerror(errno));
    /* NO selinuxfs here, deliberately — unlike androidd's Halium
     * container.  LXC leaves /sys/fs/selinux unmounted, so libselinux
     * inside the Waydroid image reports SELinux disabled and init skips
     * policy loading.  The image DOES ship a precompiled_sepolicy;
     * mounting selinuxfs would invite init to load it into the shared
     * kernel, poisoning OHOS and the Halium container alike. */

    /* tmpfs at every rootfs mountpoint the ro image expects writable. */
    (void)mount("tmpfs", ROOTFS "/tmp", "tmpfs", MS_NODEV, NULL);
    (void)mount("tmpfs", ROOTFS "/var", "tmpfs", MS_NODEV, NULL);
    (void)mount("tmpfs", ROOTFS "/run", "tmpfs", MS_NODEV, NULL);
    (void)mount("tmpfs", ROOTFS "/mnt_extra", "tmpfs", MS_NODEV, NULL);
    (void)mount("tmpfs", ROOTFS "/cache", "tmpfs", MS_NODEV, NULL);

    /* Compositor socket dir at /run/xdg (W3).  The DIRECTORY is bound,
     * not the socket file: a bind of the dir shares the underlying
     * dentry tree, so a socket the compositor re-creates after a restart
     * shows up in the container, whereas a file bind would pin the old
     * dead inode. */
    mkdir_p(ROOTFS "/run/xdg", 0777);
    bind_dir(XDG_DIR, ROOTFS "/run/xdg");

    /* The host Mali allocator reads /vendor/etc/gralloc/gpu.xml and
     * aborts ("Unable to retrieve GPU capabilities") if it is missing.
     * The shim /vendor has no such file and is read-only, so overlay
     * /vendor/etc with just that one file added from the host vendor
     * (/vendor_extra).  overlayfs resolves lowerdir before the mount
     * shadows it, so an in-place lower==mountpoint overlay is valid.
     * Done here (not with the other vendor binds) because the upper/work
     * dirs need /data mounted rw, which happens above.  Still before
     * exec /init, so the container's init sees the merged /vendor/etc. */
    {
        struct stat st;
        if (stat(ROOTFS "/vendor_extra/etc/gralloc/gpu.xml", &st) == 0) {
            mkdir_p(ROOTFS "/data/.gralloc_ovl/up/gralloc", 0755);
            mkdir_p(ROOTFS "/data/.gralloc_ovl/wk", 0755);
            if (copy_file(ROOTFS "/vendor_extra/etc/gralloc/gpu.xml",
                          ROOTFS "/data/.gralloc_ovl/up/gralloc/gpu.xml") < 0)
                logmsg("copy gpu.xml: %s", strerror(errno));
            else if (mount("overlay", ROOTFS "/vendor/etc", "overlay", 0,
                           "lowerdir=" ROOTFS "/vendor/etc,upperdir="
                           ROOTFS "/data/.gralloc_ovl/up,workdir="
                           ROOTFS "/data/.gralloc_ovl/wk") < 0)
                logmsg("overlay /vendor/etc for gpu.xml: %s (allocator will abort)",
                       strerror(errno));
            else
                logmsg("overlaid /vendor/etc/gralloc/gpu.xml from host");
        }
    }

    /* Container env: mimic lxc-start (minimal env + container=lxc). */
    clearenv();
    setenv("container", "lxc", 1);
    setenv("PATH", "/system/bin:/system/xbin:/vendor/bin", 1);
    setenv("ANDROID_ROOT", "/system", 1);
    setenv("ANDROID_DATA", "/data", 1);

    /* Root swap, androidd-style (see androidd.c for the full rationale:
     * setns(2) re-entry — which Android's own APEX mount-namespace dance
     * uses — resolves the caller's root at the namespace's REAL root
     * mount, so the new root must be MS_MOVEd onto it, and we must first
     * escape the chainload's /root chroot to reach it). */
    if (chdir(ROOTFS) < 0)
        die("chdir %s: %s", ROOTFS, strerror(errno));
    if (chroot(ROOTFS "/data") < 0)
        die("chroot escape pivot: %s", strerror(errno));
    for (int i = 0; i < 64; ++i) {
        struct stat cur, up;
        if (stat(".", &cur) < 0 || stat("..", &up) < 0)
            die("escape stat: %s", strerror(errno));
        if (cur.st_ino == up.st_ino && cur.st_dev == up.st_dev)
            break;
        if (chdir("..") < 0)
            die("escape chdir ..: %s", strerror(errno));
    }
    if (chroot(".") < 0)
        die("chroot ns root: %s", strerror(errno));
    if (chdir("/root" ROOTFS) < 0 && chdir(ROOTFS) < 0)
        die("chdir to rootfs post-escape: %s", strerror(errno));
    if (mount(".", "/", NULL, MS_MOVE, NULL) < 0)
        die("MS_MOVE rootfs -> /: %s", strerror(errno));
    if (chroot(".") < 0)
        die("chroot: %s", strerror(errno));
    if (chdir("/") < 0)
        die("chdir /: %s", strerror(errno));

    /* Early init logs to stderr before it opens /dev/kmsg itself. */
    int kfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (kfd >= 0) {
        (void)dup2(kfd, 1);
        (void)dup2(kfd, 2);
        if (kfd != 1 && kfd != 2) close(kfd);
    }

    /* Close every inherited fd > 2.  When launched from a debug shell,
     * fds connected to OHOS daemons leak through exec into zygote, whose
     * fork-time fd sanitizer aborts system_server on any unix socket
     * with a non-allowlisted name ("Socket name not allowlisted:
     * /dev/unix/socket/hdcd").  lxc-start's clean fd table is the
     * upstream equivalent.  init reopens its own stdio to /dev/null. */
    {
        DIR *fdd = opendir("/proc/self/fd");
        if (fdd) {
            int keep = dirfd(fdd);
            struct dirent *fe;
            while ((fe = readdir(fdd)) != NULL) {
                int fd = atoi(fe->d_name);
                if (fd > 2 && fd != keep)
                    close(fd);
            }
            closedir(fdd);
        }
    }

    execl("/init", "/init", (char *)NULL);
    die("exec /init: %s", strerror(errno));
    return 1;
}

/* ------------------------------------------------------------------------
 * Parent
 */

static pid_t g_child = -1;

static void on_term(int sig)
{
    (void)sig;
    if (g_child > 0) kill(g_child, SIGKILL);
}

/* keychord-wedge guard (W3).  lineage init's keychord scanner
 * (system/core/init/keychords.cpp) inotify-watches /dev/input and does a
 * BLOCKING O_RDONLY open of every new node — including the FIFOs the
 * waydroid hwc creates there for input forwarding.  A FIFO with no
 * writer blocks that open forever, freezing container init (it is the
 * zombie reaper AND the ctl.* queue; the queue then overflows and
 * services stop restarting).  Holding an O_RDWR fd on each fifo from
 * the host side means a writer always exists, so init's open returns at
 * once (it closes the fd right after its evdev ioctls fail — it never
 * steals input events).  Re-checked periodically because do_hotplug in
 * the hwc remove()s + re-mkfifo()s the touch fifo on display resize. */
static int g_fifo_fd[2] = { -1, -1 };

static void hold_fifo_writers(pid_t child)
{
    static const char *names[2] = { "wl_touch_events", "wl_keyboard_events" };
    for (int i = 0; i < 2; ++i) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "/proc/%ld/root/dev/input/%s",
                 (long)child, names[i]);
        struct stat st;
        if (stat(path, &st) < 0 || !S_ISFIFO(st.st_mode)) continue;
        if (g_fifo_fd[i] >= 0) {
            struct stat fst;
            if (fstat(g_fifo_fd[i], &fst) == 0 && fst.st_ino == st.st_ino)
                continue;   /* same fifo, writer already held */
            close(g_fifo_fd[i]);
            g_fifo_fd[i] = -1;
        }
        g_fifo_fd[i] = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if (g_fifo_fd[i] >= 0)
            logmsg("holding writer fd on %s (keychord-wedge guard)",
                   names[i]);
    }
}

/* Start-order handshake (plan §5 W3): the compositor owns the socket, so
 * the socket itself is the readiness signal — more direct than a param.
 * A timeout is not fatal: the container's hwcomposer service crashes on
 * connect failure and init restarts it, so a late compositor is absorbed
 * (just noisily). */
static void wait_for_compositor(void)
{
    struct stat st;
    for (int i = 0; i < WL_SOCKET_WAIT_SEC * 2; ++i) {
        if (stat(WL_SOCKET, &st) == 0 && S_ISSOCK(st.st_mode)) {
            if (i > 0)
                logmsg("compositor socket appeared after %d ms", i * 500);
            return;
        }
        if (i == 0)
            logmsg("waiting up to %ds for %s ...", WL_SOCKET_WAIT_SEC,
                   WL_SOCKET);
        usleep(500 * 1000);
    }
    logmsg("WARNING: no compositor socket at %s — container boots headless "
           "until one appears", WL_SOCKET);
}

static pid_t read_pidfile(void)
{
    FILE *f = fopen(PID_FILE, "r");
    if (!f) return -1;
    long pid = -1;
    if (fscanf(f, "%ld", &pid) != 1) pid = -1;
    fclose(f);
    return (pid_t)pid;
}

static int cmd_stop(void)
{
    pid_t pid = read_pidfile();
    if (pid <= 0) {
        logmsg("no pidfile — not running?");
        return 1;
    }
    if (kill(pid, SIGTERM) < 0) {
        logmsg("kill %ld: %s", (long)pid, strerror(errno));
        return 1;
    }
    logmsg("sent SIGTERM to waydroidd (%ld)", (long)pid);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "stop") == 0)
        return cmd_stop();

    if (geteuid() != 0)
        die("must run as root");

    pid_t old = read_pidfile();
    if (old > 0 && kill(old, 0) == 0)
        die("already running (pid %ld) — `waydroidd stop` first", (long)old);

    struct stat st;
    if (stat(SYSTEM_IMG, &st) < 0)
        die("%s missing", SYSTEM_IMG);
    if (stat(VENDOR_IMG, &st) < 0)
        die("%s missing", VENDOR_IMG);
    if (stat(PROP_FILE, &st) < 0)
        die("%s missing", PROP_FILE);

    if (data_img_prepare() < 0)
        die("data.img prepare failed: %s", strerror(errno));

    /* Bind target for the compositor socket dir: make sure it exists in
     * the host namespace even if the compositor has never run, so the
     * child's bind always succeeds and a compositor started later is
     * still seen through it. */
    if (mkdir_p(XDG_DIR, 0755) < 0)
        logmsg("mkdir %s: %s (wayland bind will be skipped)", XDG_DIR,
               strerror(errno));
    wait_for_compositor();

    g_sys_loop = loop_attach(SYSTEM_IMG, 1, &g_sys_dev);
    if (g_sys_loop < 0) die("loop attach system.img: %s", strerror(errno));
    g_ven_loop = loop_attach(VENDOR_IMG, 1, &g_ven_dev);
    if (g_ven_loop < 0) die("loop attach vendor.img: %s", strerror(errno));
    g_dat_loop = loop_attach(DATA_IMG, 0, &g_dat_dev);
    if (g_dat_loop < 0) die("loop attach data.img: %s", strerror(errno));
    logmsg("loops: system=%d vendor=%d data=%d",
           g_sys_loop, g_ven_loop, g_dat_loop);

    static char stack[CHILD_STACK_SIZE] __attribute__((aligned(16)));
    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
                CLONE_NEWNET | CLONE_NEWCGROUP | SIGCHLD;
    g_child = clone(child_main, stack + sizeof stack, flags, NULL);
    if (g_child < 0)
        die("clone: %s", strerror(errno));
    logmsg("container init cloned: pid %ld", (long)g_child);

    FILE *f = fopen(PID_FILE, "w");
    if (f) { fprintf(f, "%ld\n", (long)getpid()); fclose(f); }

    signal(SIGTERM, on_term);
    signal(SIGINT,  on_term);

    int wst = 0;
    for (;;) {
        pid_t r = waitpid(g_child, &wst, WNOHANG);
        if (r == g_child)
            break;
        if (r < 0 && errno != EINTR)
            break;
        hold_fifo_writers(g_child);
        sleep(2);
    }
    if (WIFEXITED(wst))
        logmsg("container init exited with status %d", WEXITSTATUS(wst));
    else if (WIFSIGNALED(wst))
        logmsg("container init killed by signal %d", WTERMSIG(wst));
    unlink(PID_FILE);
    return 0;
}
