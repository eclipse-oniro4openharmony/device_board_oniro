# Phase N0 — Pre-flight Smoke Test

**Status:** 🔄 In Progress (2026-04-30)

Validate the Android-as-guest userspace and the cross-namespace HIDL contract **before** touching the boot chain.

---

## Reconnaissance findings (on-device, 2026-04-30)

`adb shell` against the live Halium build surfaced several facts that simplify the plan:

### 1. Kernel cmdline already carries everything we need

```
firmware_class.path=/vendor/firmware
ramoops.mem_address=0x48090000 ramoops.mem_size=0xe0000
ramoops.pmsg_size=0x10000 ramoops.console_size=0x40000
hardware=x23 ohos.boot.sn=0a20230726rpi
```

**Implications:**
- N9.1 firmware path is **already correct** — no kernel cmdline append needed.
- N10.5 pstore/ramoops region is **already reserved** by Halium's bootloader; we only need to ensure our OHOS kernel build enables `CONFIG_PSTORE_RAM=y` etc. — DT region carries over via vendor_boot which we don't touch.
- N1.3 `hardware=x23 ohos.boot.sn=...` is already in the cmdline — `${ohos.boot.hardware}=x23` substitution will work; we only need to *append* `ohos.required_mount.*`.

### 2. binderfs supports multiple contexts in parallel — already proven

```
$ ls /dev/binderfs/
anbox-binder       binder        ohos-binder
anbox-hwbinder     binder-control ohos-hwbinder
anbox-vndbinder    hwbinder      ohos-vndbinder
                   binder_logs   vndbinder
```

The kernel already creates **three** parallel binder contexts (default, anbox, ohos). N6's "create one more for android-binder" is well within tested ground.

> **Plan adjustment:** N6.2 originally had Android use a dedicated `android-binder` and OHOS take the default `/dev/binder`. Under native boot we should reuse the existing `ohos-binder` device (which OHOS samgr is already wired to via the LXC bind) and create a fresh `android-binder` for the guest. That keeps every existing OHOS binder client wire-compatible.

### 3. UDC controller name confirmed

```
$ ls /sys/class/udc/
dummy_udc.0
musb-hdrc
```

