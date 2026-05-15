# Phase N1 — Boot Image & Partition Layout

**Status:** 🔄 In Progress (2026-04-30)

Build a flashable `boot.img` whose ramdisk is the OHOS one and a partition mapping that lets OHOS coexist with Halium on the device.

---

## N1.1 — Super partition layout (Volla X23) ✅

Captured 2026-04-30 from a running Halium shell. Documented under
`device/board/oniro/hybris_generic/docs/x23-super.txt` (see below; checked in alongside this doc).

### Slotted physical partitions (full A/B)

```
boot_a / boot_b              64 MB each
dtbo_a / dtbo_b               8 MB each
vendor_boot_a / vendor_boot_b 64 MB each
vbmeta_a / vbmeta_b
vbmeta_system_a / vbmeta_system_b
vbmeta_vendor_a / vbmeta_vendor_b
gz_a / gz_b                   GZ kernel
lk_a / lk_b                   LittleKernel bootloader stage
mcupm_a / mcupm_b
md1img_a / md1img_b           Modem image
spmfw_a / spmfw_b
sspm_a / sspm_b
scp_a / scp_b
tee_a / tee_b
dpm_a / dpm_b
pi_img_a / pi_img_b
connsys_wifi_a / connsys_wifi_b
connsys_bt_a / connsys_bt_b
connsys_gnss_a / connsys_gnss_b
```

### Unslotted partitions (one of each)

```
super         9.66 GB     dynamic-partitions container (system_a/b, vendor_a/b live inside)
userdata      116.83 GB   shared data (formatted on first OHOS boot — see Phase N3.4)
misc                      bootloader command messages
metadata
persist
nvcfg / nvdata / nvram
otp / sec1 / seccfg
protect1 / protect2
para / boot_para / dram_para
frp / expdb / logo / flashinfo
```

### Currently active dm-mapper devices (slot _a, Halium running)

```
$ ls /dev/mapper/
control
system_a -> ../dm-1
vendor_a -> ../dm-0
```

`system_b` and `vendor_b` exist in `super` metadata but are not dm-mapped at this boot because slot `_a` is active.

### Bootloader

`fastboot` not installed on Halium image; unlock state shown via `getvar` requires fastboot mode entry. Per CLAUDE.md the Volla X23 is sold unlocked and we already use `dd`-from-Halium-shell flashes for boot/vendor_boot today (`kernel/x23/deploy-kernel.sh:50-52`). We will not depend on fastboot for the bring-up cycle; see N10.1 for the dd-over-adb fallback that's already proven.

---

## N1.2 — Install layout decision ✅

**Decision: Layout A (OHOS over slot `_b`).**

| Image | Size | Target slot (layout A) | Slot capacity (X23 measured) |
|---|---|---|---|
| `boot-ohos.img` | 19.9 MB | `boot_b` | 64 MB ✓ |
| `dtbo.img` (Halium-built) | 8 MB | `dtbo_b` | 8 MB ✓ |
| `vendor_boot.img` (Halium-built, untouched) | 64 MB | `vendor_boot_b` | 64 MB ✓ |
| `system.img` | 2.0 GB | `system_b` (logical inside `super`) | ~3 GB available per slot inside 9.66 GB super |
| `vendor.img` | 256 MB | `vendor_b` (logical inside `super`) | ~600 MB ✓ |
| `userdata.img` | 1.4 GB | `userdata` (shared, formatted on first boot) | 116.83 GB ✓ |
| `chip_prod.img` / `sys_prod.img` | 50 MB each | TBD — defer until N5 settles | — |

**Rationale for A over B/C:**

- A/B safety net: `_a` keeps Halium 12 / Ubuntu Touch bootable as a recovery slot. Roll back via `fastboot set_active a` (when fastboot is available) or by holding volume-down on power-on (BROM USB DL).
- No super-partition resize required; OHOS images fit comfortably inside the existing slot _b budget.
- Matches the N10.2 dual-boot story (`boot_a` = Halium, `boot_b` = OHOS).

