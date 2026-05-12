# Phase N10 — Flash Tooling, Recovery & Dual-Boot

**Status:** ✅ Source-side complete (2026-04-30); on-device flash gated by user approval

Make the experiment safe. Runs in parallel with N1–N9; **gates the first reflash.**

---

## N10.1 — Flash procedure ✅

**Authored:** `device/board/oniro/hybris_generic/utils/host/flash-native.sh` (executable, syntax-validated).

The script:
1. Auto-detects fastboot vs adb-only mode.
2. Detects active slot (`fastboot getvar current-slot` or `getprop ro.boot.slot_suffix`).
3. Targets the **inactive** slot (so Halium stays bootable on the active slot).
4. Flashes `boot-ohos.img`, `system.img`, `vendor.img`. Optionally `vendor_boot`/`dtbo` (Halium versions, untouched by N1).
5. **fastboot path:** `fastboot set_active <inactive>` + reboot.
6. **adb-dd path:** dd-over-adb for boot-ohos.img/system.img/vendor.img; user manually `adb reboot bootloader` + `fastboot set_active`.

### Fastboot prerequisites

The Volla X23 ships unlocked. `fastboot getvar unlocked` should return `unlocked: yes` once the device is in fastboot mode (`adb reboot bootloader`). If verified-boot rejects unsigned `boot-ohos.img`, append `--disable-verity --disable-verification` to the boot.img flash command — the script does this for vbmeta_b automatically.

### dd-over-adb fallback caveat

