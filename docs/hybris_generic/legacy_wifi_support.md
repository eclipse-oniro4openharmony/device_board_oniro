# Phase 10: WiFi Support

> **Legacy (LXC-era) document.** Describes the original OHOS-as-LXC-container
> path, which is **no longer maintained** — the project now boots OHOS
> natively (no Ubuntu Touch host, no LXC). Kept as a reference for the HAL /
> driver bring-up detail (libhybris, graphics, audio, WiFi, …) that still
> applies under native boot. For current status start at [README.md](README.md).

## Status: ✅ Complete

---

## Overview

This phase brings WiFi connectivity to the OpenHarmony LXC container on the Volla X23 (and mimir tablet). Unlike the graphics stack (Phase 6), which required custom VDI wrappers around Android HALs via libhybris, WiFi uses the **native OHOS WiFi stack** because:

1. The OHOS container shares the host network namespace (`lxc.namespace.keep = net`), so `wlan0` is directly visible.
2. The kernel's nl80211/cfg80211 subsystem is accessible via netlink sockets from any process in the network namespace.
3. OHOS ships its own `wpa_supplicant` (v2.9) and `hostapd` binaries, built against nl80211.
4. The product config already sets `wifi_feature_non_hdf_driver = true`, meaning the WiFi framework bypasses HDF kernel drivers and talks to `wpa_supplicant` via control sockets.

The main challenges turned out to be: (1) stopping conflicting host/Android WiFi daemons; (2) missing HDF device nodes for the Chip HDI and WPA HDI services; (3) host ID mismatches in the generated `hdf_devhost.cfg`; and (4) a SIGSEGV crash in `GetChipCaps` caused by MediaTek's unsupported vendor ioctl.

---

## Architecture (OHOS 6.1 HDI WPA Path)

**Key discovery:** OHOS 6.1 uses `wifi_feature_with_hdi_wpa_supported = true`, which means the WiFi stack uses the **HDI WPA path** — NOT the legacy `wifi_hal_service` cRPC path described in the original plan. There is no `wifi_hal_service` binary to deploy. Instead:

```
┌─────────────────────────────────────────────────────────────┐
│                 OHOS WiFi Settings UI / Apps                │
└────────────────────────┬────────────────────────────────────┘
                         │ SA IPC (samgr)
                         ▼
┌─────────────────────────────────────────────────────────────┐
│              wifi_manager_service (SA 1120)                  │
│     Talks to chip_interface_service via HDI proxy           │
│     Talks to wpa_interface_service via HDI proxy            │
└────────────┬─────────────────────┬──────────────────────────┘
             │ HDF IPC             │ HDF IPC
             ▼                     ▼
┌──────────────────────┐  ┌────────────────────────────────┐
│ wifi_host (-i 5)     │  │ wpa_host (-i 6)                │
│ chip_interface_svc   │  │ wpa_interface_service           │
│ wlan_interface_svc   │  │ Manages wpa_supplicant lifecycle│
│ Chip HDI v2.0        │  │ Caps: NET_ADMIN, NET_RAW,      │
│                      │  │       DAC_OVERRIDE              │
└──────────────────────┘  └──────────┬─────────────────────┘
                                     │ wpa_ctrl socket
                                     ▼
                          ┌─────────────────────┐
                          │ wpa_supplicant       │
                          │ nl80211 driver       │
                          └──────────┬──────────┘
                                     │ nl80211 (netlink)
                                     ▼
                          ┌─────────────────────┐
                          │ Linux Kernel WiFi    │
                          │ wlan_drv_gen4m_6789  │
                          └─────────────────────┘
```

---

## Completed Steps

### 10.1 — Stop Conflicting WiFi Daemons ✅

**Problem:** Three processes outside the OHOS container competed for `wlan0`:
1. Host `wpa_supplicant` (PID 2487, Ubuntu Touch NetworkManager)
2. Android `wificond` (PID 3161)
3. Android `wlan_assistant` (PID 3137)

**Solution:** Added WiFi daemon management to `start-ohos.sh` (before container start):
- Host `wpa_supplicant`: stopped and **masked** via `systemctl mask wpa_supplicant`; `nmcli radio wifi off` tells NetworkManager to release WiFi.
- Android daemons: stopped via `lxc-attach -n android -- setprop ctl.stop <service>`. Using `ctl.stop` (not `kill`) prevents Android init from respawning them.

**Key fix:** `kill` doesn't work for Android daemons because Android init respawns them immediately. `setprop ctl.stop` is the proper mechanism.

### 10.2 — Add `/dev/rfkill` Bind Mount ✅

**Solution:** Added to LXC config:
```
lxc.mount.entry = /dev/rfkill  dev/rfkill   none bind,create=file,optional 0 0
```

### 10.3 — WiFi HAL Service (Plan Adjusted) ✅

