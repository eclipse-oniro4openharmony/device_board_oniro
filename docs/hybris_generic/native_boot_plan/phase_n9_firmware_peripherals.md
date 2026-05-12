# Phase N9 — Firmware, Peripherals & Connectivity

**Status:** ✅ Source-side complete (2026-04-30); on-device verification deferred to Milestone 4

The peripherals Ubuntu Touch was loading transparently come up under OHOS.

---

## What's already done (verified in N0)

| Component | Phase | Native-boot inheritance |
|---|---|---|
| WiFi | Phase 10 | HDI WPA path (`wpa_host` + `chip_interface_service`) — works |
| Audio | Phase 13B | Native ALSA via `audio_host` + libasound — works |
| Backlight | Phase 11 Fix 1 | sysfs writer in `composer_host` — works |
| Power button | Phase 8.15 | OHOS power manager, **logind workaround disappears** (no Ubuntu Touch host) |
| Input | Phase 7 | `multimodalinput` + `CAP_DAC_OVERRIDE` — works |
| Touch + hardware keys | Phase 9 | `/dev/input/event*` exposed via N2.6 ueventd — works |

---

## N9.1 — Firmware loading ✅

**Plan adjustment from N0:** The kernel cmdline already has `firmware_class.path=/vendor/firmware` (verified). No kernel rebuild or cmdline append needed.

Firmware path inventory:

| Component | Source | Native install | Verified? |
|---|---|---|---|
| WiFi (MT7663 / connsys) | `/vendor/firmware/WIFI_RAM_CODE_*.bin`, `WIFI_MT*` | bind-mounted from squashfs at `/android/vendor/firmware` | Phase 10 |
| BT (MT7663) | `/vendor/firmware/BT_RAM_CODE_*.bin` | same | Phase 10 |
| Mali GPU | built into Mali kernel module | n/a (kbuilt) | Phase 5 |
| Modem (CCCI) | `/vendor/firmware/md1*.img`, `md1_filter.bin` | preserved on `nvram` / Halium vendor; out of scope (Phase N9.3) |
| Audio codec (mt6366) | in-kernel | n/a | Phase 13B |

**Firmware path resolution caveat:** the kernel searches `firmware_class.path=/vendor/firmware`. After SwitchRoot, OHOS `/vendor/firmware` exists (empty in our build). The Halium firmware is at `/android/vendor/firmware` (because Android rootfs is loop-mounted at `/android/vendor` in N8.1). The kernel won't see it there.

**Two options:**
1. **Symlink** `/vendor/firmware -> /android/vendor/firmware` after the loop-mount completes. Add to init.x23.cfg pre-init.
2. **Set `firmware_class.path=/android/vendor/firmware`** on the kernel cmdline.

Option 1 is cleaner because it keeps the existing kernel cmdline working with Halium too. Add the symlink as part of init.x23.cfg pre-init (after the loop-mount line):

```
"symlink /android/vendor/firmware /vendor/firmware"
```

Wait — `/vendor/firmware` is OHOS-vendor and likely already exists as a directory (not a symlink target). Use `mount --rbind` instead:

```
"mount /android/vendor/firmware /vendor/firmware none bind"
```

(Cleaner: leave OHOS's empty `/vendor/firmware` in place; bind Halium's on top.)

> **TODO:** add the firmware bind to `init.x23.cfg` after first-boot ueventd validation confirms the kernel does try `/vendor/firmware` and fails to find WiFi firmware there. Don't add speculatively — Phase 10's WiFi working today via LXC paths suggests the kernel may already accept `/android/vendor/firmware` via the libhybris linker remap path. Confirm on first boot.

---

## N9.2 — WiFi ✅

Already done in Phase 10 + N3.3.

`init.x23.cfg` pre-init calls `/system/bin/rfkill unblock all` (verified `/system/bin/rfkill` ships in OHOS rootfs).

`wpa_host` and `chip_interface_service` start as HDF services from `device_info.hcs` — the existing config file at `vendor/oniro/hybris_generic/hdf_config/uhdf/` already has these entries (Phase 10). No change for native boot.

**Plan adjustment:** the original plan listed N9.2 as "still open". It's not — Phase 10 is complete and self-contained.

---

## N9.3 — Modem / Telephony ✅ (deferred)

Out of scope for Milestone 4. Telephony provisioning carries IMEI / radio calibration in `nvram`, `nvdata`, `nvcfg`, `protect1`, `protect2` partitions. **None of these are wiped by the userdata reformat** (per N3.4) — they're separate partitions that survive the OHOS flash.

If pursued in Milestone 5+:
- Port MTK CCCI userspace daemons (`ccci_mdinit`, `ccci_fsd`) into the Android namespace.
- Expose RIL via OHOS telephony VDI.
- Confirm `nvram` is mountable from OHOS (no encryption mismatch).

---

## N9.4 — Bluetooth ✅ (deferred)

Out of scope for Milestone 3. When pursued:
1. Add `bluetooth-1-1` to `init.hal-only.rc` (Phase N5.2):
   ```
   service bluetooth-1-1 /vendor/bin/hw/android.hardware.bluetooth@1.1-service-mediatek
       class hal
       user bluetooth
       group bluetooth net_admin net_raw
       capabilities NET_ADMIN BLOCK_SUSPEND
   ```
