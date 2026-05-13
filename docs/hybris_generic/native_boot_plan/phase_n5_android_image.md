# Phase N5 — Halium HAL Image (Native Boot)

**Status:** ✅ Source-side complete (2026-05-12 evening).  Authored 2026-05-12, then revised the same day after empirical discovery that the bootstrap zip's `system_a` slot is allocated but zeroed.  Build + on-device verification deferred to the consolidated bring-up task.

> **Goal.** Get Halium 12's Android `system` and `vendor` content onto the device, mounted read-only at `/android/system` and `/android/vendor` inside the native OHOS root, so libhybris's hard-coded `/android/vendor/lib64/{egl,hw}/...` lookups resolve and `androidd` (Phase N4) can launch the HAL services.

---

## N5.0 — Why this phase needs a rewrite

The original plan (pre-chainload) assumed Halium's `system_a` and `vendor_a` would still be present on the device, in their original slot, after the OHOS flash. **That assumption no longer holds.** Our Phase N11 flow ships a custom `super.img` (built by `kernel/x23/build_super_img.sh`) containing only:

```
system_a       — OHOS system
vendor_a       — OHOS vendor
sys_prod_a     — OHOS sys_prod
chip_prod_a    — OHOS chip_prod
```

Halium's original `system_a` and `vendor_a` are overwritten during the first `fastboot flash super`. There is no other on-device source for them — the kernel/Halium build tree at `kernel/linux/volla-vidofnir/` only produces the kernel + boot.img + vendor_boot.img; it never had the Halium ext4 images.

**Conclusion:** we must source Halium's `system_a` + `vendor_a` ourselves and bake them into our `super.img` as additional logical partitions. The launcher then bind-rebinds them post-pivot, and composer_host's `/android/vendor/lib64/hw/...` lookups resolve.

---

## N5.1 — Source(s): TWO blobs, not one (revised 2026-05-12 PM)

> **Empirical correction.**  The initial draft assumed both `system_a`
> and `vendor_a` came from the UBports bootstrap zip.  Extracting it on
> 2026-05-12 showed `system_a`'s 8.1 GB slot is allocated but
> **zero-filled** — `xxd halium_system_a.img | head` returns all
> zeros at every offset checked (0, 1 KiB, 1 MiB, 1 GiB, 8 GiB).  The
> UBports installer flow expects the empty system_a slot to be
> populated by the recovery-mode `systemimage:install` step, which
> writes Halium's Android `/system` content as the file
> `system/var/lib/lxc/android/android-rootfs.img` on the Ubuntu Touch
> userdata.  So we source the two halves separately:

### Source 1 — Bootstrap zip → `halium_vendor_a.img`

The UBports installer for Volla X23 (`vidofnir`/`vidofnir_esim`):

- **URL:** `https://volla.tech/filedump/volla-vidofnir-12.0-ubports-installer-bootstrap-v3.zip`
- **Size:** ~478 MB
- **SHA256:** `da18b5498ebae0267be894fff73bfd629be73967cf33f071d571ffc3ef46ce97`
- **Referenced from:** `https://raw.githubusercontent.com/ubports/installer-configs/master/v2/devices/vidofnir_esim.yml`

Inside it, `super.img` (sparse Android image, 926 MB → 9 GB raw via
`simg2img`) contains:

| Partition | Size  | What it is |
|---|---:|---|
| `vendor_a` | 930 MB | **The MTK 6789 Halium 12 vendor partition** — Mali EGL (`/vendor/lib64/egl/libGLES_mali.so`), HAL service binaries (`/vendor/bin/hw/android.hardware.graphics.composer@2.3-service`, etc.), `/vendor/etc/init/*.rc` files for HAL services.  This is what we want. |
| `system_a` | 8.7 GB allocated, **all zeros** | Empty placeholder — installed from the system-image flow on a real UBports install, not from this zip. |

We extract **only** `vendor_a`, rename to `halium_vendor_a.img`.

### Source 2 — UBports system-image stable channel → `halium_system_a.img`