**Original plan:** Deploy missing `wifi_hal_service` binary.

**Discovery:** OHOS 6.1 sets `wifi_feature_with_hdi_wpa_supported = true` in `wifi.gni`, which means the legacy `wifi_hal_service` (cRPC bridge to `wpa_supplicant`) is **not used**. Instead, `wpa_supplicant` is managed by the WPA HDI service (`wpa_interface_service`) loaded by the `wpa_host` HDF process.

**No action needed** — the `wifi_hal_service` binary is intentionally absent.

### 10.4 — UHDF Device Info Configuration ✅

**Problem:** The UHDF `device_info.hcs` was missing two critical entries:
1. `chip_interface_service` — Chip HDI v2.0, needed by `wifi_manager_service` to manage WiFi chip state.
2. `wpa :: host` (`wpa_host`) — separate HDF host for the WPA HDI service.
3. `wifi_host` lacked `CAP_NET_ADMIN`, `CAP_NET_RAW`, `CAP_DAC_OVERRIDE`, `CAP_DAC_READ_SEARCH` capabilities.

**Solution:** Updated `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs`:

```hcs
wlan :: host {
    hostName = "wifi_host";
    priority = 50;
    caps = ["DAC_OVERRIDE", "DAC_READ_SEARCH", "NET_ADMIN", "NET_RAW"];
    gid = ["wifi_host", "wifi_group"];
    wifi_device :: device {
        device0 :: deviceNode {
            policy = 2;
            priority = 100;
            moduleName = "libwifi_hdi_c_device.z.so";
            serviceName = "wlan_interface_service";
        }
    }
    wifi_chip_device :: device {
        device0 :: deviceNode {
            policy = 2;
            priority = 100;
            moduleName = "libchip_hdi_driver.z.so";
            serviceName = "chip_interface_service";
        }
    }
}
wpa :: host {
    hostName = "wpa_host";
    priority = 50;
    caps = ["DAC_OVERRIDE", "DAC_READ_SEARCH", "NET_ADMIN", "NET_RAW"];
    initconfig = ["\"permission\" : [\"ohos.permission.ACCESS_CERT_MANAGER\"]",
                   "\"secon\" : \"u:r:wifi_host:s0\""];
    uid = "wifi";
    gid = ["wifi", "wifi_group", "wifi_host"];
    wpa_device :: device {
        device0 :: deviceNode {
            policy = 2;
            preload = 2;
            priority = 100;
            moduleName = "libwpa_hdi_c_device.z.so";
            serviceName = "wpa_interface_service";
        }
    }
}
```

**Key details:**
- The HCS must be compiled to HCB via `hc-gen -b -i -o <out> hdf.hcs`.
- The `hc-gen -s` flag auto-generates `hdf_devhost.cfg` with correct sequential host IDs.
- `wpa_host` gets instance ID 6 (between `wifi_host=5` and `audio_host=7`).
- Both `/system/etc/init/hdf_devhost.cfg` AND `/vendor/etc/init/hdf_devhost.cfg` must match — OHOS init reads from vendor first.

### 10.5 — GetChipCaps SIGSEGV Fix ✅

**Problem:** `wifi_host` crashed immediately after loading `chip_interface_service`:
```
Signal:SIGSEGV(SEGV_MAPERR)@0x004f505055534e55
#02 pc .../libwifi_hal_default.z.so(SendCmdIoctl+680)
#03 pc .../libwifi_hal_default.z.so(GetChipCaps+164)
```

**Root cause:** `GetChipCaps()` and `WifiGetSupportedFeatureSet()` use `ioctl(SIOCDEVPRIVATE + 1)` — a Huawei/HiSilicon vendor-specific ioctl. The MediaTek gen4m driver doesn't support this ioctl and returns the string `"UNSUPPORTED"` in the buffer. The code then tries to `memcpy_s` from the garbled pointer (`0x524f505055534e55` = "UNSUPPOR" reversed), causing SIGSEGV.

**Fix:** Stubbed out both functions in `drivers/peripheral/wlan/chip/wifi_hal/wifi_ioctl.cpp` to return 0 without calling the ioctl:
```cpp
uint32_t GetChipCaps(const char *ifName)
{
    HDF_LOGI("GetChipCaps: returning 0 (vendor ioctl unsupported)");
    return 0;
}
```
Also marked `SendCmdIoctl` and `SendCommandToDriverByInterfaceName` as `__attribute__((unused))` since they are no longer called.

### 10.6 — WiFi Manager Service ✅

`wifi_manager_service` (SA 1120) starts automatically at boot as `wifi` user. It is configured as `ondemand` in `wifi_standard.cfg` and loads on first client request (e.g., Settings UI opening WiFi page). No changes were needed.

### 10.7–10.8 — DHCP and DNS ✅

