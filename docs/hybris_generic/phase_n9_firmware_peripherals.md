# Phase N9 — Firmware, Peripherals & Connectivity

**Status:** 🔄 Partial (2026-05-18).  WiFi (Phase 10) and audio (Phase 13B)
are native, but both needed their MT6789 kernel modules bundled into
`vendor_boot` — they live in the Android second-stage / `vendor_dlkm`
module set that native boot never loads.  Audio: N9.5.  WiFi: N9.2 — the
connsys (WMT) module set, plus a `wmtdetect-init` helper that runs the
`wmt_loader` chip-detect ioctl the kernel `WMT_init()` needs.  Bluetooth
and sensors still need their Android HALs running in `androidd`'s namespace;
defer until N4's exec-init SEGV is resolved (see `phase_n4_androidd.md`).

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

> **RESOLVED (2026-05-18, via N9.5 audio bring-up).** Confirmed on the
> device: OHOS `/vendor/firmware` does *not* even exist, so any
> in-kernel `request_firmware()` lands at `-ENOENT`. The AW883xx
> speaker codec's `request_firmware("aw883xx_acf.bin")` was the first
> consumer to actually exercise this path — without the firmware the
> amp never initialises (silent speaker). Fix shipped: `init.x23.cfg`
> pre-init does
> `write /sys/module/firmware_class/parameters/path /android/vendor/firmware`
> before the audio `insmod`s. `firmware_class.path` is a writable
> module param; the `write` retargets *all* in-kernel firmware lookups
> at Halium's firmware tree (loop-mounted at `/android/vendor` in N8.1)
> — cleaner than a bind, and it needs no `/vendor/firmware` dir to
> exist. (Pitfall: do **not** set it with `echo` from a shell — the
> trailing newline becomes part of the path and every lookup fails
> `…/firmware\n/<name>` → ENOENT. OHOS init's `write` command writes
> the value with no newline, so the `init.x23.cfg` line is correct.)

---

## N9.2 — WiFi ✅ (2026-05-18 — fixed + verified)

The OHOS-side WiFi userspace from Phase 10 (HDI WPA path: `wpa_host` +
`chip_interface_service` HDF services from `device_info.hcs`, the
`GetChipCaps` SIGSEGV stub, the `libwifi_hal_default.z.so` install fix)
carries over unchanged. **What did NOT carry over is the connsys kernel
stack** — exactly the same second-stage-module problem as audio (N9.5)
and the GPU/touch modules (N8.11/N8.13).

The MT6789 WiFi is a MediaTek **connsys** (WMT) chip. Its driver set —
`connadp`, `connfem`, `btif_drv`, `wmt_drv`, `mddp`, `wmt_chrdev_wifi`,
`wlan_drv_gen4m_6789` — is built `=m` and lives in the Android
`vendor_dlkm` partition (`/vendor/lib/modules`, a symlink target native
boot does not carry or mount). It is not in Halium's `vendor_boot`
`modules.load`, so under native boot nothing ever loads it: no `wlan0`,
no `phy0`, `lsmod` shows no connsys modules. In the LXC era this was
invisible — Ubuntu Touch's Android second-stage init loaded the whole
`vendor_dlkm` set, and OHOS (sharing the net namespace) just saw a ready
`wlan0`.

Fix — bundle the connsys modules into `vendor_boot` and `insmod` them at
pre-init, **plus** one extra step the audio/GPU stacks don't need:

1. **`kernel/x23/extra-modules.list`** — the 7 connsys modules added so
   `build_kernel.sh` stages their `.ko` into the `vendor_boot` overlay
   (→ `/mnt/kmodules`). `cfg80211.ko` is already in Halium's
   `modules.load`, so it is not bundled.

2. **`vendor/oniro/hybris_generic/etc/init/init.x23.cfg`** — pre-init
   `insmod`s the modules in dependency order. All transitive deps of
   `wmt_drv`/`mddp`/`wlan_drv` (`ccci_md_all`, `ccmni`, the GPU display
   chain, PMIC throttling, …) are already up from the GPU + audio
   blocks, so only the 7 connsys `.ko` are new.

