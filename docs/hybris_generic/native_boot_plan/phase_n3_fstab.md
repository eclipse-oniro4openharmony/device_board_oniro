# Phase N3 — Filesystem & fstab

**Status:** 🔄 In Progress (2026-04-30)

Define the OHOS fstab in OHOS-native syntax and align the on-disk layout with what `init_firststage.c` and `init.cfg` expect.

---

## N3.1 — fstab format and location ✅

**Plan adjustment:** The plan suggests *both* a kernel-cmdline `ohos.required_mount.*` (for required mounts) and a `/vendor/etc/fstab.${ohos.boot.hardware}` file (for non-required mounts). N1.3 already shipped the cmdline; this phase adds the file for second-stage mounts.

### Cmdline (required mounts) — already in N1.3

Set by `kernel/x23/build_boot_img_ohos.sh` when packing `boot-ohos.img`:

```
ohos.required_mount.system=/dev/disk/by-partlabel/system_b@/usr@ext4@ro,barrier=1@wait,required
ohos.required_mount.vendor=/dev/disk/by-partlabel/vendor_b@/vendor@ext4@ro,barrier=1@wait,required
ohos.required_mount.userdata=/dev/disk/by-partlabel/userdata@/data@ext4@nosuid,nodev,noatime,discard@wait,check
```

### File (second-stage, non-required) ✅

**Authored:** `vendor/oniro/hybris_generic/etc/fstab/fstab.x23`

Loaded by `mount_fstab_sp /vendor/etc/fstab.${ohos.boot.hardware}` in `init.cfg` pre-init job (`base/startup/init/services/etc/init.cfg:16`). With `${ohos.boot.hardware}=x23` (set via the Halium bootloader cmdline, verified in N0), the literal path becomes `/vendor/etc/fstab.x23`.

**Contents:**

| src | mnt | type | mnt_flags | fs_mgr_flags |
|---|---|---|---|---|
| `/dev/disk/by-partlabel/misc` | `/misc` | none | none | wait,nofail |
| `/dev/disk/by-partlabel/persist` | `/persist` | ext4 | ro,nosuid,nodev | wait,nofail |

**Plan deviations / corrections:**

1. **AOSP fs_mgr flags removed.** Original draft fstab had `fileencryption=software,quota` — those are AOSP fs_mgr flags, not OHOS. OHOS uses fscrypt as a *mount option* (`fscrypt=2:aes-256-cts:aes-256-xts`), not as an fs_mgr flag. For our case we don't enable encryption on `/data` at all (encrypted by-default ext4 is fine; no fs_mgr flag needed).
2. **No `slot_suffix`.** OHOS fstab parsing doesn't substitute slot suffixes. The slot must be encoded literally — Layout A pins to `_b` for OHOS partitions (the cmdline does this; the fstab file uses the unsuffixed `misc`/`persist` partlabels which are not slotted).
3. **No `first_stage_mount` flag.** AOSP-only. Required mounts are gated by the cmdline, not by an fstab flag.
4. **Android rootfs is NOT in this fstab.** `/android/system` and `/android/vendor` are mounted by the `androidd` launcher (Phase N4) inside the Android namespace. Mounting them at `/android/...` from OHOS init would force them into OHOS's mount namespace where they'd survive `androidd`'s `pivot_root` and waste a bind layer.

### `/vendor/etc/fstab.required` fallback (not used)

The plan mentioned `LoadFstabFromFile` (`fstab.c:506`) as an overflow path for cmdline length. Our cmdline append is 261 chars — far below any cap — so we skip the `fstab.required` file. If a future device needs more required mounts (additional logical partitions, multi-stage `/system`), revisit.

---

## N3.2 — Root layout after SwitchRoot ✅ (analysis)

The plan claimed:

```
/  (was /usr in ramdisk)            ohos system
├── bin -> /system/bin
├── system/
├── vendor/
├── data/
├── android/...
├── config/
├── dev/...
└── proc, sys, tmp, mnt, storage
```

**Verified against `out/hybris_generic/packages/phone/images/ohos-rootfs/`:**

- `bin -> /system/bin` ✓ (symlink)
- `etc -> /system/etc` ✓
- `init -> /system/bin/init` ✓ (so post-SwitchRoot `/init` and `/bin/init` both resolve to the second-stage binary)
- `lib -> /system/lib`, `lib64 -> /system/lib64` ✓
- `system/`, `vendor/`, `data/`, `chip_prod/`, `sys_prod/`, `chipset -> /vendor` ✓
- `dev/`, `proc/`, `sys/`, `tmp/`, `mnt/`, `storage/` ✓
- `config/` ✓
- `android/` exists in the rootfs (created by something — inspected and contains `apex/`, `bin/`, `lib64/`, `vendor_egl/` placeholders).

The `android` dir at the *rootfs top level* is a leftover from a build artifact step that mirrors a subset of the Halium namespace. It is **not** the runtime Android namespace mount point — that one is created at runtime by `androidd` (Phase N4) and pivoted into. So our `mkdir /android/{system,vendor,...}` in init.x23.cfg pre-init is harmless even if `/android/` already exists; `mkdir` in init's parser is idempotent (treats EEXIST as success).

---