DHCP and DNS work automatically:
- The OHOS DHCP client obtains an IP via DHCP after WiFi association.
- DNS resolves correctly (shared network namespace inherits host DNS config).
- `ping -c 3 google.com` succeeds with ~16ms latency.

No manual configuration was needed.

### 10.9 — End-to-End Validation ✅

- **WiFi toggle:** ON in Settings → WiFi networks appear within seconds.
- **Association:** Connects to WPA2 network successfully.
- **IP assignment:** DHCP obtains IP automatically.
- **Internet:** `ping -c 3 8.8.8.8` → 0% packet loss, ~16ms RTT.
- **DNS:** `ping -c 3 google.com` → resolves and reaches `142.251.209.46`.
- **WiFi processes:** `wifi_host` (-i 5), `wifi_manager_service`, `wpa_host` (-i 6) all running stably.

### 10.10 — Volla Tablet (mimir) Bring-Up ✅ (2026-04-10)

After Phase 9 completed mimir device bring-up, turning on WLAN in Settings produced a "WLAN Operation Error" toast even though the same codebase worked on X23. Two mimir-specific bugs were found and fixed:

**Bug 10.10.A — `libwifi_hal_default.z.so` not installed into the rootfs**

Symptom in hilog on WLAN toggle:
```
I C01566/HDF_LOG_TAG: Wifi HAL start enter
W C03f07/MUSL-LDSO: load libwifi_hal_default.z.so failed, namespace=ndk no inherits, errno=2
W C03f07/MUSL-LDSO: load libwifi_hal_default.z.so failed, namespace=default, errno=2
E C01566/HDF_LOG_TAG: failed to open vendor hal library: libwifi_hal_default.z.so
E C01566/HDF_LOG_TAG: Wifi HAL start failed.
E C01560/HalDeviceManager: CheckChipHdiStarted, call Init failed! ret:-1
```

The chip HDI service (`libchip_controller_chip_interface_service_2.0.z.so`) dlopens `libwifi_hal_default.z.so` at start — this is the vendor HAL lib referenced by `drivers/peripheral/wlan/chip/hdi_service/wifi_vendor_hal_list.cpp`. Our hybris-patched version (with the `GetChipCaps`/`WifiGetSupportedFeatureSet` stubs from 10.5) is built by `device/soc/oniro/hybris_generic/hardware/wlan/BUILD.gn` and does exist at `out/hybris_generic/hdf/drivers_peripheral_wlan/libwifi_hal_default.z.so`, but it was never making it into `packages/phone/vendor/lib64/` and therefore never into the deployed rootfs.

Root cause: the target had mismatched `part_name` / `subsystem_name`:
```gn
# BEFORE — wrong
install_images = [ chipset_base_dir ]
subsystem_name = "hdf"
part_name = "drivers_peripheral_wlan"
```
The target lives under `device/soc/oniro/hybris_generic/` whose `bundle.json` registers component `hybris_generic_soc` under subsystem `oniro_soc_products`. The OHOS build system only collects a module's install metadata into `parts_modules_info.json` when the target's `part_name`/`subsystem_name` match the enclosing bundle.json component. With the mismatched values the `.so` compiled fine (the target is reachable from `hybris_generic_soc_group` in `device/soc/oniro/hybris_generic/BUILD.gn`) but the install collection + packaging phase silently dropped it. Compare with the sibling `hardware/display/BUILD.gn`, which correctly uses `part_name = "hybris_generic_soc"`.

Fix in `device/soc/oniro/hybris_generic/hardware/wlan/BUILD.gn`:
```gn
# AFTER — correct
install_images = [ chipset_base_dir ]
subsystem_name = "oniro_soc_products"
part_name = "hybris_generic_soc"
```

After a full (non-`--fast-rebuild`) build, `libwifi_hal_default.z.so` now lands in `packages/phone/vendor/lib64/` and the rootfs tarball. Short-term workaround while a full build is pending: `adb push` the built lib manually into `/home/phablet/openharmony/rootfs/vendor/lib64/`.

**Bug 10.10.B — `phy0` soft-blocked by rfkill at boot**