3. **The `wmt_loader` step.** Unlike audio/GPU, `insmod` alone is not
   enough. `wmt_drv.ko`'s `module_init` only registers `/dev/wmtdetect`;
   the real WMT bring-up (`WMT_init()`, which creates `/dev/stpwmt` and
   arms the WiFi/BT function-on path) is deferred to a `/dev/wmtdetect`
   ioctl that Android's `/vendor/bin/wmt_loader` issues from second-stage
   init. Native boot has no second-stage init. Without it,
   `echo 1 > /dev/wmtWifi` fails — `WMT turn on WIFI fail`, connsys
   func-on `opId 32` completion timeout, `RST_FW_DL_FAIL`.

   `wmt_loader` is a dynamically-linked Android binary (needs
   `libcutils.so` + the Android linker namespace), so it cannot run
   under OHOS directly. Instead we ship **`wmtdetect-init`**, a tiny
   pure-C OHOS executable (`launcher/wmtdetect-init.c`, built by
   `launcher/BUILD.gn`, installed to `/system/bin`) that drives the same
   `/dev/wmtdetect` ioctl sequence wmt_loader uses for an integrated-SoC
   connsys chip: `CONNSYS_SOC_HW_INIT` → `GET_SOC_CHIP_ID` (→ `0x6789`)
   → `GET_ADIE_CHIP_ID` (→ `0x6631`) → `SET_CHIP_ID` →
   `DO_MODULE_INIT` (→ kernel `WMT_init()`).

   `init.x23.cfg` runs `wmtdetect-init` **between** the `wmt_drv` insmod
   and the `wmt_chrdev_wifi`/`wlan_drv_gen4m` insmods. This ordering is
   mandatory — loading the wlan modules *before* `WMT_init()` makes the
   connsys WiFi firmware download fail (`RST_FW_DL_FAIL`); loading them
   *after* it succeeds (`wmt call wlan probe ok`, `WMT turn on WIFI
   success`).

   > **The ordering MUST use the `syncexec` init command — not `exec`,
   > and never `exec_start`.** OHOS init implements only `exec` (fork +
   > *no wait*) and `syncexec` (fork + `waitpid`). `exec_start` is an
   > *Android* init keyword; OHOS init does not implement it and
   > silently drops the line. The first cut of this fix used
   > `exec_start /system/bin/wmtdetect-init`, so `WMT_init()` never ran
   > before the WiFi driver loaded — the connsys WiFi function-on then
   > failed its firmware download (`RST_FW_DL_FAIL`) on essentially
   > every boot. `syncexec` makes init block until `wmtdetect-init`
   > exits, guaranteeing `WMT_init()` completes first. This was the
   > single root cause of a long "WiFi flaky / WLAN Operation Error"
   > investigation — it was never flaky hardware, just a dropped init
   > command.

4. **Function-on.** Something has to write `1` to `/dev/wmtWifi` to
   power on the connsys WiFi function (which creates `wlan0`); the OHOS
   WiFi framework does not do this reliably on its own. `wmtdetect-init
   wifi-on` does it, run as the `wmtwifi-on` oneshot init service
   (started by the `boot` job, so it never blocks pre-init). With the
   `syncexec` ordering correct it succeeds on the first attempt; the
   service keeps a retry loop purely as insurance against a transient
   function-on failure.

`init.x23.cfg` also `syncexec`s `/system/bin/rfkill unblock all` after
the wlan modules (covering the freshly-created `phy0`).

### Verified (2026-05-19, on device)

After a clean flash, `wmtwifi-on` powers on the connsys WiFi during the
`boot` stage; `wlan0` comes up and OHOS auto-connects to the saved WPA2
network — `ifconfig wlan0` shows a DHCP lease and `ping 8.8.8.8` returns
0% loss (~17 ms RTT) within ~30 s of boot, reproducibly across reboots.
The Phase 10 HDI services (`wifi_host -i 5`, `wpa_host -i 6`) are
unchanged.

**Lessons:**
- An MTK connsys WiFi/BT chip needs more than its modules loaded — the
  `wmt_loader` chip-detect ioctl that fires the kernel `WMT_init()` is a
  hard requirement, and the wlan modules must load *after* it.
- OHOS init has **no `exec_start`** — only `exec` (async) and `syncexec`
  (synchronous). Any init.cfg line that must complete before later lines
  (here: `WMT_init()` before the WiFi driver insmod) must use
  `syncexec`. An `exec_start` line is silently dropped.

