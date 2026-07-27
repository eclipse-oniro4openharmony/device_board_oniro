// Copyright (c) 2026 Oniro Project
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
// The first few retries are fast because on connac2 the other reason to
// retry is that the wlan driver has not been probed yet -- we want to
// win the race against the OHOS WiFi framework, which auto-starts STA
// about 10 s into boot and gives up after four tries.
#define WIFI_ON_MAX_RETRY 20
#define WIFI_ON_FAST_TRIES 8
#define WIFI_ON_FAST_SEC 2
#define WIFI_ON_RETRY_SEC 15

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

// Mode 2 -- power on the connsys WiFi function.
//
// Always performs the "1" write, even when wlan0 is already visible: on
// connac2 the netdev is registered by the 'P' probe long before the
// function is armed, so wlan0's presence proves nothing.  See the file
// header.
static int do_wifi_on(void)
{
	if (wlan0_ready()) {
		printf("wmtdetect-init: wlan0 already armed\n");
		return 0;
	}

	for (int i = 1; i <= WIFI_ON_MAX_RETRY; i++) {
		int delay = i <= WIFI_ON_FAST_TRIES ? WIFI_ON_FAST_SEC
						    : WIFI_ON_RETRY_SEC;

		// connac2 only: register the wlan driver if nothing else has
		// (normally Android's wlan_assistant, running in the container,
		// gets there first).  Harmless once wlan0 exists, so it is
		// gated on absence rather than issued unconditionally.
		if (!wlan0_present() && !is_legacy_wmt()) {
			ssize_t p = wmtwifi_write("P");
			printf("wmtdetect-init: wlan probe write=%zd (try %d)\n",
			       p, i);
			sleep(1);
		}

		ssize_t w = wmtwifi_write("1");
		if (w < 0) {
			// wmt_chrdev_wifi not ready yet -- wait and retry.
			printf("wmtdetect-init: /dev/wmtWifi func-on write "
			       "failed (try %d): %s\n", i, strerror(errno));
			sleep(delay);
			continue;
		}

		// The connsys func-on / FW download runs synchronously inside
		// the write(); a short settle lets wlan0 register on success.
		sleep(3);
		if (wlan0_ready()) {
			printf("wmtdetect-init: wifi-on OK on attempt %d\n", i);
			return 0;
		}
		printf("wmtdetect-init: wifi-on attempt %d failed "
		       "(write=%zd) -- connsys resets, retrying in %d s\n",
		       i, w, delay);
		sleep(delay);
	}

	printf("wmtdetect-init: wifi-on gave up after %d attempts\n",
	       WIFI_ON_MAX_RETRY);
	return 1;
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "wifi-on") == 0)
		return do_wifi_on();

	return do_wmt_init();
}
