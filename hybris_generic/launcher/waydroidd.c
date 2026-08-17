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

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/loop.h>
/* The OHOS musl sysroot's <linux/socket.h> redefines sockaddr_storage
 * against <sys/socket.h>; rename the kernel copy out of the way. */
#define sockaddr_storage kernel_sockaddr_storage__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/veth.h>
#undef sockaddr_storage

#define WAYDROID_DIR   "/data/waydroid"
#define ROOTFS         WAYDROID_DIR "/rootfs"
#define SYSTEM_IMG     WAYDROID_DIR "/images/system.img"
#define VENDOR_IMG     WAYDROID_DIR "/images/vendor.img"
#define DATA_IMG       WAYDROID_DIR "/data.img"
#define PROP_FILE      WAYDROID_DIR "/waydroid.prop"
#define HOSTHALS_FILE  WAYDROID_DIR "/hosthals.xml"
#define PID_FILE       WAYDROID_DIR "/waydroidd.pid"
#define CONTAINER_PID_FILE WAYDROID_DIR "/container.pid"

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

/* Networking: a veth pair (wdb0 host <-> eth0 container) into the
 * container netns.
 *
 * W4 (networking-lite) assigned static addresses and re-published the
 * /24 subnet route into netd's local_network table (Android policy
 * routing never consults the main table; rule 32000 is `from all
 * unreachable`).  That was enough to reach the container's adbd but NOT
 * the internet: app traffic is bound to a netd "network" (fwmark/netId),
 * and no such network exists until IpClient provisions eth0 and registers
 * a NetworkAgent with ConnectivityService.
 *
 * W6 (real networking, plan §W6) makes that happen the *Android* way:
 *   - the host enables ip_forward and MASQUERADEs 192.168.240.0/24 out
 *     the active uplink (wlan0, or a future cellular rmnet — the rule is
 *     uplink-agnostic so it survives WiFi switching);
 *   - a minimal single-lease DHCP server answers on wdb0, so the image's
 *     ethernet stack (networkstack IpClient/DhcpClient, which flushes the
 *     static IP at boot expecting a lease) provisions eth0 with a default
 *     route + DNS that netd installs into the per-network policy tables
 *     app traffic actually uses.
 * The graft's waydroid-net.rc is now only a *fallback* (re-apply the W4
 * static config if DHCP produced no default route, so adb still works).
 * tcprelay bridges device-loopback (the only thing `hdc fport` can reach)
 * to the container's adbd. */
#define VETH_HOST_IF   "wdb0"
#define VETH_CONT_IF   "eth0"
#define VETH_HOST_IP   "192.168.240.1"
#define VETH_CONT_IP   "192.168.240.2"
#define VETH_NETMASK   "255.255.255.0"
#define VETH_SUBNET    "192.168.240.0/24"
#define ADB_RELAY_BIN  WAYDROID_DIR "/bin/tcprelay"
#define ADB_RELAY_PORT "15555"
#define ADB_CONT_PORT  "5555"

/* W6 real networking. */
#define IPTABLES_BIN     "/system/bin/iptables"
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC       0x63825363u
#define DHCP_LEASE_SECS  86400u

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

/* --- veth + adb relay (W4) ------------------------------------------- */

static void nla_put(struct nlmsghdr *nh, unsigned short type,
                    const void *data, int len)
{
    struct rtattr *rta =
        (struct rtattr *)((char *)nh + NLMSG_ALIGN(nh->nlmsg_len));
    rta->rta_type = type;
    rta->rta_len = (unsigned short)RTA_LENGTH(len);
    if (len)
        memcpy(RTA_DATA(rta), data, (size_t)len);
    nh->nlmsg_len = NLMSG_ALIGN(nh->nlmsg_len) + RTA_ALIGN(rta->rta_len);
}

static struct rtattr *nla_nest_start(struct nlmsghdr *nh, unsigned short type)
{
    struct rtattr *rta =
        (struct rtattr *)((char *)nh + NLMSG_ALIGN(nh->nlmsg_len));
    rta->rta_type = type;
    rta->rta_len = (unsigned short)RTA_LENGTH(0);
    nh->nlmsg_len = NLMSG_ALIGN(nh->nlmsg_len) + RTA_LENGTH(0);
    return rta;
}