Once `libwifi_hal_default.z.so` was loadable, the chip HDI started successfully and OHOS reported `wifi state = 1`, but every scan attempt failed:
```
I C01566/HDF_LOG_TAG: start scan
E C01566/HDF_LOG_TAG: failed to configure setup; result = -100
E C01560/HalDeviceManager: Scan, call StartScan failed! ret:-1
```
`-100` is `-ENETDOWN` from the nl80211 `NL80211_CMD_TRIGGER_SCAN` path (`wifi_scan.cpp:259`), because `wlan0` was still `DOWN` (`cat /sys/class/net/wlan0/operstate` → `down`). Trying `ifconfig wlan0 up` from the host returned:
```
SIOCSIFFLAGS: Operation not possible due to RF-kill
```
`rfkill list` showed:
```
1: phy0: Wireless LAN
    Soft blocked: yes
    Hard blocked: no
```
On Ubuntu Touch, `urfkill` (and/or Android `wlan_assistant` before it's killed) can leave `phy0` soft-blocked at boot. The X23 path happens not to hit this; mimir does, every time. `rfkill unblock all` on the host cleared the block and `wlan0` came UP immediately, after which scans returned networks and association/DHCP/DNS all worked normally.

Permanent fix: added an `rfkill unblock all` block to `start-ohos.sh`, right after the existing WiFi-daemon-stop section (so it runs before the container starts and before any OHOS service touches `wlan0`):
```bash
if command -v rfkill >/dev/null 2>&1; then
    echo "Unblocking rfkill on all radios..." | tee -a $LOG_FILE
    rfkill unblock all 2>&1 | tee -a $LOG_FILE || true
fi
```
Harmless on X23 (phy0 is already unblocked there).

**Diagnostic order for future WiFi regressions on mimir:**
1. `rfkill list` on host — phy0 should show `Soft blocked: no`.
2. `hdc shell "ifconfig wlan0"` and `cat /sys/class/net/wlan0/operstate` — must be `UP` / `up` after the container has tried to enable WiFi.
3. `hdc shell "ls /vendor/lib64/libwifi_hal_default.z.so"` — absence gives the `MUSL-LDSO ... errno=2` + `Wifi HAL start failed` signature above.
4. `hdc shell "hilog | grep -E 'HDF_LOG_TAG|HalDeviceManager|WifiStaHalInterface'"` while toggling WLAN.

---

## Files Modified

| File | Change |
|------|--------|
| `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs` | Added `chip_interface_service`, `wpa :: host`, caps on `wifi_host` |
| `device/soc/oniro/hybris_generic/hardware/wlan/wifi_ioctl.cpp` | Hybris-local copy of `wifi_ioctl.cpp` with `GetChipCaps`/`WifiGetSupportedFeatureSet` stubbed to avoid SIGSEGV on MediaTek (built into `libwifi_hal_default.z.so` by the sibling BUILD.gn) |
| `device/soc/oniro/hybris_generic/hardware/wlan/BUILD.gn` | **10.10.A fix** — `subsystem_name`/`part_name` changed from `hdf`/`drivers_peripheral_wlan` to `oniro_soc_products`/`hybris_generic_soc` so the built lib gets installed into the rootfs |
| `device/board/oniro/hybris_generic/utils/lxc/config` | Added `/dev/rfkill` bind mount |
| `device/board/oniro/hybris_generic/utils/start-ohos.sh` | Added WiFi daemon stop (host `wpa_supplicant` mask, Android `ctl.stop`); **10.10.B fix** — added `rfkill unblock all` |

---

## HCS Configuration Reference

The WiFi HDF configuration is in `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs`. After modification, rebuild with:
```bash
# Rebuild HCB (binary config)
hc-gen -b -i -o hdf_default.hcb vendor/oniro/hybris_generic/hdf_config/uhdf/hdf.hcs

# Rebuild hdf_devhost.cfg (init service config)
hc-gen -s -i -o hdf_devhost.cfg vendor/oniro/hybris_generic/hdf_config/uhdf/hdf.hcs
```

Both files must be deployed to `/vendor/etc/hdfconfig/hdf_default.hcb` and `/vendor/etc/init/hdf_devhost.cfg` (and `/system/etc/init/hdf_devhost.cfg`).

---

## Key Paths Reference

| Item | Path |
|------|------|
| UHDF device_info.hcs | `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs` |
| WiFi ioctl (patched) | `drivers/peripheral/wlan/chip/wifi_hal/wifi_ioctl.cpp` |
| LXC config | `device/board/oniro/hybris_generic/utils/lxc/config` |
| start-ohos.sh | `device/board/oniro/hybris_generic/utils/start-ohos.sh` |
| wpa_supplicant config | `/data/service/el1/public/wifi/wpa_supplicant/wpa_supplicant.conf` (runtime) |
| WiFi crash logs | `/data/log/faultlog/faultlogger/cppcrash-wifi_host-*` |
| HCB output | `out/hybris_generic/packages/phone/vendor/etc/hdfconfig/hdf_default.hcb` |
| libwifi_hal_default.z.so (built) | `out/hybris_generic/hdf/drivers_peripheral_wlan/libwifi_hal_default.z.so` |
| libwifi_hal_default.z.so (installed, post full build) | `out/hybris_generic/packages/phone/vendor/lib64/libwifi_hal_default.z.so` → deployed as `/vendor/lib64/libwifi_hal_default.z.so` |
| Hybris wlan BUILD.gn + patched ioctl | `device/soc/oniro/hybris_generic/hardware/wlan/` |
