// Copyright (c) 2026 Oniro Project
// Licensed under the Apache License, Version 2.0.
//
// late-camera-modules -- defer the WPE + DIP kernel-module insmod until
// AFTER androidd has brought up the Halium NS and mtk_cmdq is fully
// initialized.
//
// Why: WPE_Init() / DIP_Init() call cmdqCoreRegisterCB +
// register_pm_notifier.  Loaded at OHOS pre-init these calls deadlock
// PID 1 -- the cmdq subsystem isn't fully wired up yet (mdp_get_group_*
// returns inconsistent state, and register_pm_notifier hits a not-yet-
// initialized notifier chain).  UBports loads these modules late via
// systemd-modules-load.service post-userspace and that ordering works;
// this binary replicates that.
//
// Also chmods /dev/camera-{dip,wpe} on both OHOS and Halium-NS sides so
// the Halium camerahalserver (uid 1047 cameraserver) can open them.  In
// stock Halium, /vendor/etc/init/hw/init.mt6789.rc `on post-fs-data`
// chmods these nodes -- but it runs BEFORE camera_dip_isp6s loads, so
// the chmods silently target non-existent files.  We apply the chmod
// after the modules have created their chardevs.
//
// After perms are fixed, kill the running camerahalserver instance.
// Halium init will respawn it; the respawned copy can now open
// /dev/camera-dip.
//
// See phase_n12_camera_modulemap.md.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int finit_module(int fd, const char *args, int flags)
{
	return syscall(__NR_finit_module, fd, args, flags);
}

static int do_insmod(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "late-camera-modules: open(%s): %s\n",
			path, strerror(errno));
		return -1;
	}
	int r = finit_module(fd, "", 0);
	int e = errno;
	close(fd);
	if (r < 0) {
		// EEXIST means already loaded -- fine, treat as success.
		if (e == EEXIST) {
			fprintf(stderr,
				"late-camera-modules: %s already loaded\n", path);
			return 0;
		}
		fprintf(stderr, "late-camera-modules: finit_module(%s): %s\n",
			path, strerror(e));
		return -1;
	}
	fprintf(stderr, "late-camera-modules: insmod %s ok\n", path);
	return 0;
}