The Halium 12 Android `/system` tree is delivered as a 506 MB ext4
image embedded inside the per-version `device-<sha>.tar.xz` tarball
under `system/var/lib/lxc/android/android-rootfs.img`.  Pin to vidofnir_esim
stable v12 (latest as of 2026-05-12):

- **Channel index:** `https://system-image.ubports.com/20.04/arm64/android9plus/stable/vidofnir_esim/index.json`
- **Tarball:** `https://system-image.ubports.com/pool/device-37ea68e425f921a10982a5cbd36345dde820b239d0c3962e2ec75adea6759e17.tar.xz`
- **Tarball size:** 140 MB
- **Tarball SHA256:** `62306fcc600d7062a9d0e65e60c381c4605fdace17945565f9b0365c4a89b788`
- **Inner android-rootfs.img:** 506 MB ext4, mounts root → `/system/bin/init`,
  `/system/bin/hwservicemanager`, `/system/bin/servicemanager`,
  `/system/lib64/` with bionic + Android 12 core libs, `/system/etc/init/*.rc`.
  The `/init` symlink → `/system/bin/init` makes it directly executable as
  Android stage-2 init.

We rename to `halium_system_a.img`.

To bump the pin: `curl -fsSL https://system-image.ubports.com/20.04/arm64/android9plus/stable/vidofnir_esim/index.json | jq '.images[-1].files'` and update `DEVICE_TAR_PATH` + `DEVICE_TAR_SHA256` in `utils/host/pull-halium-blobs.sh`.

> Sourcing both from public Volla / UBports URLs (one-time, host-side)
> is cleaner than `adb pull /dev/disk/by-partlabel/system_a` from a
> Halium-running device, because (a) it requires no live X23, (b)
> SHA256 pins tie the blobs to specific upstream releases, (c) it
> produces an auditable provenance for the blob in our build pipeline.

### Host script: `device/board/oniro/hybris_generic/utils/host/pull-halium-blobs.sh` ✅

Authored and verified end-to-end on 2026-05-12.  Downloads both
sources, verifies SHA256s, extracts vendor_a via our shipped
`lpunpack.py`, extracts android-rootfs.img via `tar -xJf`.

```bash
bash device/board/oniro/hybris_generic/utils/host/pull-halium-blobs.sh
# Halium blobs ready:
# -rw-rw-r-- 1 mrfrank 441M halium_system_a.img
# -rw-rw-r-- 1 mrfrank 930M halium_vendor_a.img
```

Dependencies: `curl`, `unzip`, `tar`, `xz`, `sha256sum`, `simg2img`
(`apt install android-sdk-libsparse-utils`), `python3` (stdlib only).

### `.gitignore` entry ✅

Added to `device/board/oniro/hybris_generic/.gitignore`:

```
halium-blobs/
```

Blobs are ~1.5 GB combined and Volla-licensed — SHA256 pins in the
script provide reproducibility without checking the bytes into git.

---

## N5.2 — `lpunpack`: shipped in-tree as `utils/host/lpunpack.py` ✅

Rather than depending on an out-of-tree AOSP build of `lpunpack` (the
binary is not in `kernel-build-tools/linux-x86/bin/`), we ship a
pure-Python (stdlib only) implementation at
`device/board/oniro/hybris_generic/utils/host/lpunpack.py` (~145 LOC).

Supports LP metadata v1.0 through v1.2 with single-block-device,
linear-extent layouts — what the UBports bootstrap super.img uses.
Geometry magic `0x616c4467`, header magic `0x414c5030`, sector size
512.  Two operations:

```
python3 lpunpack.py <super.img>                             # list parts
python3 lpunpack.py --partition vendor_a <super.img> <dir>  # extract
```

If we ever hit a v1.3+ LP super or a multi-block-device layout, fall
back to building AOSP's `system/extras/partition_tools/lpunpack`.

---

## N5.3 — Bake into our `super.img`