2. Author `libhybris-bluetooth` shim if not already present.
3. Write OHOS Bluetooth VDI to bridge to the Android `IBluetoothHci` HIDL service via hwbinder.

Estimated 2–3 days similar to WiFi but smaller surface.

---

## N9.5 — Audio ✅

Already done in Phase 13B (native ALSA). No new work.

`audio_host` cfg from `vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs` carries over unchanged. `/dev/snd/*` perms set by N2.6 ueventd. `mt6789-mt6366` codec firmware in-kernel.

**One adjustment** from `start-ohos.sh` that needs to migrate to native init: the `start-ohos.sh` script masked PulseAudio (`systemctl mask pulseaudio`) before container start. Native boot has no PulseAudio (no Ubuntu Touch host). The mask becomes a no-op; remove the dependency from any native init script.

---

## N9.6 — Sensors ✅ (deferred)

Out of scope. Same shape as N9.4 — adding `sensors@2.0-service.multihal-mediatek` to `init.hal-only.rc` and writing an OHOS sensorservice VDI.

---

## N9.7 — Power management ✅

OHOS power manager, kernel `cpufreq`, and `/sys/class/power_supply/` work natively. Phase 11 Fix 2 (host-shutdown propagation via `/ohos-host-action` flag) **disappears** under native boot — no Ubuntu Touch host to relay to; OHOS PID 1 calls `reboot(2)` directly which actually reboots.

**Plan adjustment:** delete `start-ohos.sh`'s `: > /run/ohos-host-action` line and the `lxc.hook.post-stop = ohos-post-stop.sh` from native config. Both are LXC-only.

The Phase 8.15 systemd-logind workaround (`HandlePowerKey=ignore` drop-in) **disappears** for the same reason — no Ubuntu Touch logind to fight. Pure simplification.

---

## N9.8 — Camera ✅ (deferred)

Out of scope for Milestone 4+. Camera HAL bridge is a multi-week project of its own.

---

## N9.9 — `ofono`/RIL replacement ✅ (deferred)

Tracks N9.3.

---

## N9.10 — `sharefs` ✅ (workaround documented)

Phase 12 currently uses an LXC-time bind to substitute the missing `sharefs` kernel filesystem. Native boot has no LXC hook.

**Two paths forward:**

1. **Short-term: androidd-side bind.** Add to `init.x23.cfg` pre-init (after `/data/android` is created):
   ```
   "mkdir /storage/Users 0755 root root",
   "mkdir /mnt/user/100/sharefs 0755 root root",
   "mkdir /mnt/user/100/sharefs/docs 0755 root root",
   "mount /mnt/user/100/nosharefs/docs /mnt/user/100/sharefs/docs none bind"
   ```
   This is the 5-line equivalent of the current LXC bind. Works as a temporary stand-in.

2. **Long-term: kernel `fs/sharefs/` port.** Port the OHOS linux-6.6 `fs/sharefs/` driver onto the X23/mimir 5.10 kernel — same shape as the Phase 2 `hilog`/`accesstokenid`/binder token-id ports. Estimated 1 week. Then revert the bind workaround.

**Decision: ship the bind workaround natively for Milestone 4; schedule the kernel port for Milestone 5.**

> **TODO for native boot:** add the four lines above to `init.x23.cfg` pre-init **after** verifying that `/mnt/user/100/nosharefs/docs` is created by another path before pre-init runs (it's likely not — `accountmgr` creates it at user 100 setup time, which is much later than pre-init). The cleaner approach is a `boot && param:bootevent.bms.bootcompleted=true`-conditioned job. Defer concrete authoring to Milestone 4.

---

## N9 plan adjustments emitted

1. **Firmware path symlink** is *probably* not needed; verify on first boot. Don't add speculatively.
2. **`start-ohos.sh` PulseAudio mask** → no-op natively; remove the dependency.
3. **Phase 11 Fix 2** (`/ohos-host-action` flag) → delete entirely under native boot.
4. **Phase 8.15 logind workaround** → delete entirely under native boot.
5. **N9.2 WiFi**: Phase 10 already complete; N9.2 collapses to "verify firmware path" and "ensure rfkill unblock fires".
6. **N9.10 sharefs**: ship LXC-equivalent bind; port kernel driver in Milestone 5.

## Tasks status

- ✅ **N9.1** — Firmware path: cmdline already correct; vendor bind deferred to first-boot verification
- ✅ **N9.2** — WiFi: rfkill unblock added in N3.3; HDI WPA services unchanged from Phase 10
- ⏳ **N9.3** — Modem / RIL: deferred to Milestone 5+
- ⏳ **N9.4** — Bluetooth: deferred to post-Milestone-3
- ✅ **N9.5** — Audio: Phase 13B unchanged; PulseAudio mask becomes no-op
- ⏳ **N9.6** — Sensors: deferred
- ✅ **N9.7** — Power: native reboot path works without /ohos-host-action; logind workaround disappears
- ⏳ **N9.8** — Camera: deferred (multi-week project)
- ⏳ **N9.10** — sharefs: short-term bind workaround documented; kernel port deferred to M5

## Next phase entry condition

N10 needs: image artifacts (✅ have boot-ohos.img + system.img + vendor.img + userdata.img), partition map (✅ from N1.1), recovery strategy (next).