static void nla_nest_end(struct nlmsghdr *nh, struct rtattr *nest)
{
    nest->rta_len = (unsigned short)((char *)nh + nh->nlmsg_len -
                                     (char *)nest);
}

/* Delete a stale host-side veth (RTM_DELLINK wdb0), ignoring errors.  A
 * supervisor `kill -9` of the previous waydroidd can leave wdb0 lingering
 * briefly after its container netns died; without this, veth_create below
 * hits EEXIST, treats it as success, and the NEW container is left with no
 * eth0 (no static IP, no DHCP, no adb).  Deleting first guarantees a fresh
 * pair every generation.  Deleting wdb0 also removes its eth0 peer, which
 * is only ever in a dead generation's netns here, so it is safe. */
static void veth_delete(void)
{
    char buf[256];
    memset(buf, 0, sizeof buf);
    struct nlmsghdr *nh = (struct nlmsghdr *)buf;
    nh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    nh->nlmsg_type = RTM_DELLINK;
    nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nh->nlmsg_seq = 1;
    struct ifinfomsg *ifi = NLMSG_DATA(nh);
    ifi->ifi_family = AF_UNSPEC;
    nla_put(nh, IFLA_IFNAME, VETH_HOST_IF, (int)strlen(VETH_HOST_IF) + 1);

    int s = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (s < 0)
        return;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    if (sendto(s, nh, nh->nlmsg_len, 0, (struct sockaddr *)&sa,
               sizeof sa) >= 0) {
        char rbuf[256];
        (void)!recv(s, rbuf, sizeof rbuf, 0);   /* drain the ACK */
    }
    close(s);
}

/* Create wdb0 (this netns) <-> eth0 (container_pid's netns).  The
 * kernel checklist equivalent of
 *   ip link add wdb0 type veth peer name eth0 netns <pid>
 */
static int veth_create(pid_t container_pid)
{
    veth_delete();   /* clear any stale wdb0 from a prior generation */

    char buf[1024];
    memset(buf, 0, sizeof buf);
    struct nlmsghdr *nh = (struct nlmsghdr *)buf;
    nh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    nh->nlmsg_type = RTM_NEWLINK;
    nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    nh->nlmsg_seq = 1;

    nla_put(nh, IFLA_IFNAME, VETH_HOST_IF, (int)strlen(VETH_HOST_IF) + 1);
    struct rtattr *linkinfo = nla_nest_start(nh, IFLA_LINKINFO);
    nla_put(nh, IFLA_INFO_KIND, "veth", 5);
    struct rtattr *infodata = nla_nest_start(nh, IFLA_INFO_DATA);
    struct rtattr *peer = nla_nest_start(nh, VETH_INFO_PEER);
    /* VETH_INFO_PEER payload = struct ifinfomsg + nested attrs */
    memset((char *)nh + nh->nlmsg_len, 0, sizeof(struct ifinfomsg));
    nh->nlmsg_len += (unsigned int)sizeof(struct ifinfomsg);
    nla_put(nh, IFLA_IFNAME, VETH_CONT_IF, (int)strlen(VETH_CONT_IF) + 1);
    unsigned int ns_pid = (unsigned int)container_pid;
    nla_put(nh, IFLA_NET_NS_PID, &ns_pid, sizeof ns_pid);
    nla_nest_end(nh, peer);
    nla_nest_end(nh, infodata);
    nla_nest_end(nh, linkinfo);

    int s = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (s < 0)
        return -1;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    int rc = -1;
    if (sendto(s, nh, nh->nlmsg_len, 0, (struct sockaddr *)&sa,
               sizeof sa) >= 0) {
        char rbuf[1024];
        ssize_t n = recv(s, rbuf, sizeof rbuf, 0);
        if (n >= (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
            struct nlmsghdr *rnh = (struct nlmsghdr *)rbuf;
            if (rnh->nlmsg_type == NLMSG_ERROR) {
                int err = ((struct nlmsgerr *)NLMSG_DATA(rnh))->error;
                if (err == 0 || err == -EEXIST) {
                    rc = 0;
                } else {
                    errno = -err;
                }
            }
        }
    }
    int e = errno;
    close(s);
    errno = e;
    return rc;
}

/* ifconfig <ifname> <ip> netmask <mask> up, in the CURRENT netns. */
static int if_config(const char *ifname, const char *ip, const char *mask)
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &sin->sin_addr) != 1 ||
        ioctl(s, SIOCSIFADDR, &ifr) < 0)
        goto err;
    if (inet_pton(AF_INET, mask, &sin->sin_addr) != 1 ||
        ioctl(s, SIOCSIFNETMASK, &ifr) < 0)
        goto err;
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0)
        goto err;
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0)
        goto err;
    close(s);
    return 0;