The `adb shell dd` path can write `boot_<TARGET>` (it's a top-level partition under `/dev/disk/by-partlabel/`), but cannot directly write `system_<TARGET>` or `vendor_<TARGET>` *inside* the super partition unless those logical partitions are dm-mapped. On a Halium-active boot, only slot _a's logical partitions are dm-mapped (`/dev/mapper/system_a`, `vendor_a`); slot _b's logical partitions need fastboot's super-resizer to land. So the dd path is reliable for `boot_<TARGET>` only; `system_b`/`vendor_b` typically require fastboot.

The script falls back gracefully and prints actionable error messages.

---

## N10.2 — A/B dual-boot ✅

| Slot | Contents | Purpose |
|---|---|---|
| _a | Halium 12 / Ubuntu Touch (untouched) | Recovery / fallback |
| _b | OHOS native (the experiment) | Daily driver candidate |

Boot sequence:
- **Default state after first flash:** `set_active b` boots OHOS.
- **Roll back:** `set_active a` → reboot, lands in Halium.
- **Volume-down at power-on** → MTK BROM USB DL mode (last-resort recovery; needs `mtkclient`).

### Per-slot deltas

| Item | _a (Halium) | _b (OHOS native) |
|---|---|---|
| boot.img kernel | Halium-built | Halium-built (same kernel) |
| boot.img ramdisk | Halium init | OHOS init_early |
| boot.img cmdline | empty | `ohos.required_mount.system=…@/usr@…` etc. |
| vendor_boot.img | Halium (cmdline `bootopt=…systempart=… hardware=x23`) | Halium (untouched — same hardware= line works for both) |
| system | Halium android system | OHOS system.img (2 GB) |
| vendor | Halium android vendor | OHOS vendor.img (256 MB) |
| userdata | shared, formatted on each side's first boot | shared (one-way reformat) |

Verified-boot enforcement: if vbmeta is enforced, Halium ignores it (Ubuntu Touch flashes never check) but our fastboot flash needs `--disable-verity --disable-verification` for `vbmeta_b`. The script handles this.

---

## N10.3 — Recovery image ✅ (analysis)

`out/hybris_generic/packages/phone/images/updater.img` is built and 21 MB. Investigate post-flash whether `fastboot flash recovery_b updater.img` boots the OHOS updater. If not, **the Halium A-slot is the recovery** — boot it and re-flash.

For Milestone 1–4, treating the Halium A-slot as the recovery is sufficient. Skip flashing `recovery_b` to avoid breaking what works.

---

## N10.4 — UART debug ✅ (deferred)

Volla X23 has UART test pads per the mainline schematic. Document pinout when a developer pries open a unit; until then, pstore is the only post-mortem channel.

---

## N10.5 — pstore / ramoops ✅

**Plan adjustment from N0 reconnaissance:** the Halium kernel **already has** all the pstore configs we need:

```
$ cat /proc/config.gz | gunzip | grep -E "PSTORE|RAMOOPS"
CONFIG_PSTORE=y
CONFIG_PSTORE_DEFLATE_COMPRESS=y
CONFIG_PSTORE_CONSOLE=y
CONFIG_PSTORE_PMSG=y
CONFIG_PSTORE_RAM=y

$ cat /proc/cmdline | tr ' ' '\n' | grep ramoops
ramoops.mem_address=0x48090000
ramoops.mem_size=0xe0000
ramoops.pmsg_size=0x10000
ramoops.console_size=0x40000
```

**Plan adjustment:** the original plan called for `ramoops.patch` adding the DT region. Not needed — the bootloader cmdline already injects it. **Drop the ramoops DT patch from N10.5 work.**

The OHOS-side change required is just to mount `/sys/fs/pstore`:

```
"mount pstore none /sys/fs/pstore nodev,nosuid,noexec"
```

(Add to `init.x23.cfg` pre-init when first-boot reveals `/sys/fs/pstore` isn't auto-mounted. **Don't add speculatively** — the kernel may auto-mount via `pstore_set_kmsg_bytes` or systemd-style; verify on first boot.)

After a panic, logs land at `/sys/fs/pstore/console-ramoops-0` on the next boot.

---

## N10.6 — Dev iteration loop ✅

For native bring-up, the slow loop is:
1. Build OHOS (`./build.sh --product-name hybris_generic --ccache`)
2. Build OHOS boot.img (`./kernel/x23/build_boot_img_ohos.sh`)
3. Flash to slot _b (~3 min over fastboot)
4. Reboot (~1 min)
5. Lose userdata each time (one-way reformat from Halium FBE → OHOS plain ext4)

Tight iteration paths (mirrors what we use today):

- **Kernel/cmdline/ramdisk only:** `fastboot flash boot_b boot-ohos.img` (~10 s).
- **OHOS service iteration:** once OHOS boots, `hdc shell mount -o remount,rw /` and push deltas. Same loop as today's LXC build, but over hdc instead of `adb shell lxc-attach`. ~5 s per service.
- **Userdata preservation:** Once OHOS has formatted `/data` once with its layout, subsequent OHOS boots preserve it. Reformat only when fscrypt config changes. The flash script does NOT wipe userdata by default.

---

## N10 plan adjustments emitted

1. **Ramoops DT patch dropped** — Halium kernel cmdline already reserves the region; OHOS kernel inherits the same cmdline via vendor_boot. No new patch needed.
2. **vbmeta disable in fastboot path** — added `--disable-verity --disable-verification` to the `fastboot flash vbmeta_b` step (best-effort with `|| echo "(skipped)"`).
3. **dd path covers `boot_<TARGET>` only**, NOT `system_<TARGET>`/`vendor_<TARGET>` (those need fastboot's super-resizer). Documented in script error messages.
4. **Recovery: Halium A-slot is the recovery** for Milestones 1–4. Skip flashing `recovery_b` until the OHOS updater is verified bootable.
5. **pstore mount** required in `init.x23.cfg` if not auto-mounted; verify on first boot before adding.

## Tasks status

- ✅ **N10.1** — `flash-native.sh` authored + executable + syntax-clean
- ✅ **N10.2** — A/B strategy documented (Halium on _a, OHOS on _b)
- ✅ **N10.3** — Recovery: A-slot Halium is the recovery; updater.img path deferred
- ⏳ **N10.4** — UART debug pads documented when needed (deferred)
- ✅ **N10.5** — Pstore: kernel cfg already correct in inherited Halium config; DT patch unnecessary
- ✅ **N10.6** — Dev loop documented

## First-flash checklist

When the user is ready to flash:

1. **Backup** any data on the device — userdata reformats on first OHOS boot.
2. Confirm Halium A-slot is bootable (`adb shell uname -a` returns Halium kernel).
3. Build OHOS images:
   ```
   sudo docker exec -u root -w /home/openharmony/workdir 8f7084d45c89 \
       ./build.sh --product-name hybris_generic --ccache
   ./device/board/oniro/hybris_generic/kernel/x23/build_boot_img_ohos.sh
   ```
4. Reboot to fastboot:
   ```
   adb reboot bootloader
   ```
5. Run flash script:
   ```
   ./device/board/oniro/hybris_generic/utils/host/flash-native.sh
   ```
6. Device reboots into OHOS slot _b. Confirm via `hdc list targets`.
7. **If OHOS fails to boot:** `fastboot set_active a` from fastboot mode (volume-down + power on the X23).

This checklist is the *gate* between source-side work (Phases N1–N9) and on-device validation (Milestones 1–4). Source side is now complete.