Extend `device/board/oniro/hybris_generic/kernel/x23/build_super_img.sh` to add the Halium partitions, **only when the blobs are present**. Builds without the blobs (pure OHOS-only super) still work — this is important for bring-up developers who don't need graphics yet.

### Patch to `build_super_img.sh`

```bash
HALIUM_SYS="$OHOS_ROOT/device/board/oniro/hybris_generic/halium-blobs/halium_system_a.img"
HALIUM_VEN="$OHOS_ROOT/device/board/oniro/hybris_generic/halium-blobs/halium_vendor_a.img"

LPMAKE_EXTRA=()
if [[ -f "$HALIUM_SYS" && -f "$HALIUM_VEN" ]]; then
    HS_SZ=$(stat -c %s "$HALIUM_SYS")
    HV_SZ=$(stat -c %s "$HALIUM_VEN")
    echo "halium_system_a.img: $HS_SZ bytes"
    echo "halium_vendor_a.img: $HV_SZ bytes"
    LPMAKE_EXTRA+=(
        --partition "halium_system_a:readonly:$HS_SZ:main_a"
        --image     "halium_system_a=$HALIUM_SYS"
        --partition "halium_vendor_a:readonly:$HV_SZ:main_a"
        --image     "halium_vendor_a=$HALIUM_VEN"
    )
else
    echo "WARN: halium-blobs/ not populated — building OHOS-only super.img"
    echo "      Run utils/host/pull-halium-blobs.sh to enable graphics."
fi

"$LPMAKE" \
    --metadata-size "$METADATA_SIZE" \
    --metadata-slots "$METADATA_SLOTS" \
    --block-size "$BLOCK_SIZE" \
    --device super:"$SUPER_SIZE" \
    --group main_a:"$GROUP_SIZE" \
    --partition system_a:readonly:"$SYS_SZ":main_a    --image system_a="$SYSTEM_IMG" \
    --partition vendor_a:readonly:"$VEN_SZ":main_a    --image vendor_a="$VENDOR_IMG" \
    --partition sys_prod_a:readonly:"$SP_SZ":main_a   --image sys_prod_a="$SYS_PROD_IMG" \
    --partition chip_prod_a:readonly:"$CP_SZ":main_a  --image chip_prod_a="$CHIP_PROD_IMG" \
    "${LPMAKE_EXTRA[@]}" \
    --sparse \
    --output "$OUTPUT"
```

### Group budget

The X23's super is 9.66 GB. We currently use ~4 GB of it (OHOS system 2 GB + vendor 256 MB + sys_prod 50 MB + chip_prod 50 MB + slack). Halium adds ~3 GB raw (~600 MB used) for `system_a` + ~600 MB raw (~150 MB used) for `vendor_a`. Comfortable fit inside `GROUP_SIZE = SUPER_SIZE/2 - 1 MB ≈ 4.83 GB`.

Re-measure on `mimir` (tablet) when porting — different super geometry.

---

## N5.4 — Mount in chainload

Extend `device/board/oniro/hybris_generic/launcher/init-chainload.sh` to mount `halium_system_a` and `halium_vendor_a` at `/root/android/system` and `/root/android/vendor` — **after** OHOS partitions, **before** the bind of `/proc /sys /dev`.

### Patch to `init-chainload.sh` (Stage 3 addition)

```bash
# ---------------------------------------------------------------------------
# Stage 3b — mount Halium system + vendor at /root/android/{system,vendor}
# if the partitions exist.  Optional: a graphics-disabled native build skips
# the Halium blobs in super, so these mappers won't exist.
# ---------------------------------------------------------------------------
if [ -b /dev/mapper/halium_system_a ] && [ -b /dev/mapper/halium_vendor_a ]; then
    mkdir -p /root/android/system /root/android/vendor
    mount -t ext4 -o ro /dev/mapper/halium_system_a /root/android/system || \
        echo "[init-chainload] mount halium_system_a failed (non-fatal)"
    mount -t ext4 -o ro /dev/mapper/halium_vendor_a /root/android/vendor || \
        echo "[init-chainload] mount halium_vendor_a failed (non-fatal)"
else
    echo "[init-chainload] halium_{system,vendor}_a absent — graphics disabled"
fi
```