err:
    {
        int e = errno;
        close(s);
        errno = e;
        return -1;
    }
}

/* Configure the container end from outside: fork, setns(net), ioctl. */
static int netns_if_config(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%ld/ns/net", (long)pid);
    int nsfd = open(path, O_RDONLY | O_CLOEXEC);
    if (nsfd < 0)
        return -1;
    pid_t p = fork();
    if (p == 0) {
        if (setns(nsfd, CLONE_NEWNET) < 0)
            _exit(1);
        if (if_config(VETH_CONT_IF, VETH_CONT_IP, VETH_NETMASK) < 0)
            _exit(2);
        _exit(0);
    }
    close(nsfd);
    if (p < 0)
        return -1;
    int st;
    if (waitpid(p, &st, 0) < 0)
        return -1;
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

static void spawn_adb_relay(void)
{
    struct stat st;
    if (stat(ADB_RELAY_BIN, &st) < 0) {
        logmsg("no %s — adb relay disabled", ADB_RELAY_BIN);
        return;
    }
    pid_t p = fork();
    if (p == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2)
                close(devnull);
        }
        execl(ADB_RELAY_BIN, "tcprelay", ADB_RELAY_PORT, VETH_CONT_IP,
              ADB_CONT_PORT, (char *)NULL);
        _exit(127);
    }
    if (p > 0)
        logmsg("adb relay: 127.0.0.1:%s -> %s:%s (pid %ld)",
               ADB_RELAY_PORT, VETH_CONT_IP, ADB_CONT_PORT, (long)p);
}

/* --- W6 real networking: NAT + DHCP (host side) ---------------------- */

static void enable_ip_forward(void)
{
    int fd = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        logmsg("ip_forward open: %s", strerror(errno));
        return;
    }
    if (write(fd, "1\n", 2) < 0)
        logmsg("ip_forward write: %s", strerror(errno));
    close(fd);
}

/* Spawn /system/bin/iptables with argv (argv[0] = "iptables"); returns the
 * child's exit status, or -1 on spawn/wait failure.  Output is muted so a
 * failing -C (rule absent) does not spam the log. */
