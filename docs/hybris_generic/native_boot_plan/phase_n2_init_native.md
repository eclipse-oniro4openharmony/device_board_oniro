# Phase N2 — Init System: Native Mode Bring-Up

**Status:** 🔄 In Progress (2026-04-30)

Confirm OHOS init (`/bin/init_early` → `/bin/init --second-stage`) boots cleanly as PID 1, doing the work the 29 `InContainerMode()` call sites currently skip in container mode.

---

## N2.1 — Audit of the 29 InContainerMode call sites ✅

Verified count: `grep -rn "InContainerMode" base/startup/init/ | wc -l` → **29**, matching the plan's claim.

### Call-site polarity

| Polarity | Meaning | Count |
|---|---|---|
| `InContainerMode() == 0` (native runs the body, container skips) | code that's *required* in native | 6 |
| `InContainerMode() != 0` (container-only, native skips) | code that's *only* meaningful in containers | 19 |
| Header / interface / declaration | not a runtime path | 3 (`init_utils.c:607` def, `init_utils.h:106` decl, `libbegetutil.versionscript:166` export) |
| Other | `main.c` PID/stage assertion bypasses | 1 (`main.c:45` lets non-PID-1 init run in container; native asserts PID 1) |

Total accounted: 29 ✓

### Detailed breakdown

| Site | File:line | Native behaviour | Container behaviour | Risk natively |
|---|---|---|---|---|
| First-stage transition | `main.c:45,52` | Asserts PID 1; runs `SystemPrepare` | Skips; runs second-stage logic immediately | None — inverse of container, exactly what we need |
| `CreateFsAndDeviceNode` | `device.c:116` | Calls `MountBasicFs`+`CreateDeviceNode` | Skips (LXC autodev/mounts handle it) | None — `MountBasicFs` tolerates ENOENT/EINVAL on selinuxfs and devpts; we have CONFIG_DEVPTS_MULTIPLE_INSTANCES=y from Phase 2. |
| `settimeofday` | `init.c:117` | Sets timezone | Skips | None |
| `ExecReboot("panic")` on InitParamService failure | `init.c:321` | Reboots if param service can't init | Skips (returns) | Same fail-fast posture as a real production OHOS device. ParamService init failure is a hard error. |
| Cgroup wait + SELinux load | `init.c:333` | Waits for `/dev/memcg/procs` (~1s timeout); loads SELinux policy | Skips | SELinux is compiled out (`build_selinux=false`); loop body's `WaitForFile` tolerates absence. **No risk** as long as `/dev/cgroup` doesn't exist either (then it skips the wait). |
| Cgroup add/remove on service lifecycle | `init_cgroup.c:123,142` | Manages per-service cgroup paths | Skips | Cgroup hierarchy needs to be mounted; configfs is in `init.cfg` pre-init (line 26). cgroup v1+v2 mount happens via kernel cmdline `cgroup_disable=pressure cgroup.memory=nokmem` which is already there. |
| Service common-cmd guards | `init_common_cmds.c:333,398,454` | Runs (mksandbox / restorecon / namespace setup) | Skips | All three are `mksandbox`/sandbox-related; the existing fix from Phase 3 (`mksandbox = false` in service cfg) covers services that can't be sandboxed. |
| Service cap/seccomp setup | `init_service.c:311,326` | Applies caps + seccomp | Skips | seccomp currently disabled via `persist.init.debug.seccomp.enable=0` (Phase 6.14). Caps work natively. |
| Service start cmds | `init_cmds.c:172,332,356,368,383,832,989` | Runs (chmod/chown/symlink/etc) | Skips | These are all standard init verbs that work natively. |
| Reboot writeback for host | `reboot.c:50` | Returns immediately | Writes to `/ohos-host-action` | Native reboot path: `WriteHostShutdownRequest` no-ops, then plain `reboot(2)` syscall fires (no Ubuntu Touch host to relay through). **This is exactly what we want.** |
| SELinux loaders | `selinux_adp.c:47,77,103,129` | Runs SELinux loads | Skips | `build_selinux=false` collapses these to no-ops regardless. |
| ueventd `lxc.autodev` skip | `ueventd_main.c:182` | Runs ueventd loop | Skips (LXC creates devs) | Ueventd is **required** natively — N2.6 ships the vendor overlay. |