The chainload's existing wait-loop already waits for `system_a`, `vendor_a`, `sys_prod_a`. We don't add `halium_*` to the wait loop because their absence is non-fatal (the OHOS-only path is valid).

### After-mount layout (inside OHOS root)

```
/             (= OHOS system_a)
├── system/                      OHOS bin/lib64/etc
├── vendor/                      OHOS vendor partition
├── sys_prod/                    OHOS sys_prod
├── chip_prod/                   OHOS chip_prod
├── android/                     (new, mounted via chainload)
│   ├── system/                  Halium system_a (RO)
│   └── vendor/                  Halium vendor_a (RO)
├── proc, sys, dev/              bind from initramfs
└── ...
```

The `/android/system` and `/android/vendor` mount points must exist in the OHOS rootfs. Either:

1. Ship empty dirs via `init.x23.cfg` pre-init (`mkdir /android/system 0755 root root`, etc. — already partially present per Phase N3.3 reference).
2. Bake them into OHOS `system.img` via a `bundle.json` overlay or via the `vendor/oniro/hybris_generic/etc/` install paths.

Option 1 is sufficient; the cfg already creates `/android`. Add the two subdirs.

---

## N5.5 — Trimmed `init.rc`: defer

The previous draft's goal was to author a stripped `init.hal-only.rc` running only the 5 HAL services. Reality check: Halium's existing `system_a` and `vendor_a` ship full `*.rc` files at `/system/etc/init/` and `/vendor/etc/init/` covering all of Android's userspace. Trimming them is an **optimization** (RAM, fork churn) — not a correctness requirement.

For bring-up, **let Halium's init.rc run as-is.** Most of the non-HAL services (zygote, system_server, audioserver, …) will start, may crash because we don't have a `/data` set up for them, and Android init's restart limits will quiesce them. The HAL services we care about (composer, gralloc, hwservicemanager, servicemanager, vndservicemanager) come up cleanly because their dependencies are container-local.

Once Milestone 3 (display) is green and we want to reduce footprint, revisit: author the trimmed cfg under `device/board/oniro/hybris_generic/launcher/android-overlay/init.hal-only.rc`, bind-mount it over `/init.rc` post-pivot.

---

## N5 deliverables

| Item | Path | Status |
|---|---|---|
| Host fetcher script | `device/board/oniro/hybris_generic/utils/host/pull-halium-blobs.sh` | ✅ Authored + run successfully |
| `lpunpack.py` (stdlib Python) | `device/board/oniro/hybris_generic/utils/host/lpunpack.py` | ✅ Authored + verified |
| `.gitignore` | `device/board/oniro/hybris_generic/.gitignore` | ✅ Added |
| `build_super_img.sh` Halium support | `device/board/oniro/hybris_generic/kernel/x23/build_super_img.sh` | ✅ Patched (conditional on `halium-blobs/`) |
| Chainload Halium mount | `device/board/oniro/hybris_generic/launcher/init-chainload.sh` | ✅ Stage 3b mounts `halium_{system,vendor}_a` |
| Bug 8.18 sandbox chmod | `device/board/oniro/hybris_generic/launcher/init-chainload.sh` Stage 3a | ✅ Remount rw → chmod 0644 → remount ro |
| `/android/{system,vendor}` mkdir | `vendor/oniro/hybris_generic/etc/init/init.x23.cfg` | ✅ Already shipped (lines 12–13) |
| `lpunpack` external | `kernel/linux/volla-vidofnir/build-dir/downloads/.../lpunpack` | ❌ Not needed — superseded by lpunpack.py |

## Entry condition for Phase N4 (androidd) ✅ MET source-side

`/android/system` and `/android/vendor` will be populated when OHOS init's `post-fs` trigger runs (after chainload Stage 3b binds them).  Verify with `ls /android/system/bin/hwservicemanager` from an `hdc shell` once the build + flash lands.