static int iptables_run(char *const argv[])
{
    pid_t p = fork();
    if (p == 0) {
        int n = open("/dev/null", O_WRONLY);
        if (n >= 0) { dup2(n, 1); dup2(n, 2); if (n > 2) close(n); }
        execv(IPTABLES_BIN, argv);
        _exit(127);
    }
    if (p < 0)
        return -1;
    int st;
    if (waitpid(p, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* The container-uplink NAT ruleset.  `spec` is the argv tail after the
 * chain; FORWARD accepts are belt-and-suspenders (the default policy is
 * ACCEPT today, but an OHOS firewall could flip it later).  The rule is
 * uplink-agnostic (no `-o wlan0`) so MASQUERADE follows WiFi reconnects
 * and a future cellular default route without re-plumbing. */
struct ipt_rule {
    const char *table;
    const char *chain;
    const char *spec[10];
};

static const struct ipt_rule g_nat_rules[] = {
    { "nat", "POSTROUTING",
      { "-s", VETH_SUBNET, "!", "-d", VETH_SUBNET, "-j", "MASQUERADE", NULL } },
    { "filter", "FORWARD", { "-i", VETH_HOST_IF, "-j", "ACCEPT", NULL } },
    { "filter", "FORWARD", { "-o", VETH_HOST_IF, "-j", "ACCEPT", NULL } },
};

static int ipt_op(const struct ipt_rule *r, const char *op)
{
    char *argv[18];
    int i = 0;
    argv[i++] = "iptables";
    argv[i++] = "-t";
    argv[i++] = (char *)r->table;
    argv[i++] = (char *)op;
    argv[i++] = (char *)r->chain;
    for (int j = 0; r->spec[j] && i < 17; ++j)
        argv[i++] = (char *)r->spec[j];
    argv[i] = NULL;
    return iptables_run(argv);
}

static void nl_del_main_default(void);   /* fwd: defined with uplink router */

static void nat_setup(void)
{
    enable_ip_forward();
    for (size_t k = 0; k < sizeof g_nat_rules / sizeof g_nat_rules[0]; ++k) {
        if (ipt_op(&g_nat_rules[k], "-C") != 0)   /* absent -> append */
            ipt_op(&g_nat_rules[k], "-A");
    }
    logmsg("NAT up: ip_forward=1, MASQUERADE %s -> active uplink", VETH_SUBNET);
}

static void nat_teardown(void)
{
    for (size_t k = 0; k < sizeof g_nat_rules / sizeof g_nat_rules[0]; ++k)
        while (ipt_op(&g_nat_rules[k], "-D") == 0)  /* drop any dups */
            ;
    nl_del_main_default();
}

/* --- W6 uplink router: mirror OHOS's default route into table `main` ---
 *
 * OHOS (like Android) keeps its uplink default route in a per-network
 * policy table, and forwarded container packets are routed out of `main`,
 * which has no default — so they are dropped before ever reaching the
 * MASQUERADE rule.  Discover the active uplink default (gateway + oif) with
 * a netlink route dump and install a copy in `main`.  A background poll
 * refreshes it so WiFi reconnects / late WiFi bring-up / network switches
 * keep working (the plan's §W6 coexistence requirement). */

/* Find the active uplink default route (RTN_UNICAST, /0, has a gateway) in
 * any table other than `main` — i.e. OHOS's own, not the copy we install.
 * Picks the lowest-metric candidate.  Returns 0 and fills gw/oif on hit. */
static int nl_find_uplink_default(uint32_t *gw_out, int *oif_out)
{
    int s = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (s < 0)
        return -1;
    struct { struct nlmsghdr nh; struct rtgenmsg gen; } req;
    memset(&req, 0, sizeof req);
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg));
    req.nh.nlmsg_type = RTM_GETROUTE;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nh.nlmsg_seq = 1;
    req.gen.rtgen_family = AF_INET;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    if (sendto(s, &req, req.nh.nlmsg_len, 0, (struct sockaddr *)&sa,
               sizeof sa) < 0) {
        close(s);
        return -1;
    }

    char buf[16384];
    uint32_t best_gw = 0, best_metric = 0xffffffffu;
    int best_oif = 0, found = -1, done = 0;
    while (!done) {
        ssize_t n = recv(s, buf, sizeof buf, 0);
        if (n <= 0)
            break;
        for (struct nlmsghdr *nh = (struct nlmsghdr *)buf;
             NLMSG_OK(nh, n); nh = NLMSG_NEXT(nh, n)) {
            if (nh->nlmsg_type == NLMSG_DONE || nh->nlmsg_type == NLMSG_ERROR) {
                done = 1;
                break;
            }
            if (nh->nlmsg_type != RTM_NEWROUTE)
                continue;
            struct rtmsg *rtm = NLMSG_DATA(nh);
            if (rtm->rtm_family != AF_INET || rtm->rtm_dst_len != 0 ||
                rtm->rtm_type != RTN_UNICAST)
                continue;
            uint32_t gw = 0, table = rtm->rtm_table, metric = 0;
            int oif = 0, rtl = (int)RTM_PAYLOAD(nh);
            for (struct rtattr *rta = RTM_RTA(rtm); RTA_OK(rta, rtl);
                 rta = RTA_NEXT(rta, rtl)) {
                switch (rta->rta_type) {
                case RTA_GATEWAY:  memcpy(&gw, RTA_DATA(rta), 4); break;
                case RTA_OIF:      memcpy(&oif, RTA_DATA(rta), 4); break;
                case RTA_TABLE:    memcpy(&table, RTA_DATA(rta), 4); break;
                case RTA_PRIORITY: memcpy(&metric, RTA_DATA(rta), 4); break;
                }
            }
            if (!gw || table == RT_TABLE_MAIN)
                continue;                        /* need a gw; skip our copy */
            if (metric < best_metric) {
                best_metric = metric;
                best_gw = gw;
                best_oif = oif;
                found = 0;
            }
        }
    }
    close(s);
    if (found == 0) {
        *gw_out = best_gw;
        *oif_out = best_oif;
    }
    return found;
}

/* RTM_NEWROUTE/RTM_DELROUTE for `default via gw dev oif` in table main. */
static int nl_main_default(int type, unsigned flags, uint32_t gw, int oif)
{
    char buf[256];
    memset(buf, 0, sizeof buf);
    struct nlmsghdr *nh = (struct nlmsghdr *)buf;
    nh->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    nh->nlmsg_type = (unsigned short)type;
    nh->nlmsg_flags = (unsigned short)(NLM_F_REQUEST | NLM_F_ACK | flags);
    nh->nlmsg_seq = 2;
    struct rtmsg *rt = NLMSG_DATA(nh);
    rt->rtm_family = AF_INET;
    rt->rtm_dst_len = 0;
    rt->rtm_table = RT_TABLE_MAIN;
    rt->rtm_protocol = RTPROT_STATIC;
    rt->rtm_scope = RT_SCOPE_UNIVERSE;
    rt->rtm_type = RTN_UNICAST;
    nla_put(nh, RTA_GATEWAY, &gw, 4);
    if (oif > 0)
        nla_put(nh, RTA_OIF, &oif, 4);

    int s = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (s < 0)
        return -1;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    int rc = -1;
    if (sendto(s, nh, nh->nlmsg_len, 0, (struct sockaddr *)&sa,
               sizeof sa) >= 0) {
        char rbuf[256];
        ssize_t n = recv(s, rbuf, sizeof rbuf, 0);
        if (n >= (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
            struct nlmsghdr *rnh = (struct nlmsghdr *)rbuf;
            if (rnh->nlmsg_type == NLMSG_ERROR) {
                int err = ((struct nlmsgerr *)NLMSG_DATA(rnh))->error;
                if (err == 0 || err == -EEXIST || err == -ESRCH)
                    rc = 0;
                else
                    errno = -err;
            }
        }
    }
    int e = errno;
    close(s);
    errno = e;
    return rc;
}

static void nl_del_main_default(void)
{
    uint32_t gw;
    int oif;
    if (nl_find_uplink_default(&gw, &oif) == 0)
        nl_main_default(RTM_DELROUTE, 0, gw, oif);
}

static void uplink_router_loop(void)
{
    uint32_t last_gw = 0;
    int last_oif = -1;
    for (;;) {
        uint32_t gw;
        int oif;
        /* Re-install every tick (idempotent REPLACE) so the copy survives
         * any host-side route churn; only log when it actually changes. */
        if (nl_find_uplink_default(&gw, &oif) == 0 &&
            nl_main_default(RTM_NEWROUTE, NLM_F_CREATE | NLM_F_REPLACE,
                            gw, oif) == 0 &&
            (gw != last_gw || oif != last_oif)) {
            struct in_addr a = { .s_addr = gw };
            logmsg("uplink default -> main: via %s oif %d", inet_ntoa(a), oif);
            last_gw = gw;
            last_oif = oif;
        }
        sleep(15);   /* follow WiFi reconnects / late bring-up / switches */
    }
}

static void spawn_uplink_router(void)
{
    pid_t p = fork();
    if (p == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        uplink_router_loop();
        _exit(0);
    }
    if (p > 0)
        logmsg("uplink router forked (pid %ld)", (long)p);
    else
        logmsg("uplink router fork: %s", strerror(errno));
}

/* Host default gateway (network-order __be32) from /proc/net/route, or 0.
 * Handed to the container as a DNS resolver — most home routers resolve,
 * and it is the same server the OHOS side uses on this LAN. */
static uint32_t host_default_gw(void)
{
    FILE *f = fopen("/proc/net/route", "r");
    if (!f)
        return 0;
    char line[256];
    uint32_t gw = 0;
    if (fgets(line, sizeof line, f)) {          /* skip header */
        char iface[32];
        unsigned long dest, gwv, flags;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "%31s %lx %lx %lx", iface, &dest, &gwv, &flags)
                    == 4 && dest == 0 && (flags & 0x2) && gwv != 0) {
                gw = (uint32_t)gwv;   /* /proc prints __be32 host-order hex,
                                       * i.e. the wire bytes on this LE host */
                break;
            }
        }
    }
    fclose(f);
    return gw;
}

