// Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
// SPDX-License-Identifier: Apache-2.0
// Licensed under the Apache License, Version 2.0.
//
// wmtdetect-init -- native-boot bring-up helper for the MediaTek connsys
// (WMT) WiFi stack.  Two modes:
//
//   wmtdetect-init            (no args)  -- WMT chip-detect / WMT_init.
//   wmtdetect-init wifi-on               -- power on connsys WiFi, retrying.
//
// Mode 1 (chip-detect) replaces the Android `wmt_loader`.  On Android,
// second-stage init runs `/vendor/bin/wmt_loader`, which opens
// /dev/wmtdetect and drives a fixed ioctl sequence: it powers on the
// connsys SoC, reads the chip / A-die IDs, and calls
// COMBO_IOCTL_DO_MODULE_INIT to run the kernel's `WMT_init()`.
// `WMT_init()` is what creates /dev/stpwmt and arms the WiFi/BT
// function-on path -- the WMT driver's own module_init does NOT do it.
// Native boot has no Android second-stage init, so without this the
// connsys WiFi/BT path is dead.
//
// CRITICAL ORDERING: init.x23.cfg must run mode 1 with the *synchronous*
// `syncexec` init command (NOT `exec`, and NOT the Android-ism
// `exec_start` -- which OHOS init does not implement at all and silently
// drops).  WMT_init() has to finish before `wmt_chrdev_wifi` and
// `wlan_drv_gen4m` are insmod'd; if the WiFi driver loads first the
// connsys WiFi function-on fails its firmware download (RST_FW_DL_FAIL).
//
// Mode 2 (wifi-on) writes "1" to /dev/wmtWifi to power on the connsys
// WiFi function.  With mode 1 ordered correctly this succeeds on the
// first try; the retry loop is cheap insurance against a transient
// connsys function-on failure (on failure the connsys does a
// whole-chip-reset and the next attempt succeeds).  It runs as the
// `wmtwifi-on` oneshot service (background, so it never blocks boot) --
// see init.<device>.cfg.
//
// Mode 2 serves both connsys generations, which differ in *when* wlan0
// appears:
//
//   * legacy WMT (x23 / mt6789, has /dev/wmtdetect) -- the wlan driver
//     registers no netdev until the function is powered on, so the "1"
//     write is what creates wlan0.
//   * connac2 / conninfra (ansuz / mt6878, no /dev/wmtdetect) -- the
//     netdev is registered separately by a 'P' (probe) write, after
//     which wlan0 is *visible but dead*: its ndo_open fails with
//     -ENODEV until the "1" write arms the function.  Android's
//     wlan_assistant issues the 'P'; we issue it ourselves if nothing
//     else has by the time we run.
//
// Because wlan0 can therefore already exist while WiFi is still off,
// mode 2 must not use its presence as the done/not-done signal -- doing
// so was why the Plinius came up with wlan0 present but wpa_supplicant
// failing "Could not set interface wlan0 flags (UP): No such device".
// It gates on whether the interface can actually be brought UP instead.
//
// See phase_n9_firmware_peripherals.md N9.2 and phase_p8_wifi_audio.md.

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// From kernel drivers/misc/mediatek/connectivity/common/common_detect/
// wmt_detect.h.  Magic 'w', payload sizeof(int).
#define WMT_DETECT_IOC_MAGIC 'w'
#define COMBO_IOCTL_SET_CHIP_ID         _IOW(WMT_DETECT_IOC_MAGIC, 1, int)
#define COMBO_IOCTL_GET_SOC_CHIP_ID     _IOR(WMT_DETECT_IOC_MAGIC, 3, int)
#define COMBO_IOCTL_DO_MODULE_INIT      _IOR(WMT_DETECT_IOC_MAGIC, 4, int)
#define COMBO_IOCTL_GET_ADIE_CHIP_ID    _IOR(WMT_DETECT_IOC_MAGIC, 9, int)
#define COMBO_IOCTL_CONNSYS_SOC_HW_INIT _IOR(WMT_DETECT_IOC_MAGIC, 10, int)