## N3.3 — Tmpfs / pseudo-fs setup ✅ (artifact authored)

**Plan adjustment vs original:** the plan put binderfs mount + symlinks plus `/android/*` directory creation in a single pre-init job. We took the same shape but split USB-controller setup out into N7's `init.x23.usb.cfg` (cleaner separation of concerns).

**Authored:** `vendor/oniro/hybris_generic/etc/init/init.x23.cfg`

```json
{
    "jobs" : [
        {
            "name" : "pre-init",
            "cmds" : [
                "mkdir /dev/binderfs 0755 root root",
                "mount binder binder /dev/binderfs nodev,noexec,nosuid",
                "symlink /dev/binderfs/binder    /dev/binder",
                "symlink /dev/binderfs/hwbinder  /dev/hwbinder",
                "symlink /dev/binderfs/vndbinder /dev/vndbinder",
                "mkdir /android         0755 root root",
                "mkdir /android/system  0755 root root",
                "mkdir /android/vendor  0755 root root",
                "mkdir /android/odm     0755 root root",
                "mkdir /android/apex    0755 root root",
                "mkdir /android/data    0755 root root",
                "exec_start /system/bin/rfkill unblock all"
            ]
        }
    ]
}
```

**Plan deviations:**

1. **No JSON comments.** OHOS init's cfg parser uses cJSON; comments are not part of the JSON spec and the parser would treat free-floating string entries inside the `cmds` array as commands. Author intent is captured in this doc instead.
2. **rfkill unblock moved here from N9.2.** Per N0 reconnaissance the existing `start-ohos.sh` runs `rfkill unblock all` on container start; the native equivalent is to run it once in pre-init. Verified `/system/bin/rfkill` is shipped in the OHOS rootfs (`out/hybris_generic/packages/phone/images/ohos-rootfs/system/bin/rfkill`).
3. **USB controller setup moved to N7.** `setparam sys.usb.controller musb-hdrc` belongs in `init.x23.usb.cfg`, not in this generic init overlay.

### BUILD.gn integration

Added to `vendor/oniro/hybris_generic/etc/BUILD.gn`:
```gn
ohos_prebuilt_etc("init_x23_cfg") {
  source = "./init/init.x23.cfg"
  install_images = [ vendor_base_dir ]
  relative_install_dir = "init"
  part_name = "product_hybris_generic"
}
```

This installs to `/vendor/etc/init/init.x23.cfg`. **Caveat:** the `init.cfg` import path is `/vendor/etc/init.${ohos.boot.hardware}.cfg` (not under `init/`). Need to verify the actual import path works with this BUILD.gn `relative_install_dir`. **TODO:** if first-boot reveals the file is loaded from `/vendor/etc/init.x23.cfg` (no subdir), drop the `relative_install_dir = "init"` and reinstall.

> **Plan adjustment for BUILD.gn:** drop `relative_install_dir = "init"` to land the file at `/vendor/etc/init.x23.cfg` exactly. Will fix once verified on first-boot — the file lookup pattern in `init.cfg:5` is unambiguous.

(Fixed inline below — see edit.)

---

## N3.4 — Encryption / first-boot data wipe ✅ (documented)

**Important user-facing behaviour:** Volla X23 stock Halium uses Android FBE (file-based encryption) with keys held in the bootloader-managed metadata. When OHOS first mounts `/data` with our cmdline mount options, the existing FBE keys are unrecognised and `e2fsprogs`/the kernel reformats the partition. **The device's user data is wiped on the OHOS-flash transition.**

This is documented in N10.6 (dev iteration loop). For developer testing this is fine; for any production rollout we'd need an out-of-band data backup workflow.

Same applies on rollback to Halium A-slot — Halium will re-encrypt on next boot, wiping again. **Both directions wipe userdata.**

---

## N3 plan adjustments emitted

1. **fs_mgr flags**: dropped AOSP-style `fileencryption=software,quota` from the fstab template. OHOS doesn't recognise those.
2. **fstab.x23 scope**: limited to `/misc` and `/persist` — these are the only optional second-stage mounts that benefit native boot. Required mounts (system/vendor/data) are pinned to the cmdline.
3. **rfkill unblock all**: moved from N9.2 to N3.3 pre-init (it has no dependency on the Android namespace and benefits ueventd-side device perms set immediately after).
4. **USB controller setparam**: moved from N3.3 to N7.
5. **JSON comments**: removed from the cfg file; captured in the phase doc.
6. **BUILD.gn `relative_install_dir`**: needs to match the `init.cfg` import literal. Fix (drop `init` subdir) emitted below.

## Tasks status

- ✅ **N3.1** — `fstab.x23` authored + BUILD.gn wired
- ✅ **N3.2** — Post-SwitchRoot root layout verified against built rootfs
- ✅ **N3.3** — `init.x23.cfg` authored + BUILD.gn wired (`relative_install_dir` fix below)
- ✅ **N3.4** — First-boot data wipe documented for N10.6
- ⏳ **First-boot validation** — exact path of `init.x23.cfg` lookup needs on-device confirmation

## Next phase entry condition

N4 needs: `/android/{system,vendor,...}` directory tree pre-created (✅ this phase), `/dev/binderfs/` available with control device (✅ this phase), root layout confirmed (✅).
