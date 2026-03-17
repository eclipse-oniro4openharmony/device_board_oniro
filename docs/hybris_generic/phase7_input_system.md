# Phase 7: Input System Integration

## Status: ✅ Complete

**Completed 2026-03-27.** Touch input and hardware keys working on the physical Volla X23 display. All five `/dev/input/event*` devices enumerated by `multimodalinput` via libinput; touch coordinates correctly mapped to 720×1560.

---

## Environment Analysis

### Host input devices (`/dev/input/`)

| Node   | Name             | Type                       | Notes |
|--------|------------------|----------------------------|-------|
| event0 | mtk-pmic-keys    | kbd — EV_KEY               | Power / PMIC keys |
| event1 | mtk-kpd          | kbd — EV_KEY               | Hardware keypad |
| event2 | mtk-tpd          | MT touchscreen — EV_KEY+ABS | Primary touch input; `INPUT_PROP_DIRECT`; MT protocol B (SLOT, TOUCH_MAJOR, TOUCH_MINOR, POSITION_X, POSITION_Y, TRACKING_ID) |
| event3 | mt6789 Headset Jack | EV_KEY+EV_SW            | Headset events — not needed for UI |
| event4 | ff_key           | kbd — EV_KEY               | Fingerprint side key |

All nodes: `crw-rw---- root:android_input(GID 1004)`, mode `0660`.

### OHOS `multimodalinput` service

- **Already running** at boot (PID exists in container, `start-mode: condition` is satisfied by WindowManager).
- Runs as **uid `input` (6696)**, supplementary groups: `input`, `tp_host`, `lcd_host`, `sensor_host`, `consumerir_host` — none of which map to GID 1004.
- Device discovery: `HotplugDetector` sets an `inotify` watch on `/dev/input/` for `IN_CREATE`/`IN_DELETE`, then calls `Scan()` to enumerate existing files. On service start it opens all present `event*` files via `libinput_path_add_device()` → `open_restricted()` → `open(path, O_RDWR|O_NONBLOCK)`.
- **`/dev/input/` is not mounted** inside the container (`lxc.autodev = 1` creates a bare tmpfs `/dev`). Until it is, the service is idle.
- **Permission problem**: even after mounting, uid 6696 cannot open `root:1004 0660` files. `open_restricted` returns `EACCES` for every device.

### Coordinate mapping

`multimodalinput` uses libinput, which reads ABS_MT_POSITION_X/Y min/max from the kernel to normalise touch coordinates to [0, 1]. It re-scales to logical display pixels using the `DisplayInfo` struct from WindowManager/DisplayManager. Since the display stack is already working (720×1560, launcher visible), coordinate mapping is automatic once events flow. Confirmed working — no calibration quirks file required.

---

## Implementation

### 7.1 — LXC bind-mount `/dev/input` ✅

**Files changed:**
- `device/board/oniro/hybris_generic/utils/lxc/config`
- `/var/lib/lxc/openharmony/config` (live)

Added after the `/dev/dri` entry:

```
lxc.mount.entry = /dev/input   dev/input    none rbind,create=dir,optional 0 0
```

- `rbind` includes `by-path/` symlinks so libinput's `realpath()` in `open_restricted` resolves correctly.
- `optional` prevents container start failure if the host kernel does not expose `/dev/input/` (e.g. when running in a VM for testing).
- `lxc.cgroup.devices.allow = a` is already set, so no per-device cgroup rule is needed.

All five `event*` nodes are visible inside the container after restart. libinput enumerates them all; irrelevant ones (headset jack event3, ff_key event4) produce no ABS/touch events and cause no log noise.

### 7.2 — Fix `open_restricted` permissions via `CAP_DAC_OVERRIDE` ✅

**Root cause:** `multimodalinput` runs as uid `input` (6696), not a member of GID `android_input` (1004). libinput opens devices with `O_RDWR|O_NONBLOCK` — which requires both read and write permission on the `0660` nodes.

**Critical finding during implementation:** The plan originally specified `CAP_DAC_READ_SEARCH`, but this only bypasses DAC *read* permission checks. For `O_RDWR` opens, the write permission check is not bypassed and still returns `EACCES`. Confirmed via strace: a root-started process opened all five devices successfully, but the `input`-uid process with only `DAC_READ_SEARCH` could not. The correct capability is **`CAP_DAC_OVERRIDE`**, which bypasses read, write, and execute DAC checks.