// Mode 2 retry budget.  With mode 1 (WMT_init) ordered correctly the
// first attempt succeeds; this is insurance against a transient connsys
// function-on failure, after which the connsys whole-chip-resets
// (~1-30 s) and the next attempt succeeds.
//
// On connac2 the loop is doing something else as well: waiting for the
// connsys to become willing.  Measured on the Plinius (the per-attempt
// trace in WIFI_ON_LOG is what produced these numbers):
//
//   t+4.7 s   service starts; /dev/wmtWifi does not exist yet
//   t+8.7 s   node appears (ueventd), writes now fail EIO
//   t+13 s    the "1" write finally takes, blocking ~3.5 s inside the
//             kernel while the connsys powers up and downloads firmware
//   t+17 s    the OHOS WiFi framework auto-starts STA
//
// So arming and the framework's one-shot window land within a second of
// each other, and which goes first has flipped between boots.  Every
// wasted millisecond in this loop is therefore a real risk of booting
// with WiFi off, which is why the retries poll instead of sleeping in
// whole seconds and why nothing waits on a write that already failed.
// Cadence is measured, not guessed, and faster is NOT better: a func-on
// write that fails makes the connsys whole-chip-reset, so hammering it
// pushes readiness further out.  Across four boots each:
//
//   ~1.5 s between attempts -> armed at 16.8 s (attempt 6)
//   ~0.25 s                 -> armed at 18.6-19.0 s (attempt 32), and
//                              two of the four boots came up with no IP
//
// 1.5 s is the best of what has been measured.  If you retune this,
// retune it against /data/log/wmtdetect-init.log over several boots --
// a single boot proves nothing here.
#define WIFI_ON_MAX_RETRY 30
#define WIFI_ON_FAST_TRIES 20
#define WIFI_ON_FAST_MS 1500
#define WIFI_ON_RETRY_MS 15000
// How long to give the driver to register/arm after a write.  The
// function-on runs synchronously inside write(), so a success shows up
// almost immediately; this is a ceiling, not a delay.
#define WIFI_ON_SETTLE_MS 1500
#define WIFI_PROBE_SETTLE_MS 1000
#define WIFI_POLL_MS 100

// Progress goes to stdout (useful when run by hand), to /dev/kmsg so the
// timeline interleaves with the driver's own messages, and to
// WIFI_ON_LOG.  init discards a service's stdout and this runs before
// hilog is much use, so those two are the only channels that survive a
// boot -- and kmsg alone is not enough: the wlan driver is verbose
// enough to wrap the kernel ring buffer inside a minute, which ate the
// first diagnosis of the boot race.  The file is truncated per run, so
// it always holds exactly the last bring-up and never grows.
#define WIFI_ON_LOG "/data/log/wmtdetect-init.log"

static FILE *g_log;

// Seconds since boot, to line the file trace up with dmesg timestamps.
static double uptime_now(void)
{
	double up = 0.0;
	FILE *f = fopen("/proc/uptime", "re");

	if (f) {
		if (fscanf(f, "%lf", &up) != 1)
			up = 0.0;
		fclose(f);
	}
	return up;
}

__attribute__((format(printf, 1, 2)))
static void trace(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;

	printf("wmtdetect-init: %s\n", buf);
	fflush(stdout);

	int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		dprintf(fd, "wmtdetect-init: %s\n", buf);
		close(fd);
	}

	if (g_log) {
		fprintf(g_log, "[%10.3f] %s\n", uptime_now(), buf);
		fflush(g_log);
	}
}

static int call(int fd, unsigned long cmd, unsigned long arg, const char *name)
{
	int r = ioctl(fd, cmd, arg);
	printf("wmtdetect-init: %-20s ret=%d (0x%x) errno=%d %s\n",
	       name, r, r, errno, r < 0 ? strerror(errno) : "");
	return r;
}

// Mode 1 -- WMT chip-detect + WMT_init via /dev/wmtdetect ioctls.
static int do_wmt_init(void)
{
	int fd = -1;

	// /dev/wmtdetect is created by the wmt_drv.ko insmod that runs just
	// before us in init.x23.cfg; allow a brief settle for the node.
	for (int i = 0; i < 50; i++) {
		fd = open("/dev/wmtdetect", O_RDONLY);
		if (fd >= 0)
			break;
		usleep(100 * 1000);
	}
	if (fd < 0) {
		printf("wmtdetect-init: open /dev/wmtdetect failed: %s\n",
		       strerror(errno));
		return 1;
	}

	// Same order as wmt_loader for an integrated-SoC connsys chip.
	call(fd, COMBO_IOCTL_CONNSYS_SOC_HW_INIT, 0, "CONNSYS_SOC_HW_INIT");
	int soc = call(fd, COMBO_IOCTL_GET_SOC_CHIP_ID, 0, "GET_SOC_CHIP_ID");
	call(fd, COMBO_IOCTL_GET_ADIE_CHIP_ID, 0, "GET_ADIE_CHIP_ID");
	call(fd, COMBO_IOCTL_SET_CHIP_ID, (unsigned long)soc, "SET_CHIP_ID");
	int init = call(fd, COMBO_IOCTL_DO_MODULE_INIT, 0, "DO_MODULE_INIT");

	close(fd);
	return init < 0 ? 1 : 0;
}

static int wlan0_present(void)
{
	struct stat st;

	return stat("/sys/class/net/wlan0", &st) == 0;
}