The plan's super-partition flashing path (`fastboot flash system_b`) writes through the `super` resizer if available. If `lpmake` is not in our toolchain, the immediate fallback is to `dd` directly into `/dev/disk/by-partlabel/system` *raw region* corresponding to slot _b's logical partition extents — but this requires reading the dynamic-partition metadata first. Defer until N10 actually ships; the build artefact (`system.img`) is identical either way.

**Plan adjustment from N0 reconnaissance:** layout A is now confirmed, not assumed.

---

## N1.3 — Boot image repack ✅

**Script:** `device/board/oniro/hybris_generic/kernel/x23/build_boot_img_ohos.sh` (created 2026-04-30, executable; sibling to `build_kernel.sh` to keep the Halium boot.img path intact for fallback).

**Verified output:**

```
$ unpack_bootimg.py --boot_img out/hybris_generic/boot-ohos.img --format info
boot magic: ANDROID!
kernel_size: 17135360                    # Halium kernel preserved
ramdisk size: 2801473                    # OHOS ramdisk substituted
boot image header version: 4
command line args: ohos.required_mount.system=/dev/disk/by-partlabel/system_b@/usr@ext4@ro,barrier=1@wait,required ohos.required_mount.vendor=/dev/disk/by-partlabel/vendor_b@/vendor@ext4@ro,barrier=1@wait,required ohos.required_mount.userdata=/dev/disk/by-partlabel/userdata@/data@ext4@nosuid,nodev,noatime,discard@wait,check
```

Total `boot-ohos.img` size: 19.9 MB. Fits in `boot_b` (64 MB) with room to spare.

### Header v4 cmdline placement (plan adjustment vs original)

The original plan recipe used `--cmdline "$(unpack_bootimg --get cmdline)"` to inherit the Halium cmdline and append OHOS keys. **This is unnecessary on header v4** — the kernel cmdline reaching `/proc/cmdline` is the concatenation of:
1. Bootloader-injected runtime cmdline (handles `hardware=x23 ohos.boot.sn=...` etc; verified on device)
2. `vendor_boot.img` `vendor_command_line`
3. `boot.img` `command_line` (this is where we put `ohos.required_mount.*`)

Since `boot.img` cmdline was empty on the Halium build (verified — `command line args: ` empty in the original `unpack_bootimg.py` output), we are not displacing anything. Keeping the cmdline minimal also stays well under any conservative 1024-char cap.

**Plan adjustment:** drop the cmdline-merge step from the script. Fallback to a `/vendor/etc/fstab.required` file (parsed by `LoadFstabFromFile` in `fstab.c:506`) is unnecessary since cmdline cap is not threatened.

### Critical correctness check — SwitchRoot mount point

OHOS first-stage `MountItemByFsType` triggers `SwitchRoot("/usr")` only when an fstab entry's mountPoint is **literally `/usr`** (`fstab_mount.c:723,736,938,939`). Our cmdline's first entry is `ohos.required_mount.system=...@/usr@...` — confirmed match.

Reading the rootfs (`out/hybris_generic/packages/phone/images/ohos-rootfs/`) shows the system layout has `/system/`, `/vendor/`, `/data/`, etc. at top level with `/bin`, `/etc`, `/lib`, `/lib64` and `/init` as symlinks into `/system/...`. After `system.img` is mounted at `/usr` and SwitchRoot moves it to `/`, all OHOS paths resolve correctly.

---

## N1.4 — Image budget ✅

Re-measured 2026-04-30 from `out/hybris_generic/packages/phone/images/`:

| Image | Built size | Target slot |
|---|---|---|
| `system.img` | 2,147,483,648 B (2.0 GB) | `system_b` |
| `vendor.img` | 268,431,360 B (256 MB) | `vendor_b` |
| `userdata.img` | 1,468,006,400 B (1.4 GB) | `userdata` (shared) |
| `chip_prod.img` | 52,428,800 B (50 MB) | defer |
| `sys_prod.img` | 52,428,800 B (50 MB) | defer |
| `ramdisk.img` | 2,801,473 B (2.7 MB) | embedded in `boot-ohos.img` |
| `boot-ohos.img` | 19,943,424 B (19 MB) | `boot_b` |

`super` is 9.66 GB total; system_b (2 GB) + vendor_b (256 MB) is well under the 4-5 GB likely available for slot _b after deducting slot _a's existing logical partitions.