/* BOOTP/DHCP wire frame (RFC 2131). */
struct dhcp_msg {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint8_t  options[312];
} __attribute__((packed));

static uint8_t *dhcp_put(uint8_t *o, uint8_t code, uint8_t len, const void *d)
{
    *o++ = code;
    *o++ = len;
    if (len) memcpy(o, d, len);
    return o + len;
}

/* Find DHCP option `want` in a received message; returns its first byte or
 * -1.  Only fixed-header options are parsed (no overload). */
static int dhcp_opt_u8(const struct dhcp_msg *m, size_t optlen, uint8_t want)
{
    const uint8_t *o = m->options;
    size_t i = 4;                               /* past magic cookie */
    while (i < optlen && o[i] != 255) {
        if (o[i] == 0) { i++; continue; }       /* pad */
        if (i + 1 >= optlen) break;
        uint8_t code = o[i], len = o[i + 1];
        if (i + 2 + len > optlen) break;
        if (code == want && len >= 1)
            return o[i + 2];
        i += 2 + len;
    }
    return -1;
}

static size_t dhcp_build_reply(const struct dhcp_msg *req, struct dhcp_msg *rep,
                               uint8_t msgtype)
{
    memset(rep, 0, sizeof *rep);
    rep->op    = 2;                             /* BOOTREPLY */
    rep->htype = req->htype ? req->htype : 1;
    rep->hlen  = req->hlen ? req->hlen : 6;
    rep->xid   = req->xid;
    rep->flags = req->flags;
    rep->yiaddr = inet_addr(VETH_CONT_IP);
    rep->siaddr = inet_addr(VETH_HOST_IP);
    rep->giaddr = req->giaddr;
    memcpy(rep->chaddr, req->chaddr, 16);

    uint8_t *o = rep->options;
    uint32_t v = htonl(DHCP_MAGIC);
    memcpy(o, &v, 4); o += 4;

    o = dhcp_put(o, 53, 1, &msgtype);
    v = inet_addr(VETH_HOST_IP);   o = dhcp_put(o, 54, 4, &v);   /* server id */
    v = htonl(DHCP_LEASE_SECS);    o = dhcp_put(o, 51, 4, &v);   /* lease */
    v = inet_addr(VETH_NETMASK);   o = dhcp_put(o, 1,  4, &v);   /* mask */
    v = inet_addr(VETH_HOST_IP);   o = dhcp_put(o, 3,  4, &v);   /* router */

    uint32_t dns[3];
    int nd = 0;
    uint32_t gw = host_default_gw();
    if (gw) dns[nd++] = gw;
    dns[nd++] = inet_addr("8.8.8.8");
    dns[nd++] = inet_addr("1.1.1.1");
    o = dhcp_put(o, 6, (uint8_t)(nd * 4), dns);

    *o++ = 255;                                 /* end */
    return (size_t)(o - (uint8_t *)rep);
}