// Whether wlan0 is not merely registered but *armed*.  On connac2 the
// netdev exists from the 'P' probe onward and only the function-on write
// makes its ndo_open work, so presence alone proves nothing -- gating on
// it is what let the Plinius boot with a dead wlan0.  SIOCSIFFLAGS is
// the very call wpa_supplicant fails with -ENODEV in that state, which
// makes it the exact condition to test.  Any flag we change is undone
// before returning, so this stays a read-only probe.
//
// An interface that is *already* IFF_UP is taken as armed without
// probing, because setting IFF_UP on an up interface is a no-op that the
// kernel answers without ever reaching ndo_open -- it would report
// success on a dead radio.  Up therefore has to be read as proof that
// ndo_open once succeeded, which it is: nothing in the system powers the
// connsys down behind an up interface's back (OHOS turning WiFi off does
// not touch /dev/wmtWifi).  Doing it by hand is the one way to
// manufacture an up-but-dead wlan0, and mode 2 stays correct there too
// because it always writes before it believes this.
static int wlan0_ready(void)
{
	struct ifreq ifr;
	int s, ok = 0;

	if (!wlan0_present())
		return 0;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return 0;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
		short was = ifr.ifr_flags;

		if (was & IFF_UP) {
			ok = 1;
		} else {
			ifr.ifr_flags = was | IFF_UP;
			if (ioctl(s, SIOCSIFFLAGS, &ifr) == 0) {
				ok = 1;
				ifr.ifr_flags = was;
				ioctl(s, SIOCSIFFLAGS, &ifr);
			}
		}
	}
	close(s);
	return ok;
}

// Legacy WMT (x23 / mt6789) exposes the chip-detect node that mode 1
// drives; connac2 (ansuz / mt6878) has conninfra instead and no such
// node.  Used only to decide whether the 'P' probe write applies.
static int is_legacy_wmt(void)
{
	return access("/dev/wmtdetect", F_OK) == 0;
}

static ssize_t wmtwifi_write(const char *cmd)
{
	int fd = open("/dev/wmtWifi", O_WRONLY);
	if (fd < 0)
		return -1;

	ssize_t w = write(fd, cmd, 1);
	close(fd);
	return w;
}

static void msleep(int ms)
{
	usleep((useconds_t)ms * 1000);
}

// Wait up to `ms` for `cond` to hold, checking every WIFI_POLL_MS.
// Returns as soon as it does, so a success costs one poll interval
// rather than the whole budget.
static int poll_for(int (*cond)(void), int ms)
{
	for (int t = 0; t < ms; t += WIFI_POLL_MS) {
		if (cond())
			return 1;
		msleep(WIFI_POLL_MS);
	}
	return cond();
}

// Mode 2 -- power on the connsys WiFi function.
//
// Always performs the "1" write, even when wlan0 is already visible: on
// connac2 the netdev is registered by the 'P' probe long before the
// function is armed, so wlan0's presence proves nothing.  See the file
// header.
static int do_wifi_on(void)
{
	// No early exit on wlan0_ready(): always write at least once.  The
	// "1" write is harmless when the function is already on (verified
	// on device against a live association), and going through it means
	// the readiness check is only ever consulted *after* the thing that
	// arms the radio has run.
	g_log = fopen(WIFI_ON_LOG, "we");
	trace("wifi-on start: wmtWifi=%d wlan0=%d legacy_wmt=%d",
	      access("/dev/wmtWifi", F_OK) == 0, wlan0_present(),
	      is_legacy_wmt());

	for (int i = 1; i <= WIFI_ON_MAX_RETRY; i++) {
		int delay = i <= WIFI_ON_FAST_TRIES ? WIFI_ON_FAST_MS
						    : WIFI_ON_RETRY_MS;

		// connac2 only: register the wlan driver if nothing else has
		// (normally Android's wlan_assistant, running in the container,
		// gets there first).  Harmless once wlan0 exists, so it is
		// gated on absence rather than issued unconditionally.
		if (!wlan0_present() && !is_legacy_wmt()) {
			ssize_t p = wmtwifi_write("P");

			// Only wait for a result the write might actually
			// produce.  Waiting after a failed probe burned a
			// second per iteration -- the difference between
			// noticing the connsys is ready and missing the
			// framework's window by it.
			int seen = p < 0 ? 0
					 : poll_for(wlan0_present,
						    WIFI_PROBE_SETTLE_MS);
			trace("try %d: probe write=%zd (%s) -> wlan0=%d", i, p,
			      p < 0 ? strerror(errno) : "ok", seen);
		}

		ssize_t w = wmtwifi_write("1");
		if (w < 0) {
			// Driver or container not ready yet.  Cheap failure --
			// go straight back round without paying the settle.
			trace("try %d: func-on write failed: %s", i,
			      strerror(errno));
			msleep(delay);
			continue;
		}

		if (poll_for(wlan0_ready, WIFI_ON_SETTLE_MS)) {
			trace("wifi-on OK on attempt %d", i);
			return 0;
		}
		trace("try %d: wrote %zd but wlan0 still not armed", i, w);
		msleep(delay);
	}

	trace("wifi-on gave up after %d attempts", WIFI_ON_MAX_RETRY);
	return 1;
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "wifi-on") == 0)
		return do_wifi_on();

	return do_wmt_init();
}