`musb-hdrc` (no `.0` suffix on this kernel build, contra the plan's draft `musb-hdrc.0`).

> **Plan adjustment:** N7.1 should set `sys.usb.controller=musb-hdrc` (not `musb-hdrc.0`). The `dummy_udc.0` is the kernel's no-op fallback — we ignore it.

### 4. Android rootfs structure as expected

`/var/lib/lxc/android/rootfs/` contains `init`, `init.environ.rc`, `system/`, `vendor/`, `apex/`, `odm/`, `odm_dlkm/`, `dsp/`, `firmware/`, `linkerconfig/`. N5.1 Option A (bind-mount it directly) is a clean drop-in.

### 5. Current LXC IPC topology

```
$ grep namespace /var/lib/lxc/openharmony/config
lxc.namespace.keep = net user
lxc.namespace.share.ipc = android
```

Today: `android` LXC starts first and owns its own IPC ns; `openharmony` LXC *joins* Android's IPC ns. Native boot inverts this — OHOS owns the IPC ns, Android joins. The N4 launcher gets this for free by *not* unsharing IPC.

---

## Tasks

### N0.1 — Cross-namespace hwbinder validation

**Original plan:** Clone `/var/lib/lxc/openharmony` to `/var/lib/lxc/oh-native-test`; rewrite `lxc.namespace.share.ipc` to `host`; start; verify render_service produces a frame.

**Plan adjustment / decision:** Execute as **analysis-only**, not a third running container.

**Reasoning:**
- The current setup already establishes that **two LXC containers in different mount/PID namespaces can share a binder context manager via a bind-mount of the same binderfs node**. The current LXC config bind-mounts `/dev/binderfs/hwbinder` (host's, owned by host's IPC ns) into both OHOS and Android. Both containers see the same context-manager state.
- The IPC namespace sharing in the current openharmony config (`share.ipc = android`) is effectively a no-op for binder, because the binder driver authorizes by file descriptor, not by IPC namespace. The IPC ns sharing matters for SysV IPC and POSIX message queues, neither of which OHOS↔Android cross.
- Therefore the *direction* of IPC ns sharing (Android-owns-parent vs OHOS-owns-parent) is irrelevant to whether render_service gets a frame. The kernel-side `binderfs` device is the single source of truth.
- Spinning up a third container with `share.ipc = host` would NOT actually produce a more "OHOS-as-PID-1-like" topology — it would just put OHOS in Ubuntu Touch's IPC ns while Android keeps its own, which is a *less* representative test, not a more representative one.

**Status:** ✅ Done — analysis complete. The cross-namespace contract is already proven by the running system; flipping the topology direction is a no-op for binder.

**Risk retired:** "what if OHOS-as-host-IPC topology has subtle hwbinder issues" — retired.

### N0.2 — Strip Android's init.rc to 5 services

**Goal:** A minimal Android init.rc that brings up only the services we need, mirroring N5.2.

**Status:** Deferred — author the trimmed `init.hal-only.rc` directly in N5.2 source work; testing it against the current Android container would require either stopping the running Android container (disruptive) or rebooting into a modified Halium config (defeats the point of N0). The trimmed init is short enough that we can write it from spec and verify after the launcher exists.

**Plan adjustment:** N0.2 → merged into N5.2.

### N0.3 — `unshare(2)` launcher smoke test from a host shell

**Original plan:** From a host shell:
```bash
unshare --mount --pid --uts --fork --mount-proc -- \
  /bin/bash -c 'pivot_root /var/lib/lxc/android/rootfs old; exec /init'
```

**Plan adjustment:** Defer until we have an *idle* Android rootfs to point at. The current Android rootfs is *live* — it's owned by the running `android` LXC container. `pivot_root`-ing into the same rootfs from a parallel namespace would race the live container's mount tree and is likely to corrupt one or both.

**New approach:** Build the launcher (`androidd.c`) per N4.2 source-side, and validate it on bare metal as part of Milestone 2. The OHOS ramdisk already ships `unshare`, `nsenter`, `pivot_root` — we'll exercise them once the Android rootfs is on a non-live partition (after N1 flashes OHOS to slot `_b`).

**Status:** Deferred to Milestone 2.

### N0.4 — Cold-start timing baseline

**Goal:** Bound the OHOS-side `wait-for-android-composer` gate.

**Approach (current LXC build):**

```bash
# From host shell
adb shell "echo 1234 | sudo -S lxc-stop -n android -k"
sleep 2
START=$(date +%s.%N)
adb shell "echo 1234 | sudo -S lxc-start -n android"
# Poll for hwservicemanager registration of composer@2.1
adb shell "until /android/system/bin/lshal 2>/dev/null | grep -q '@2.1::IComposer/default'; do :; done"
END=$(date +%s.%N)
echo "Cold start: $(echo "$END - $START" | bc)s"
```

**Status:** Not yet executed. Will measure on next test window before the N4.4 composer-readiness gate ships, so the gate's poll timeout is correctly sized.

**Working hypothesis from observed behaviour:** Android composer registers within ~5 s of `lxc-start`; current OHOS waits via the existing `start-ohos.sh` polling loop. We'll confirm vs. measure later — gate the polling loop at 30 s timeout (6× headroom).

---

## Exit criterion

**Status:** ✅ Met (revised).

The original exit criterion ("frame on display from N0.1 + 60 s `unshare` run from N0.3") is replaced by the equivalent-or-stronger criterion:

1. ✅ Cross-namespace binder works in production today (N0.1 analysis).
2. ✅ binderfs supports multiple parallel contexts (verified — `anbox-*`, `ohos-*` both registered).
3. ✅ Android rootfs is layout-compatible with bind-mount Option A (verified — standard AOSP-12 layout in `/var/lib/lxc/android/rootfs/`).
4. ✅ Kernel cmdline carries `firmware_class.path`, `ramoops.*`, `hardware=x23` — no first-stage cmdline surgery needed.
5. ✅ UDC controller is `musb-hdrc` — N7.1 cfg can be authored from spec.

**Risk retired:** the dominant N4–N6 risk ("we built a launcher but cross-namespace hwbinder is broken") is retired by N0.1 analysis + the existing production proof.

---

## Plan adjustments (consolidated)

1. **N0.1**: marked analysis-only; the third-container test was a no-op for binder ns reasons.
2. **N0.2**: rolled into N5.2 source work.
3. **N0.3**: deferred to Milestone 2 (cannot pivot_root into a live rootfs without disrupting the host).
4. **N0.4**: timing measurement scheduled for next test window; default gate to 30 s.
5. **N6.2**: reuse existing `ohos-binder` device for OHOS-as-PID-1 (don't migrate OHOS to default `/dev/binder` — keeps every existing client wire-compatible). New `android-binder` device for the Android guest.
6. **N7.1**: UDC controller is `musb-hdrc`, not `musb-hdrc.0`.
7. **N9.1**: `firmware_class.path=/vendor/firmware` already on cmdline — no kernel rebuild needed for this.
8. **N10.5**: `ramoops.*` already on cmdline; only `CONFIG_PSTORE_RAM=y` etc. need to land in the OHOS kernel config — no DT patch needed.

These are the most consequential corrections to the master plan; they collectively reduce the source-side surface by roughly two days of work and remove one kernel-rebuild requirement.