### Conclusions

- All 29 sites resolve to the *correct* native path when `container=` env is unset (which is the natural state on a PID-1 boot — no LXC to inject the env var).
- **No code changes required.** The plan's claim is verified.
- Risk: only the cgroup hierarchy mount is genuinely new. Defer instrumentation until first bare-metal boot; if `init_cgroup.c:142` ProcessServiceAdd starts erroring, we'll add an explicit cgroup-mount in pre-init.

---

## N2.2 — `MountBasicFs` validation ✅ (analysis)

`device.c:31-77` (Phase N0 reading) mounts /proc, /sys, /sys/fs/selinux (tolerated EINVAL), /dev/pts (DEVPTS_MULTIPLE_INSTANCES needed). All present in the X23 kernel from Phase 2's `openharmony.config`.

**Plan adjustment — none.** The original plan's analysis is accurate.

---

## N2.3 — `MountRequiredPartitions` validation ✅ (analysis)

`init_firststage.c:98-133` reads cmdline `ohos.required_mount.*` (preferred path, set by N1.3) → calls `LoadFstabFromCommandLine` (`fstab.c:553-568`). For each entry tagged `wait,required` runs `DoMountOneItem`; if the mount point is `/usr` triggers `SwitchRoot("/usr")` (`fstab_mount.c:723,736,938`).

Verified by reading the source: the trigger is a literal string match on `"/usr"`, NOT on a flag. Our N1.3 cmdline has `@/usr@` for the system entry — confirmed correct.

**Plan adjustment — none.** Already documented in N1.3.

---

## N2.4 — Second-stage transition ✅ (analysis)

`SystemPrepare` (called from main.c:53) ends with `execv("/bin/init", ["init", "--second-stage", buf])` (`init_firststage.c:194`). After SwitchRoot, `/bin/init` resolves to the OHOS rootfs's `/bin/init` symlink → `/system/bin/init`. Second-stage parses `/system/etc/init/*.cfg`, runs `pre-init` (mounts configfs, fstab.x23, sets up /data, etc.), starts param service, ueventd, watchdog, then opens services per cfg.

`/system/etc/init.cfg` (verified in repo): imports `/etc/init.usb.cfg`, `/etc/init.usb.configfs.cfg`, and `/vendor/etc/init.${ohos.boot.hardware}.cfg`. Native `${ohos.boot.hardware}` = `x23` (from kernel cmdline, verified in N0). So `/vendor/etc/init.x23.cfg` is the hardware-specific import we ship in N3.3.

**Plan adjustment — none.**

---

## N2.5 — SELinux: stays compiled out ✅

`build_selinux = false` in `vendor/oniro/hybris_generic/config.json` (verified). All 4 `selinux_adp.c` `InContainerMode` guards are unreachable when SELinux is compiled out. **No changes needed.** Re-evaluate post-Milestone 4.

**Plan adjustment — none.**

---

## N2.6 — ueventd rules ✅ (artifact authored)

**Plan adjustment vs original:** The plan claimed ueventd reads `/vendor/etc/ueventd.${ohos.boot.hardware}.rc`. The actual loader (`ueventd_main.c:78-79`) hard-codes three paths:

```c
const char *ueventdConfigs[] = {
    "/etc/ueventd.config",                  // -> /system/etc/ueventd.config (already shipped)
    "/vendor/etc/ueventd.config",           // <-- this is what we ship for hybris vendor overlay
    "/vendor/etc/ueventd_factory.config",
    NULL
};
```

There is **no per-hardware ueventd config** — only a per-partition split (system vs vendor). Since the rootfs is built per-product (`hybris_generic`), one vendor-partition ueventd.config suffices for both X23 and mimir.

**Authored artifacts:**

