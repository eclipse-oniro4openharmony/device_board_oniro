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
// WiFi function, which creates wlan0.  With mode 1 ordered correctly
// this succeeds on the first try; the retry loop is cheap insurance
// against a transient connsys function-on failure (on failure the
// connsys does a whole-chip-reset and the next attempt succeeds).  It
// runs as the `wmtwifi-on` oneshot service (background, so it never
// blocks boot) -- see init.x23.cfg.
//
// See phase_n9_firmware_peripherals.md N9.2.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
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
#define WIFI_ON_MAX_RETRY 20
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

// Mode 2 -- power on connsys WiFi, retrying until wlan0 appears.
static int do_wifi_on(void)
{
	if (wlan0_present()) {
		printf("wmtdetect-init: wlan0 already up\n");
		return 0;
	}

	for (int i = 1; i <= WIFI_ON_MAX_RETRY; i++) {
		int fd = open("/dev/wmtWifi", O_WRONLY);
		if (fd < 0) {
			// wmt_chrdev_wifi not ready yet -- wait and retry.
			printf("wmtdetect-init: open /dev/wmtWifi failed "
			       "(try %d): %s\n", i, strerror(errno));
			sleep(WIFI_ON_RETRY_SEC);
			continue;
		}
		ssize_t w = write(fd, "1", 1);
		close(fd);

		// The connsys func-on / FW download runs synchronously inside
		// the write(); a short settle lets wlan0 register on success.
		sleep(3);
		if (wlan0_present()) {
			printf("wmtdetect-init: wifi-on OK on attempt %d\n", i);
			return 0;
		}
		printf("wmtdetect-init: wifi-on attempt %d failed "
		       "(write=%zd) -- connsys resets, retrying in %d s\n",
		       i, w, WIFI_ON_RETRY_SEC);
		sleep(WIFI_ON_RETRY_SEC);
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