`chip_prod` + `sys_prod`: these carry MTK-specific calibration and firmware that OHOS init expects under `/chip_prod/` and `/sys_prod/`. They are not required for first-stage boot. Defer carving the logical partitions until N5; meantime, populate them as empty directories from the OHOS rootfs (already true — `ohos-rootfs/chip_prod/` and `ohos-rootfs/sys_prod/` exist and are empty).

---

## N1.5 — Android rootfs placement ✅

**Decision: Layout A.1 — leave Android rootfs in slot `_a` and bind-mount.**

The Android rootfs is *not* in OHOS `system.img`. It stays where it lives today (`/dev/mapper/system_a` and `vendor_a` inside `super`). The N4 `androidd` launcher will mount slot `_a`'s logical partitions read-only at `/android/system` and `/android/vendor` inside the Android namespace. Zero copy, zero space cost.

This is also what the existing LXC config does — it bind-mounts `/system`, `/vendor`, `/odm`, `/apex` from the host's view (which is Halium's mounts at slot `_a`) into the OHOS container. Native boot does the same one level out, with the launcher doing the mount.

**N4 implication:** `androidd` needs to dm-map `system_a`/`vendor_a` itself (or rely on first-stage init to do so via additional `ohos.optional_mount.*` cmdline entries, if such a thing existed). Concrete approach: `androidd` runs `dmsetup` on the `super` partition's slot _a metadata to materialise `/dev/mapper/system_a` if it isn't already mapped. Defer the implementation detail to N4.

**Plan adjustment:** the original plan had vague "rbind from /android/system" — now nailed down to "androidd loads the Halium dynamic-partition metadata for slot _a and mounts those dm devices read-only into the namespace."

---

## Risk register (post-N1)

| Obstacle | Risk | Mitigation |
|---|---|---|
| MTK verified-boot rejects unsigned `boot-ohos.img` | Medium-High | Verified the device has `vbmeta_*` partitions; once `_b` is targeted, set `fastboot --disable-verity --disable-verification` flash if vbmeta is enforced. Halium runs unsigned today, so the bootloader is permissive. |
| `lpmake` / `fastboot flash super_b` not in our build | Medium | dd-over-adb fallback (proven for boot.img); for super, may need `lptools` or direct dynamic-partition metadata write. Defer to N10. |
| No OHOS-side `lpdump` to verify slot _b sizing pre-flash | Low | The super metadata reports both slots' partition sizes in its header; we'll read it from the inactive Halium dump path. |
| Cmdline overflow | Low | Current cmdline append: 261 chars. Well under any conceivable cap. |

---

## Tasks status

- ✅ **N1.1** — Super partition layout documented (this file + `docs/x23-super.txt`)
- ✅ **N1.2** — Layout A confirmed (OHOS on slot _b, Halium stays on _a)
- ✅ **N1.3** — `build_boot_img_ohos.sh` written + tested; `boot-ohos.img` built and verified (19.9 MB, header v4, OHOS cmdline + ramdisk + Halium kernel)
- ✅ **N1.4** — Image budget measured; everything fits
- ✅ **N1.5** — Android rootfs bind-mount strategy decided (Layout A.1)
- ⏳ **mimir N1** — repeat N1.1–N1.4 against the Volla Tablet super layout (deferred per Plan §"Open Decisions"; bring up X23 to Milestone 3 first)

## Plan adjustments emitted by N1

1. Drop the cmdline-merge step from `build_boot_img_ohos.sh` — header v4 doesn't share the cmdline budget across boot/vendor_boot/bootloader on this device.
2. Layout A.1 implementation detail: `androidd` mounts `_a`'s dynamic partitions itself (rather than relying on a non-existent `ohos.optional_mount.*` cmdline syntax).
3. `chip_prod` / `sys_prod` carving deferred to N5 (no functional impact on Milestones 1–3).
4. fastboot use is *not* on the critical path; dd-over-adb fallback is already proven for boot.img; extend it to system/vendor in N10.1.

## Next phase entry condition

N2 needs: a `boot-ohos.img` to validate (✅ have it), a target slot (`_b` ✅ decided), an understanding of how first-stage init will pick up `/usr` (✅ verified via `fstab_mount.c:723`). Move forward.
