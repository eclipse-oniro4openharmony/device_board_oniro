# Power Off & Backlight Control — Investigation and Plan

> **Legacy (LXC-era) document.** Describes the original OHOS-as-LXC-container
> path, which is **no longer maintained** — the project now boots OHOS
> natively (no Ubuntu Touch host, no LXC). Kept as a reference for the HAL /
> driver bring-up detail (libhybris, graphics, audio, WiFi, …) that still
> applies under native boot. For current status start at [README.md](README.md).

Two related bugs observed after the Phase 8.15 power-button fix:

1. **Screen goes black on power button press, but the panel backlight stays on.**
2. **"Power off" from the OHOS menu stops the OHOS container, but the Ubuntu Touch host (and the phone) stays on.**

This document captures the root cause analysis and the planned fix for both.

**Status (2026-04-10):** Fix 1 (backlight) is **implemented, deployed, and verified on Volla X23**. Fix 2 (container → host shutdown propagation) is **implemented and built; on-device deploy + verification still pending** (see "Outcome" block under Fix 2 for details).

---

## Problem 1 — Screen black but backlight stays on

### Call chain on short power-button press

1. Host `systemd-logind` ignores the power key (Phase 8.15 `HandlePowerKey=ignore` drop-in).
2. OHOS `multimodalinput` → `PowerMgrService` → `DeviceStateAction::SetDisplayState(DISPLAY_OFF)` → `DisplayPowerMgrClient` → `ScreenAction::SetDisplayPower` → `Rosen::ScreenManagerLite::SetScreenPowerForAll(POWER_OFF)`.
3. Inside `render_service`, `RSScreen` calls the composer HDI via two independent paths:
   - **Panel power path** — `SetDisplayPowerStatus(POWER_STATUS_OFF)` → our VDI `HybrisDisplay::SetDisplayPowerStatus` → `hwc2_compat_display_set_power_mode(HWC2_POWER_MODE_OFF)` → MTK HWC2 HAL. **This blanks the panel composition → screen turns black.** ✔
   - **Backlight path** — `RSScreen::SetScreenBacklight(0)` → HDI `SetDisplayBacklight(devId, 0)` → our VDI.

### Root cause

`HybrisComposerVdiImpl::SetDisplayBacklight` at `device/soc/oniro/hybris_generic/hardware/display/src/display_composer/hybris_composer_vdi_impl.cpp:312-318` is a **no-op stub**:

```cpp
int32_t HybrisComposerVdiImpl::SetDisplayBacklight(uint32_t devId, uint32_t level)
{
    DISPLAY_UNUSED(devId);
    DISPLAY_UNUSED(level);
    /* Backlight control not available via HWC2 compat API */
    return HDF_SUCCESS;
}
```

On MediaTek Halium devices, the panel backlight is **not** driven by HWC2 — it is a separate LED/PWM controller exposed by the kernel at `/sys/class/leds/lcd-backlight/brightness` (or `/sys/class/backlight/panel0-backlight/brightness`). On stock Android this is why SurfaceFlinger talks HWC2 for panel power while PowerManagerService talks the `lights` HAL for backlight; the two paths are independent. Our VDI bridges only the HWC2 path, so the backlight sits at whatever level it was before the OFF request.

`GetDisplayBacklight` at lines 305-310 is also a stub that just returns a hardcoded `100`.

**Corollary:** the OHOS brightness slider, automatic dimming, and auto-brightness are all silently broken by the same stub. The user hasn't noticed because the initial backlight level is fine for normal use.

---

## Problem 2 — Power off stops the container, not the host

### Call chain on "Power off" menu tap

1. OHOS UI → `PowerMgrService::ShutDownDevice` → `DevicePowerAction::Shutdown("shutdown")` (`base/powermgr/power_manager/services/native/src/actions/default/device_power_action.cpp:51`).
2. → `DoRebootExt("shutdown", reason)` → init's reboot plugin `DoRebootShutdown` (`base/startup/init/services/modules/reboot/reboot.c:145`).
3. → `syscall(__NR_reboot, ..., LINUX_REBOOT_CMD_POWER_OFF, ...)` — a raw reboot syscall from OHOS container's PID-1 init.

### Root cause