/* Single-lease DHCP responder bound to wdb0.  Runs in a forked child for
 * the life of waydroidd; the container's DhcpClient (a raw packet socket)
 * accepts the broadcast reply, and renewals arrive unicast to us. */
static void dhcp_server_loop(void)
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) { logmsg("dhcp socket: %s", strerror(errno)); return; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, VETH_HOST_IF, IFNAMSIZ - 1);
    if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof ifr) < 0)
        logmsg("dhcp SO_BINDTODEVICE %s: %s", VETH_HOST_IF, strerror(errno));

    struct sockaddr_in me = {
        .sin_family = AF_INET,
        .sin_port = htons(DHCP_SERVER_PORT),
        .sin_addr.s_addr = INADDR_ANY,          /* must catch 255.255.255.255 */
    };
    if (bind(s, (struct sockaddr *)&me, sizeof me) < 0) {
        logmsg("dhcp bind :67: %s (server disabled)", strerror(errno));
        close(s);
        return;
    }
    logmsg("dhcp server on %s, lease %s -> gw/dns %s",
           VETH_HOST_IF, VETH_CONT_IP, VETH_HOST_IP);

    for (;;) {
        struct dhcp_msg req;
        ssize_t n = recv(s, &req, sizeof req, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            logmsg("dhcp recv: %s", strerror(errno));
            break;
        }
        if ((size_t)n < offsetof(struct dhcp_msg, options) + 4 || req.op != 1)
            continue;
        uint32_t magic;
        memcpy(&magic, req.options, 4);
        if (ntohl(magic) != DHCP_MAGIC)
            continue;

        /* bytes present in options[], including the 4-byte magic cookie */
        size_t optlen = (size_t)n - offsetof(struct dhcp_msg, options);
        int type = dhcp_opt_u8(&req, optlen, 53);
        logmsg("dhcp: rx type=%d xid=%08x ci=%08x flags=%04x mac=%02x:%02x:%02x:%02x:%02x:%02x",
               type, ntohl(req.xid), ntohl(req.ciaddr), ntohs(req.flags),
               req.chaddr[0], req.chaddr[1], req.chaddr[2],
               req.chaddr[3], req.chaddr[4], req.chaddr[5]);
        uint8_t reply_type;
        if (type == 1)        reply_type = 2;   /* DISCOVER -> OFFER */
        else if (type == 3)   reply_type = 5;   /* REQUEST  -> ACK   */
        else continue;                          /* ignore the rest   */

        struct dhcp_msg rep;
        size_t rlen = dhcp_build_reply(&req, &rep, reply_type);

        /* RFC 2131 §4.1 reply destination. */
        struct sockaddr_in dst = { .sin_family = AF_INET,
                                   .sin_port = htons(DHCP_CLIENT_PORT) };
        if (req.giaddr) {
            dst.sin_addr.s_addr = req.giaddr;
            dst.sin_port = htons(DHCP_SERVER_PORT);
        } else if (req.ciaddr) {                /* RENEW/REBIND (unicast) */
            dst.sin_addr.s_addr = req.ciaddr;
        } else {
            dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        }
        if (sendto(s, &rep, rlen, 0, (struct sockaddr *)&dst, sizeof dst) < 0)
            logmsg("dhcp send %s: %s",
                   reply_type == 2 ? "OFFER" : "ACK", strerror(errno));
        else
            logmsg("dhcp: tx %s yiaddr=%s dst=%08x len=%zu",
                   reply_type == 2 ? "OFFER" : "ACK", VETH_CONT_IP,
                   ntohl(dst.sin_addr.s_addr), rlen);
    }
    close(s);
}