Any future connsys-side bring-up (Bluetooth N9.4, GPS, FM) reuses the
same `wmt_drv` + `wmtdetect-init` foundation.

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

## N9.5 — Audio ✅ (2026-05-18 — fixed + flashed)

The Phase 13B userspace (native ALSA via `audio_host` + libasound,
`audio_alsa/vendor_render.c`) carries over unchanged. **Two separate
things did NOT carry over** — both fixed 2026-05-18, rebuilt and
flashed:

1. **The MT6789/MT6366 audio kernel drivers never loaded** →
   `/proc/asound/cards` showed *no soundcards*, `/dev/snd` held only
   `timer`, `audio_host` had no `primary` adapter.
2. **The AW883xx speaker amp's config firmware was unreachable** → even
   once the card was up, the speaker stayed silent (see "AW883xx
   firmware" below).

Root cause — same class as the Mali GPU (N8.11) and touch (N8.13)
modules: the MTK audio stack is built `=m` and lives in the Android
**second-stage** module set (`/vendor/lib/modules`). It is *not* in
Halium's `vendor_boot` `modules.load`, so under native boot — where OHOS
init replaces Android second-stage init — nothing ever loads it. In the
LXC era this was invisible: OHOS ran inside Ubuntu Touch, whose Android
init loaded the full `/vendor/lib/modules` set before the container
started.

Fix — bundle the audio modules into `vendor_boot` and `insmod` them at
pre-init, exactly like the GPU/touch stacks:

- **`kernel/x23/extra-modules.list`** — 23 audio modules added so
  `build_kernel.sh` stages their `.ko` into the `vendor_boot` overlay
  (→ `/mnt/kmodules`).
- **`vendor/oniro/hybris_generic/etc/init/init.x23.cfg`** — 23 `insmod`
  lines in the pre-init job, in dependency order, plus a
  `write /sys/module/firmware_class/parameters/path /android/vendor/firmware`
  line before them (see "AW883xx firmware" below).

Module set + load order (dep-resolved from `modules.dep` / each module's
`depends` field; all other transitive deps — `clk-common`, `emi*`,
`mtk-mbox`, `aee_aed`, … — are already up via the chainload
`modules.load` + the GPU `insmod` block):

```
nvmem-mt635x-efuse   PMIC efuse nvmem provider; mt6366 codec + mt635x-auxadc
                     defer (-517 "Get efuse failed") without it
mtk-afe-external     SCP/AFE leaf, no deps
scp adsp             SCP + audio-DSP coprocessors
mtk-scp-audiocommon audio_ipi mtk-scp-audio snd-soc-audiodsp-common
mtk-scp-ultra snd-soc-mtk-scp-ultra        ultrasound (proximity) path
rps_perf ccmni ccci_util_lib ccci_auxadc ccci_md_all
                     modem (eccci) — hard symbol deps of snd-soc-mtk-common
snd-soc-mtk-common   MTK ASoC common
mtk-sp-spk-amp smartpa             speaker-amp common + AWINIC AW883xx codec
mt6358-accdet snd-soc-mt6366       PMIC codec (mt6366 = mt6358 family)
snd-soc-mt6789-afe                 MT6789 AFE platform
mtk-btcvsd                         BT-CVSD — registers platform component
                     `18050000.mtk-btcvsd-snd`; the card's btcvsd dai_link
                     references it by name, so the card -517-defers without it
mt6789-mt6366                      machine driver — registers card 0
```

Two non-obvious blockers found while bringing the card up live:

1. **`nvmem-mt635x-efuse`** is a separate `=m` nvmem provider. Without
   it the `mt6358-sound` codec probe and `mt635x-auxadc` both stick at
   `-EPROBE_DEFER` ("Get efuse failed (-517)") — the codec reads PMIC
   trim data from this efuse.
2. **`mtk-btcvsd`** is not a symbol dependency of any audio module, so
   it isn't pulled in by `modules.dep`. But the `mt6789-mt6366` card has
   a `btcvsd` dai_link whose platform component (`18050000.mtk-btcvsd-snd`)
   is registered by `mtk-btcvsd`. `snd_soc_register_card` returns
   `-517` until that component exists. `CONFIG_SND_SOC_MTK_BTCVSD=m` ⇒
   the dai_link is compiled in ⇒ the module is mandatory for the card.

### AW883xx firmware (the "card up but silent" half)

With the card registered, the music app played but the speaker was
silent. The X23 speaker is an AWINIC AW883xx smart amp; its codec
driver (`smartpa.ko`) loads a DSP config blob `aw883xx_acf.bin` via
`request_firmware()` — and **only after that load succeeds does it
create the `aw_dev_0_prof` / `aw_dev_0_switch` kcontrols** that Phase
13B's `vendor_render.c` toggles to power the amp. The kernel searched
`firmware_class.path=/vendor/firmware`, which on OHOS doesn't even
exist; the firmware actually ships at `/android/vendor/firmware/aw883xx_acf.bin`
(Halium's tree, loop-mounted in N8.1). So `request_firmware` hit
`-ENOENT`, the amp was never initialised, and the `aw_dev_0_*` controls
were absent.

Fix: the `init.x23.cfg` `write` line above retargets `firmware_class.path`
at `/android/vendor/firmware`. `aw883xx_load_fw` is async and runs from
the *ASoC codec probe* (when the card binds the codec), so the path is
set well before the lookup. With it, `aw883xx_acf.bin` loads,
`aw_dev_0_*` appear, and `aw883xx_start_pa` succeeds. See N9.1 for the
`firmware_class.path` mechanism + the trailing-newline pitfall.

### Verified

On a clean boot after flashing `super` + `boot_a` + `vendor_boot_a`
(no hand-patching): `firmware_class.path` = `/android/vendor/firmware`,
card 0 `mt6789mt6366` registers with all 45 PCM nodes, the `aw_dev_0_*`
controls exist, `audio_host` loads the `primary` adapter, and
`aw883xx_start_pa: start success` — audible on the X23 speaker
(confirmed by the user playing the music-app demo).

`/dev/snd/*` perms set by N2.6 ueventd. `audio_host` cfg from
`vendor/oniro/hybris_generic/hdf_config/uhdf/device_info.hcs` carries
over unchanged.

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
5. **N9.2 WiFi**: Phase 10's OHOS userspace carries over, but the connsys
   *kernel* stack does not — the `vendor_dlkm` connsys modules must be
   bundled into `vendor_boot` and `insmod`'d at pre-init, and a
   `wmtdetect-init` helper must run the `wmt_loader` chip-detect ioctl
   (kernel `WMT_init()`) between the `wmt_drv` and `wlan_drv` insmods.
6. **N9.10 sharefs**: ship LXC-equivalent bind; port kernel driver in Milestone 5.

## Tasks status

- ✅ **N9.1** — Firmware path: `init.x23.cfg` writes `firmware_class.path=/android/vendor/firmware` at pre-init (2026-05-18) — OHOS `/vendor/firmware` doesn't exist; in-kernel `request_firmware` (AW883xx codec) needs Halium's tree
- ✅ **N9.2** — WiFi: 7 connsys (WMT) kernel modules bundled into
  vendor_boot + insmod'd at pre-init, with the `wmtdetect-init` helper
  running the kernel `WMT_init()` ioctl between the `wmt_drv` and
  `wlan_drv` insmods (2026-05-18); rfkill unblock from N3.3; HDI WPA
  services unchanged from Phase 10. Scan + connect verified on device
- ⏳ **N9.3** — Modem / RIL: deferred to Milestone 5+
- ⏳ **N9.4** — Bluetooth: deferred to post-Milestone-3
- ✅ **N9.5** — Audio: 23 audio-stack kernel modules bundled into vendor_boot
  + insmod'd at pre-init, + `firmware_class.path` retargeted for the AW883xx
  codec firmware (2026-05-18); rebuilt + flashed; speaker audible on clean
  boot. Phase 13B userspace unchanged
- ⏳ **N9.6** — Sensors: deferred
- ✅ **N9.7** — Power: native reboot path works without /ohos-host-action; logind workaround disappears
- ⏳ **N9.8** — Camera: deferred (multi-week project)
- ⏳ **N9.10** — sharefs: short-term bind workaround documented; kernel port deferred to M5

## Next phase entry condition

N10 needs: image artifacts (✅ have boot-ohos.img + system.img + vendor.img + userdata.img), partition map (✅ from N1.1), recovery strategy (next).