Inside an LXC container with its own PID namespace, the Linux kernel does **not** power off the host when the container's PID-1 invokes `reboot()`. Instead, the kernel converts the syscall into termination of that PID namespace (kills container init). The host kernel, host systemd, and Ubuntu Touch stay up; only the OHOS container stops. **Reboot-from-menu has the same problem** (long press → Reboot): `DoReboot` at `reboot.c:85` also calls `reboot(RB_AUTOBOOT)` unconditionally.

There is no container-mode guard anywhere in the reboot plugin today. `InContainerMode()` (`base/startup/init/services/utils/init_utils.c:607`) is already used elsewhere in init for Phase 1's privileged-op bypasses, but nothing in the reboot path.

---

## Plan

### Fix 1 — Backlight control (`SetDisplayBacklight` + `GetDisplayBacklight`) ✅ DONE (2026-04-10)

**Files to change:**
- `device/soc/oniro/hybris_generic/hardware/display/src/display_composer/hybris_composer_vdi_impl.cpp`
- (possibly) `hybris_composer_vdi_impl.h` if new members are needed

**Approach:** write to kernel sysfs directly. Sysfs is already bind-mounted into the container (`device/board/oniro/hybris_generic/utils/lxc/config:46`), so no mount changes are required.

**Steps:**
1. At class construction / first use, probe for the backlight sysfs node in a small ordered list:
   - `/sys/class/leds/lcd-backlight/brightness` (MTK/Halium convention)
   - `/sys/class/backlight/panel0-backlight/brightness`
   - `/sys/class/backlight/panel1-backlight/brightness` (mimir tablet fallback)

   Cache the chosen path plus its `max_brightness` (read once; typically 255 on MTK but can differ — don't assume).

2. Implement `SetDisplayBacklight(devId, level)`:
   - Scale OHOS input level (0–255 per HDI contract) to the kernel `max_brightness` range.
   - `open(path, O_WRONLY | O_CLOEXEC)` → `write` the scaled value → `close`. Log the mapping once per call at `DISPLAY_LOGI` level.
   - Return `HDF_FAILURE` if the write fails. If the sysfs file is simply absent (probe found nothing), log a one-time warning and return `HDF_SUCCESS` so brightness changes don't cascade into user-visible errors.
   - Cache the last-written level for `GetDisplayBacklight`.

3. Implement `GetDisplayBacklight(devId, level&)`: return the cached last-written level, or read back from sysfs on first call.

4. **Belt-and-braces**: in `HybrisDisplay::SetDisplayPowerStatus` (`hybris_display.cpp:213`), when `status == POWER_STATUS_OFF`, also force the backlight to 0 via the same sysfs path. This guarantees the display fully blanks even if the OHOS brightness path is bypassed in some code paths. Gate this on a small helper so we don't duplicate the sysfs probe logic.

5. Verify against both the Volla X23 and mimir tablet kernels (different panels) — the probe order covers both.

**Permissions / caps check:** the `display_composer` UHDF host runs under the `display_composer` uid/gid per `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs:314+`. The sysfs brightness node on Halium kernels is typically `root:system 0664` (writable by the `system` group). Confirm after build that the composer host has write permission; if not, add `CAP_DAC_OVERRIDE` to the host's `caps` list in `device_info.hcs` (same pattern as the multimodalinput fix in Phase 7).

**Testing plan (after build + deploy):**
- `hdc shell "cat /sys/class/leds/lcd-backlight/brightness"` — confirm the node exists and is writable.
- Short-press power button → screen goes black AND backlight off.
- Short-press again → backlight on + display composition resumes.
- Brightness slider in Settings → verify smooth scaling across the range.
- Relevant hilog tags: `DISP_HDI_COMP`, `UL_POWER`, `HybrisDisplay`, `HybrisComposerVdi`.

#### Outcome (2026-04-10)

All of Fix 1 implemented and verified on Volla X23.

**Code changes (persistent):**
- `device/soc/oniro/hybris_generic/hardware/display/src/display_composer/hybris_composer_vdi_impl.h` — added `static int32_t WriteBacklight(uint32_t level)` and `static uint32_t GetLastBacklight()`.
- `device/soc/oniro/hybris_generic/hardware/display/src/display_composer/hybris_composer_vdi_impl.cpp` — anonymous-namespace file-scope state + mutex, `ProbeBacklightLocked()` walks `kBacklightProbePaths[]` (`/sys/class/leds/lcd-backlight/brightness`, `/sys/class/backlight/panel0-backlight/brightness`, `/sys/class/backlight/panel1-backlight/brightness`), reads `max_brightness` next to it, seeds `g_backlightLast` from the current kernel value; `WriteBacklight` scales `0..255 → 0..max_brightness` with rounding, writes via `open(O_WRONLY | O_CLOEXEC) + snprintf + write`, falls back to HDF_SUCCESS (with a one-time warning) if no node is found; `SetDisplayBacklight` / `GetDisplayBacklight` stubs replaced with real impls.
- `device/soc/oniro/hybris_generic/hardware/display/src/display_composer/hybris_display.cpp` — belt-and-braces `HybrisComposerVdiImpl::WriteBacklight(0)` in `SetDisplayPowerStatus` when `status == POWER_STATUS_OFF`. This turned out to be **load-bearing**, not just insurance (see below).
- `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs:325` — composer_host caps changed from `["SYS_NICE"]` to `["SYS_NICE", "DAC_OVERRIDE"]`. The backlight sysfs node is owned `system:system 0664` on the Volla X23 kernel; composer_host runs as `composer_host:composer_host` (uid/gid 3036) and is not in the `system` group, so `DAC_OVERRIDE` is required for the write to succeed. Init's `GetCapByString` recognises the name without a `CAP_` prefix (see `base/startup/init/services/init/init_capability.c:33-88`).

**On-device state after deploy:**
- `CapEff` of `composer_host` pid: `0x00800002` = `CAP_SYS_NICE | CAP_DAC_OVERRIDE` (was `0x00800000`).
- Probed node at runtime: `/sys/class/leds/lcd-backlight/brightness`, `max_brightness=255` (1:1 scaling, no rounding loss on X23).

**Verification log excerpts (hilog, tag `HybrisDisp`):**
```
01:13:01.458 WriteBacklight: Backlight: level=44  -> raw=44  (max=255)   # power-shell display -s 50
01:13:02.509 WriteBacklight: Backlight: level=175 -> raw=175 (max=255)   # power-shell display -s 200
01:13:13.663 SetDisplayPowerStatus devId=0 status=3                       # power-shell suspend (POWER_STATUS_OFF)
01:13:14.228 WriteBacklight: Backlight: level=0   -> raw=0   (max=255)   # belt-and-braces fired
01:13:34.993 SetDisplayPowerStatus devId=0 status=0                       # power-shell wakeup (POWER_STATUS_ON)
01:13:35.488 WriteBacklight: Backlight: level=175 -> raw=175 (max=255)   # OHOS brightness restore
```

`cat /sys/class/leds/lcd-backlight/brightness` reflected the writes each time (175 → 0 → 175 across the suspend/wakeup cycle).

**Key observation — belt-and-braces is required, not optional.**
The plan treated the `WriteBacklight(0)` call inside `SetDisplayPowerStatus(POWER_STATUS_OFF)` as defensive insurance. In practice, OHOS's suspend path **does not** call `SetDisplayBacklight(0)` separately before calling `SetDisplayPowerStatus(POWER_STATUS_OFF)` — the only thing that blanks the backlight on suspend is the belt-and-braces write. Without it the screen would turn black (HWC2 power mode off) but the backlight would stay at whatever level was last set. Leave this call in place.

**Deploy details:**
- Built with `./build.sh --product-name hybris_generic --ccache --fast-rebuild --build-target libdisplay_composer_vdi_impl`, then a full `--fast-rebuild` (no target) to regenerate `hdf_default.hcb` + `vendor/etc/init/hdf_devhost.cfg` from the updated hcs. A narrow `--build-target` does not pick up hcs changes because the hcb is not a dep of the VDI library.
- Pushed two artifacts directly into the running rootfs rather than a full `deploy-lxc-container.sh` re-deploy (faster test cycle):
  - `out/.../vendor/lib64/passthrough/libdisplay_composer_vdi_impl.z.so` → `/home/phablet/openharmony/rootfs/vendor/lib64/passthrough/`
  - `vendor/etc/init/hdf_devhost.cfg` — patched in place with a one-shot python script to add `DAC_OVERRIDE` to composer_host caps while **preserving** the `env` block that `deploy-lxc-container.sh` adds post-extraction (HYBRIS_LD_LIBRARY_PATH, LD_LIBRARY_PATH, HYBRIS_EGLPLATFORM). Copied to both `/vendor/etc/init/` and `/system/etc/init/`.
- Bounced the LXC container (`lxc-stop -k -n openharmony; lxc-start -n openharmony`).
- A future full `deploy-lxc-container.sh` run will regenerate the cfg from the hcs source correctly — the deploy script's JSON edit only touches the `env` field, leaving `caps` alone, so the caps change survives. No follow-up maintenance needed.

**Not yet verified:**
- Volla Tablet (mimir). The probe order covers it (`panel0-backlight` / `panel1-backlight` fallback), but needs a physical test when the tablet is plugged in. Expected to "just work" because the HDF `device_info.hcs` change is shared between both boards.

**Not addressed (intentional):**
- OHOS brightness-slider scaling curve: `power-shell display -s 50` writes 44, `-s 200` writes 175. The non-1:1 mapping is in OHOS `DisplayPowerMgr` (discount/override logic) upstream of the HDI — our VDI receives the already-scaled value and forwards it linearly. Not a bug in the VDI.

---

### Fix 2 — Container shutdown propagates to host ✅ IMPLEMENTED (2026-04-10, not yet deployed)

**Design:** write a flag file from OHOS init's reboot plugin before the `reboot()` syscall, then have the host act on the flag via an `lxc.hook.post-stop` hook. This keeps the clean-stop path (container teardown → host systemd → host poweroff) and distinguishes voluntary shutdowns from crashes or manual `lxc-stop`.

**Why not alternatives:**
- `/proc/sysrq-trigger` direct write: skips host service cleanup, risks corruption on Ubuntu Touch's writable data partition.
- DBus to host `logind`: requires bind-mounting the host DBus socket + user/uid reconciliation across namespaces; significantly more code for a one-shot use case.
- Host helper socket/FIFO: more moving parts than a flag file for identical semantics.

**Files to change:**

1. **`base/startup/init/services/modules/reboot/reboot.c`**
   - Add a small helper, e.g. `WriteHostShutdownRequest(const char *action)` that, when `InContainerMode()` returns 1, writes `action` (`"poweroff"` or `"reboot"`) to `/ohos-host-action` (a bind-mounted flag file). Guard with `#include "init_utils.h"`.
   - In `DoRebootShutdown` (line 145): call `WriteHostShutdownRequest("poweroff")` before the syscall block. Leave the syscall in place — it still correctly stops the container.
   - In `DoReboot` (line 85): same, with `"reboot"`. This also fixes reboot-from-menu propagating to the host.
   - This is purely an add-on before the actual reboot call — existing code paths untouched.

2. **`device/board/oniro/hybris_generic/utils/lxc/config`**
   - Add a bind-mount for the flag file:
     ```
     lxc.mount.entry = /run/ohos-host-action ohos-host-action none bind,create=file,optional 0 0
     ```
     Container-side path: `/ohos-host-action` (root of container rootfs). Host-side path: `/run/ohos-host-action`. Both must exist as empty files before container start.
   - Add the post-stop hook:
     ```
     lxc.hook.post-stop = /home/phablet/openharmony/ohos-post-stop.sh
     ```

3. **New host script `device/board/oniro/hybris_generic/utils/ohos-post-stop.sh`** (deployed to `/home/phablet/openharmony/ohos-post-stop.sh` by `deploy-lxc-container.sh`):
   ```sh
   #!/bin/sh
   FLAG=/run/ohos-host-action
   [ -r "$FLAG" ] || exit 0
   action=$(cat "$FLAG")
   : > "$FLAG"     # clear so a subsequent crash doesn't re-trigger
   case "$action" in
       poweroff) systemctl poweroff ;;
       reboot)   systemctl reboot ;;
   esac
   exit 0
   ```

4. **`device/board/oniro/hybris_generic/utils/deploy-lxc-container.sh`**
   - Install `ohos-post-stop.sh` to `/home/phablet/openharmony/ohos-post-stop.sh` with `0755 root:root`.
   - Ensure `/run/ohos-host-action` is created (empty) at boot via a systemd tmpfiles.d drop-in. `/run` is tmpfs, so the file disappears on reboot and must be recreated before the OHOS container starts.
     - Install `/etc/tmpfiles.d/ohos-host-action.conf` with:
       ```
       f /run/ohos-host-action 0666 root root -
       ```
   - Persist these on the writable data partition using the same bind-mount pattern as `ohos-logind-powerkey.service` in Phase 8.15.

5. **Documentation:** add a Bug 8.19 entry to `legacy_system_stability.md` once deployed, and update `MEMORY.md`'s `project_hybris_stability_status.md` accordingly.

**Why this is safe:**
- If OHOS crashes instead of cleanly shutting down, the flag file is empty → post-stop script exits 0 → host stays up. Good.
- If the user manually runs `lxc-stop -n openharmony` on the host, flag is empty → host stays up. Good.
- If OHOS reboots cleanly, flag=`reboot` → host reboots. Good — this also fixes reboot-from-menu.
- The flag is cleared by the post-stop script itself so a subsequent kernel-level container respawn can't re-trigger.

**Edge case to watch:** if the host is configured to auto-restart the OHOS container on exit (e.g., `Restart=always` in `ohos.service`), a `systemctl poweroff` race might try to start the container again before systemd reaches `poweroff.target`. If that happens in practice, add `Conflicts=poweroff.target reboot.target` to `ohos.service`. Confirm after first test.

**Testing plan:**
- Build, deploy.
- From OHOS: power menu → **Power off** → host phone powers down to charging LED.
- Reboot from menu → host reboots.
- Crash test: `hdc shell "kill -9 1"` inside container → container dies, host stays up (flag empty).
- Host `lxc-stop -k -n openharmony` → host stays up.

#### Outcome (2026-04-10)

All of Fix 2 code written and `librebootmodule.z.so` built cleanly via `./build.sh --product-name hybris_generic --ccache --fast-rebuild --build-target rebootmodule`. Symbol `WriteHostShutdownRequest` confirmed present in the output `.so` at `out/hybris_generic/lib.unstripped/startup/init/librebootmodule.z.so`. **On-device deploy + verification is still pending** — holding for user sign-off because a failed test will power off or reboot the Volla X23.

**Code changes (persistent):**
- `base/startup/init/services/modules/reboot/reboot.c` — added static helper `WriteHostShutdownRequest(const char *action)` (lines 34-70) guarded by `InContainerMode()`, which writes `"poweroff"` or `"reboot"` to `/ohos-host-action` (fails silently if the flag file is absent, e.g. on non-hybris_generic targets). Called from `DoReboot` before `DoRoot_("reboot", RB_AUTOBOOT)` and from `DoRebootShutdown` before the `reboot(RB_POWER_OFF)` / `LINUX_REBOOT_CMD_POWER_OFF` syscall block. Actual reboot syscall left in place — it still correctly tears the container down. Transitive includes (via `init_utils.h` / existing `open()` usage in `WritePowerOffReason`) cover `O_WRONLY | O_TRUNC | O_CLOEXEC`, `errno`, `ssize_t`; no new `#include` lines needed. `init_utils` is already a listed dep in `BUILD.gn`, so no GN changes required either.
- `device/board/oniro/hybris_generic/utils/lxc/config` — appended two directives just before the `lxc.cgroup.devices.allow` block: `lxc.mount.entry = /run/ohos-host-action ohos-host-action none bind,create=file,optional 0 0` (host `/run/ohos-host-action` → container `/ohos-host-action`) and `lxc.hook.post-stop = /home/phablet/openharmony/ohos-post-stop.sh`.
- `device/board/oniro/hybris_generic/utils/ohos-post-stop.sh` — **new** POSIX-sh hook script (executable, `0755`). Reads `/run/ohos-host-action`, clears it, and `exec`s `systemctl poweroff` or `systemctl reboot` based on the content. Empty flag → exit 0 (covers manual `lxc-stop`, container crash, and host-initiated teardown). Unknown action → logger warning + exit 0. Uses `logger -t ohos-post-stop` for traceability in the host journal.
- `device/board/oniro/hybris_generic/utils/start-ohos.sh` — appended a "Prepare host shutdown-request flag" block that truncates `/run/ohos-host-action` to zero bytes and chmods it `0666` immediately before `lxc-start -n openharmony -F`. This replaces the planned tmpfiles.d drop-in: `/run` is tmpfs, but `start-ohos.sh` already runs on every container (re)start as `ohos.service` `ExecStart`, so a one-line `: > /run/ohos-host-action` is equivalent to a tmpfiles.d entry without an extra file to deploy. Matches the pattern already used for the logind `HandlePowerKey=ignore` drop-in earlier in the same script.
- `device/board/oniro/hybris_generic/utils/deploy-lxc-container.sh` — added `adb push` of `ohos-post-stop.sh` to `/home/phablet/openharmony/` and `chmod +x` on the device so the post-stop hook lands next to `start-ohos.sh`.

**Deviations from the plan:**
- **No tmpfiles.d drop-in.** The plan called for `/etc/tmpfiles.d/ohos-host-action.conf`. Replaced with an inline `: > /run/ohos-host-action` in `start-ohos.sh`. Simpler (no new file to deploy, no systemd dependency), lands the flag at exactly the moment the container needs it, and mirrors the existing logind power-key drop-in written into `/run` by the same script. The tmpfiles.d approach can be reinstated later if `start-ohos.sh` ever grows multi-instance semantics, but it does not today.
- **Guard in the init plugin, not just at the bind mount.** The plan implicitly assumed `open` on `/ohos-host-action` would be cheap to fail on non-container builds. Added an explicit `InContainerMode() == 0 → return` early out so non-hybris_generic OHOS builds (e.g., a future bare-metal target reusing the same init module) skip the helper entirely instead of hitting `access()` on every reboot. Matches the pattern used by the Phase 1 init patches.

**Not yet done (requires physical device test):**
1. Redeploy the init module. Two options:
   - Fast path: push `out/hybris_generic/startup/init/librebootmodule.z.so` directly to `/home/phablet/openharmony/rootfs/system/lib64/init/reboot/librebootmodule.z.so` (same pattern Fix 1 used for `libdisplay_composer_vdi_impl.z.so`). Also push the updated `config`, `start-ohos.sh`, and new `ohos-post-stop.sh` via `adb push`; move `config` to `/var/lib/lxc/openharmony/config`, the scripts to `/home/phablet/openharmony/`, and `chmod +x` the post-stop hook.
   - Slow path: full `./deploy-lxc-container.sh` run — picks everything up automatically.
2. After deploy, bounce the container so the new lxc config (bind mount + post-stop hook) takes effect.
3. Run the four tests in the "Testing plan" block above:
   - Power menu → Power off → host powers down.
   - Power menu → Restart → host reboots.
   - `hdc shell "kill -9 1"` → host stays up.
   - Host `sudo lxc-stop -k -n openharmony` → host stays up.
4. If all four pass: add a Bug 8.19 entry to `legacy_system_stability.md`, update `MEMORY.md`'s `project_hybris_stability_status.md`, and flip the status line at the top of this file to "implemented, deployed, and verified".

**Safety notes for the first test run:**
- Unplug anything plugged into the device you don't want cycled. First test will either power the phone off (success) or hang the container (failure).
- Recovery from a hang: `adb shell "echo 1234 | sudo -S lxc-stop -k -n openharmony; echo 1234 | sudo -S systemctl start ohos"` from the dev host.
- The post-stop hook uses `exec systemctl poweroff` so the failing case cannot spin forever — worst case is the flag file has a garbage value and the script exits 0.

---

## Order of work

1. ~~Wait for the current background build to finish.~~
2. ~~**Fix 1 (backlight) first**~~ — **done 2026-04-10.** See "Outcome" block under Fix 1 above.
3. ~~**Fix 2 (shutdown propagation) next**~~ — **code written and built 2026-04-10.** See "Outcome" block under Fix 2 above. Touches init + LXC config + deploy script; on-device test still pending (holds the phone's power state, wants user sign-off before the first run).
4. After Fix 2 on-device verification: update `legacy_system_stability.md` (add Bug 8.19 entry) + memory index. Fix 1 is small enough that its record lives in this plan doc; it does not need its own bug number unless the mimir tablet verification surfaces a follow-up.