**Init cfg merge load-order bug found and fixed:** The OHOS init system merges duplicate service definitions from all `etc/init/*.cfg` files on MUSL builds (`init_service_manager.c`: `INIT_LOGI("Service %s already exists, updating.")`). The merge is last-writer-wins per field. `ReadFileInDir` uses `readdir()` (filesystem/inode order, not alphabetical). On this rootfs, the original `multimodalinput.cfg` was consistently returned by `readdir` *after* an overlay named `hybris_input_caps.cfg`, so the original (with only `["SYS_NICE"]`) overwrote the caps every boot. Fixed by naming the override file `z_multimodalinput_caps.cfg` — ensuring it is appended to the directory after the original and thus consistently returned last by `readdir`.

**New file:** `device/board/oniro/hybris_generic/cfg/z_multimodalinput_caps.cfg`

```json
{
    "services" : [{
        "name" : "multimodalinput",
        "path" : ["/system/bin/sa_main", "/system/profile/multimodalinput.json"],
        "uid" : "input",
        "gid" : ["input", "tp_host", "lcd_host", "sensor_host", "consumerir_host"],
        "caps" : ["SYS_NICE", "DAC_OVERRIDE"]
    }]
}
```

**`device/board/oniro/hybris_generic/cfg/BUILD.gn`** — added alongside `hybris_graphic_env`:

```gn
ohos_prebuilt_etc("hybris_input_caps") {
  source = "z_multimodalinput_caps.cfg"
  relative_install_dir = "init"
  install_images = [ "system" ]
  part_name = "device_hybris_generic"
  subsystem_name = "device_hybris_generic"
}

group("hybris_input_caps_group") {
  deps = [ ":hybris_input_caps" ]
}
```

**`device/board/oniro/hybris_generic/BUILD.gn`** — added dep:

```gn
group("hybris_generic_group") {
  deps = [
    "cfg:hybris_graphic_env_group",
    "cfg:hybris_input_caps_group",
  ]
}
```

### 7.3 — multimodalinput re-scan ✅

`multimodalinput` starts at `post-fs-data`, after all LXC bind mounts are in place. On fresh container start, `HotplugDetector::Scan()` enumerates the mounted `event*` nodes and opens them. Confirmed working — no manual re-scan required.

After fresh container boot, verified `CapPrm=0x800002` (`SYS_NICE | DAC_OVERRIDE`) and all five event devices open:

```
lrwx------ input input  20 -> /dev/input/event4
lrwx------ input input  21 -> /dev/input/event3
lrwx------ input input  22 -> /dev/input/event2
lrwx------ input input  23 -> /dev/input/event1
lrwx------ input input  26 -> /dev/input/event0
lr-x------ input input  13 -> anon_inode:inotify
lrwx------ input input   7 -> /data/log/libinput/libinput.log
```

### 7.4 — Verification ✅

**Capabilities confirmed:** `CapPrm=0x800002 = CAP_SYS_NICE(23) | CAP_DAC_OVERRIDE(1)`.

**All five devices open** as shown above. libinput log at `/data/log/libinput/libinput.log` confirms all devices configured as supported input devices.

**Event pipeline active:** `InputManagerImpl::SetWindowInputEventConsumer` registers windows; `InputWindowsManager` processes pointer events; `GestureNavigationManage` touch callbacks active in Launcher.

### 7.5 — End-to-end touch test ✅

Touch input confirmed working on physical Volla X23 display. Lockscreen and launcher respond to touch. Hardware keys (power button event0) functional.

---

## Deliverable

Functional touch and hardware button input in the OpenHarmony UI on the Volla X23 (720×1560), with all five `/dev/input/event*` devices enumerated by `multimodalinput` via libinput and touch coordinates correctly mapped to display pixels.

---

## Key Findings

| Finding | Detail |
|---------|--------|
| libinput opens `O_RDWR` | `CAP_DAC_READ_SEARCH` is insufficient; `CAP_DAC_OVERRIDE` required |
| init cfg load-order | `ReadFileInDir` uses `readdir()` (inode order); override file must be added to directory *after* the original to load last and win the merge |
| No axis calibration needed | libinput reads ABS_MT min/max from kernel; Mediatek TPD reports panel-pixel coordinates matching 720×1560 exactly |
| All 5 devices opened | event3 (headset jack) and event4 (ff_key) produce no ABS events; no log noise observed |