static void spawn_dhcp_server(void)
{
    pid_t p = fork();
    if (p == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        dhcp_server_loop();
        _exit(0);
    }
    if (p > 0)
        logmsg("dhcp server forked (pid %ld)", (long)p);
    else
        logmsg("dhcp server fork: %s", strerror(errno));
}

static void setup_network(pid_t container_pid)
{
    if (veth_create(container_pid) < 0) {
        logmsg("veth create failed: %s (adb rig disabled)", strerror(errno));
        return;
    }
    if (if_config(VETH_HOST_IF, VETH_HOST_IP, VETH_NETMASK) < 0) {
        logmsg("host veth config failed: %s (networking disabled)",
               strerror(errno));
        return;
    }
    /* The early static IP on the container end is best-effort: it lets the
     * adb rig work before Android's DHCP completes.  DHCP (below) is what
     * really provisions eth0, so a transient failure here (eth0 not yet
     * migrated into the container netns) must NOT skip NAT/DHCP/uplink. */
    if (netns_if_config(container_pid) < 0)
        logmsg("container veth static config failed (DHCP will provision eth0)");
    else
        logmsg("veth up: %s=%s <-> %s=%s", VETH_HOST_IF, VETH_HOST_IP,
               VETH_CONT_IF, VETH_CONT_IP);
    nat_setup();            /* W6: ip_forward + MASQUERADE */
    spawn_uplink_router();  /* W6: mirror OHOS uplink default into `main` */
    spawn_dhcp_server();    /* W6: eth0 gets a real lease -> default route */
    spawn_adb_relay();
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

    setup_network(g_child);

    FILE *f = fopen(PID_FILE, "w");
    if (f) { fprintf(f, "%ld\n", (long)getpid()); fclose(f); }

    /* The container init pid, for the compositor's W5 cgroup freezer
     * (it runs in the host pid-ns and finds the rest by pid-ns inode). */
    FILE *cf = fopen(CONTAINER_PID_FILE, "w");
    if (cf) { fprintf(cf, "%ld\n", (long)g_child); fclose(cf); }

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
    /* Best-effort NAT cleanup on a graceful exit; a supervisor `kill -9`
     * skips this, but nat_setup() is idempotent (-C||-A) and a stray
     * MASQUERADE for a dead subnet matches no traffic, so it is harmless. */
    nat_teardown();
    unlink(PID_FILE);
    return 0;
}