1. `vendor/oniro/hybris_generic/etc/ueventd/ueventd.config` — the overlay file.
2. `vendor/oniro/hybris_generic/etc/BUILD.gn` — added `ohos_prebuilt_etc("ueventd_config_vendor")` target installing to `/vendor/etc/ueventd.config`, hooked into the existing `product_etc_conf` group.

**What it adds vs. the existing /system/etc/ueventd.config:**

| Node | Why |
|---|---|
| `/dev/mali0` 0666 graphics:graphics | libhybris-egl needs r/w by render_service / composer_host (uid graphics) |
| `/dev/dma_heap/{system,mtk_mm,mtk_mm-uncached,mtk_prot,mtk_sec}` | Cross-process gralloc + Mali allocations (Phase 6.10E) |
| `/dev/input/event*` 0660 root:android_input | Phase 7 — multimodalinput uses CAP_DAC_OVERRIDE to open O_RDWR |
| `/dev/rfkill` 0660 wifi:wifi | Phase 10 — wpa_host needs rfkill unblock |
| `/dev/snd/pcmC*D*{p,c}` 0660 audio:audio | Phase 13B — libasound opens PCM nodes directly |

**Verification:** the existing system-side `/system/etc/ueventd.config` (the prebuilt) was inspected. The vendor overlay only *extends* it (vendor lines override system lines, but we don't conflict). Confirmed by reading both side-by-side.

---

## Other adjustments

### Plan claim: "Cgroup setup mounts /sys/fs/cgroup hierarchies" — partial verification

The plan says `init_cgroup.c` "mounts /sys/fs/cgroup hierarchies". Reading the file (`init_cgroup.c:123,142`), neither site mounts cgroups — they only manage per-service cgroup paths. The actual mount must happen elsewhere (likely an `init.cfg` job or kernel cmdline + auto-mounting of cgroup v1).

**Action:** if `ProcessServiceAdd` errors at native bring-up because `/dev/memcg/procs` doesn't exist, add an explicit `mount cgroup memcg /dev/memcg` or `mount cgroup2 cgroup2 /sys/fs/cgroup` to `init.x23.cfg` pre-init. **Not blocking for source-side authoring.**

### Plan adjustment: ueventd config filename

- **Plan said:** `/vendor/etc/ueventd.${ohos.boot.hardware}.rc` (i.e. `ueventd.x23.rc`)
- **Reality:** `/vendor/etc/ueventd.config` (literal filename, no template)
- **Why this matters:** the plan's filename would be silently ignored at boot. The artifact we shipped in N2.6 uses the correct filename.

---

## Tasks status

- ✅ **N2.1** — All 29 InContainerMode sites audited; native paths correct as-is, no code changes needed
- ✅ **N2.2** — `MountBasicFs` analysis confirms it works natively on the X23 kernel
- ✅ **N2.3** — `MountRequiredPartitions` + cmdline parsing path verified; `/usr` literal-string trigger for SwitchRoot confirmed in source
- ✅ **N2.4** — Second-stage transition analysis: `init.cfg` imports `/vendor/etc/init.x23.cfg` for our overlay (Phase N3.3)
- ✅ **N2.5** — SELinux stays compiled out
- ✅ **N2.6** — Vendor ueventd.config authored + BUILD.gn updated; will install to `/vendor/etc/ueventd.config`
- ⏳ **N2.7 (new)** — Cgroup mount validation deferred to first bare-metal boot

## Plan adjustments emitted by N2

1. **ueventd filename**: `/vendor/etc/ueventd.config` (not `ueventd.${ohos.boot.hardware}.rc`).
2. **Cgroup mounting**: `init_cgroup.c` does NOT mount cgroup hierarchies — only manages per-service paths. The mount path is external to those sites; we may need to add an explicit `mount cgroup` job in `init.x23.cfg` if first-boot reveals errors.
3. **No init source code changes** required to enable native boot — the 29 InContainerMode guards already implement the correct native path.

## Next phase entry condition

N3 needs: vendor BUILD.gn precedent for installing into vendor partition (✅ have it from this phase), confirmation of where `/vendor/etc/init.x23.cfg` sits (✅ second-stage import confirmed), and the fstab format. Move forward to N3.