// Scan /proc for a process whose /proc/<pid>/comm matches name.
// Returns 0 if found and writes pid to *out, -1 otherwise.
static int find_pid(const char *name, pid_t *out)
{
	DIR *d = opendir("/proc");
	if (!d) return -1;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!isdigit((unsigned char)e->d_name[0])) continue;
		char path[64], buf[64];
		snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) continue;
		ssize_t n = read(fd, buf, sizeof buf - 1);
		close(fd);
		if (n <= 0) continue;
		buf[n] = 0;
		// strip trailing newline
		while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
			buf[--n] = 0;
		if (strcmp(buf, name) == 0) {
			*out = (pid_t)atoi(e->d_name);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

// Block until `name` shows up in /proc.  Returns 0 on success,
// -1 on timeout.
static int wait_for_process(const char *name, int timeout_sec)
{
	for (int i = 0; i < timeout_sec * 2; i++) {
		pid_t p;
		if (find_pid(name, &p) == 0)
			return 0;
		usleep(500 * 1000);
	}
	return -1;
}

// Fix Halium-NS /dev/camera-* perms.  Uses the same setns+chroot pattern
// as halium_exec and androidd's probe_composer.  hal_pid is a process
// running inside the Halium NS (typically camerahalserver).
static int fix_halium_perms(pid_t hal_pid, const char *dev_path,
			    uid_t uid, gid_t gid, mode_t mode)
{
	char ns_mnt[64], ns_pid[64];
	snprintf(ns_mnt, sizeof ns_mnt, "/proc/%d/ns/mnt", hal_pid);
	snprintf(ns_pid, sizeof ns_pid, "/proc/%d/ns/pid", hal_pid);
	int fd_mnt = open(ns_mnt, O_RDONLY | O_CLOEXEC);
	int fd_pid = open(ns_pid, O_RDONLY | O_CLOEXEC);
	if (fd_mnt < 0 || fd_pid < 0) {
		if (fd_mnt >= 0) close(fd_mnt);
		if (fd_pid >= 0) close(fd_pid);
		fprintf(stderr,
			"late-camera-modules: open NS fds for pid %d: %s\n",
			(int)hal_pid, strerror(errno));
		return -1;
	}

	pid_t child = fork();
	if (child < 0) {
		close(fd_mnt); close(fd_pid);
		fprintf(stderr, "late-camera-modules: fork: %s\n",
			strerror(errno));
		return -1;
	}

	if (child == 0) {
		if (unshare(CLONE_FS) < 0) {
			fprintf(stderr,
				"fix_halium_perms: unshare(CLONE_FS): %s\n",
				strerror(errno));
			// Non-fatal.
		}
		if (setns(fd_mnt, CLONE_NEWNS) < 0) {
			fprintf(stderr, "setns(mnt): %s\n", strerror(errno));
			_exit(2);
		}
		if (setns(fd_pid, CLONE_NEWPID) < 0) {
			fprintf(stderr, "setns(pid): %s\n", strerror(errno));
			_exit(3);
		}
		close(fd_mnt); close(fd_pid);

		// Grandchild lives in the Halium PID NS.
		pid_t grand = fork();
		if (grand < 0) _exit(4);
		if (grand == 0) {
			char rootpath[64];
			snprintf(rootpath, sizeof rootpath,
				 "/proc/%d/root", (int)hal_pid);
			if (chroot(rootpath) < 0) _exit(5);
			if (chdir("/") < 0) _exit(6);

			// Now we're in the Halium NS.  /dev = Halium's /dev tmpfs.
			if (chmod(dev_path, mode) < 0) {
				fprintf(stderr, "halium chmod(%s, 0%o): %s\n",
					dev_path, mode, strerror(errno));
				_exit(7);
			}
			if (chown(dev_path, uid, gid) < 0) {
				fprintf(stderr, "halium chown(%s, %d:%d): %s\n",
					dev_path, uid, gid, strerror(errno));
				_exit(8);
			}
			_exit(0);
		}
		int s;
		if (waitpid(grand, &s, 0) < 0) _exit(9);
		_exit(WIFEXITED(s) ? WEXITSTATUS(s) : 10);
	}

	close(fd_mnt); close(fd_pid);
	int s;
	if (waitpid(child, &s, 0) < 0) return -1;
	int rc = WIFEXITED(s) ? WEXITSTATUS(s) : -1;
	if (rc == 0) {
		fprintf(stderr,
			"late-camera-modules: halium chmod/chown %s -> %d:%d 0%o ok\n",
			dev_path, uid, gid, mode);
	} else {
		fprintf(stderr,
			"late-camera-modules: halium-side fix for %s failed rc=%d\n",
			dev_path, rc);
	}
	return rc;
}

// camerahalserver runs as cameraserver(1047) in group camera(1006).
// Stock Halium init.mt6789.rc uses chown system camera -> uid 1000 gid 1006
// and mode 0660.  Match it.
#define UID_SYSTEM       1000
#define GID_CAMERA       1006
#define CAM_MODE         0660

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	// Wait until Halium HAL services are up.  hwservicemanager registers
	// once Halium init has finished `on post-fs-data` -- a reasonable
	// proxy for "mtk_cmdq is up and ready for callback registrations".
	// Note: /proc/<pid>/comm is truncated to TASK_COMM_LEN (16 incl. NUL),
	// so "hwservicemanager" appears as "hwservicemanage".
	if (wait_for_process("hwservicemanage", 120) < 0) {
		fprintf(stderr, "late-camera-modules: hwservicemanager never "
			"appeared after 120s; aborting\n");
		return 1;
	}
	// Extra settle so any in-progress cmdq probes finish.
	sleep(2);

	// Insmod WPE first (in the original init.x23.cfg order) then DIP.
	if (do_insmod("/mnt/kmodules/camera_wpe_isp6s.ko") < 0) {
		// Non-fatal -- DIP may still load.
	}
	if (do_insmod("/mnt/kmodules/camera_dip_isp6s.ko") < 0) {
		fprintf(stderr,
			"late-camera-modules: DIP insmod failed; bailing\n");
		return 2;
	}

	// Brief settle for the chardevs to appear.
	for (int i = 0; i < 20; i++) {
		struct stat st;
		if (stat("/dev/camera-dip", &st) == 0) break;
		usleep(100 * 1000);
	}

	// OHOS-side perms: ueventd.config already grants 0666 root:root,
	// but be defensive in case ueventd hasn't fired yet.
	chmod("/dev/camera-dip", 0666);
	chmod("/dev/camera-wpe", 0666);

	// Halium-side perms: stock init.mt6789.rc tried `chown system camera`
	// + `chmod 0660` early at boot but those targeted non-existent files.
	// Apply them now via the Halium NS.  camerahalserver may not have
	// spawned yet (it starts in `class hal` shortly after hwservicemanager
	// -- give it a window).
	if (wait_for_process("camerahalserver", 30) < 0) {
		fprintf(stderr,
			"late-camera-modules: camerahalserver did not appear "
			"after 30s; cannot fix Halium-NS perms\n");
		return 0;
	}
	pid_t hal;
	if (find_pid("camerahalserver", &hal) != 0) {
		fprintf(stderr,
			"late-camera-modules: lost camerahalserver between "
			"wait and find\n");
		return 0;
	}
	fix_halium_perms(hal, "/dev/camera-dip",
			 UID_SYSTEM, GID_CAMERA, CAM_MODE);
	fix_halium_perms(hal, "/dev/camera-wpe",
			 UID_SYSTEM, GID_CAMERA, CAM_MODE);

	// camerahalserver was almost certainly running with the old EACCES'd
	// fd state.  Kill it; Halium init's `service` definition is
	// not `oneshot` so it'll restart and pick up the new perms.
	fprintf(stderr,
		"late-camera-modules: killing camerahalserver (pid %d) so "
		"it respawns with new perms\n", (int)hal);
	kill(hal, SIGTERM);

	return 0;
}
