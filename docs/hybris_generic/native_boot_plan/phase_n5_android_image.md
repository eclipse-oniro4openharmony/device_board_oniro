# Phase N5 — Halium HAL Image (Native Boot)

**Status:** 🔄 Open — rewritten 2026-05-12 to reflect the chainload reality.

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

## N5.1 — Source: UBports installer bootstrap zip

The UBports installer for Volla X23 (`vidofnir`/`vidofnir_esim`) ships exactly the Halium 12 super content we need, fetched from a public Volla URL:

- **URL:** `https://volla.tech/filedump/volla-vidofnir-12.0-ubports-installer-bootstrap-v3.zip`
- **Size:** ~478 MB
- **SHA256:** `da18b5498ebae0267be894fff73bfd629be73967cf33f071d571ffc3ef46ce97`
- **Referenced from:** `https://raw.githubusercontent.com/ubports/installer-configs/master/v2/devices/vidofnir_esim.yml`

Inside the zip, `unpacked/super.img` is the Halium 12 base — an LP-formatted super containing `system_a`, `vendor_a`, and likely `product_a`/`system_ext_a` (we extract only `system_a` + `vendor_a`; the others are not load-bearing for libhybris).

> Sourcing from this zip (one-time, host-side) is cleaner than `adb pull /dev/disk/by-partlabel/system_a` from a Halium-running device, because (a) it requires no live X23, (b) the SHA256 in the installer-config pins the exact version, (c) it produces an auditable provenance for the blob in our build pipeline.

### Host script: `device/board/oniro/hybris_generic/utils/host/pull-halium-blobs.sh`

```bash
#!/bin/bash
# Fetch + extract Halium 12 system_a and vendor_a from the UBports
# bootstrap zip, stash under halium-blobs/ for build_super_img.sh.

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BLOBS="$HERE/../../halium-blobs"
URL="https://volla.tech/filedump/volla-vidofnir-12.0-ubports-installer-bootstrap-v3.zip"
SHA256="da18b5498ebae0267be894fff73bfd629be73967cf33f071d571ffc3ef46ce97"

mkdir -p "$BLOBS"
cd "$BLOBS"

if [[ ! -f bootstrap.zip ]]; then
    curl -L "$URL" -o bootstrap.zip
fi
echo "$SHA256  bootstrap.zip" | sha256sum -c -

unzip -p bootstrap.zip unpacked/super.img > halium-super.img

# Convert sparse → raw if needed (the bootstrap zip's super is non-sparse already,
# but be defensive).
if file halium-super.img | grep -q "Android sparse image"; then
    simg2img halium-super.img halium-super.raw.img
    mv halium-super.raw.img halium-super.img
fi

# Extract system_a + vendor_a from the LP container.  Use lpunpack from AOSP
# system/extras/partition_tools, or the standalone Python implementation
# (see N5.2 for build / fallback).
lpunpack --partition system_a --partition vendor_a halium-super.img .

# Rename to our convention.
mv system_a.img halium_system_a.img
mv vendor_a.img halium_vendor_a.img
rm -f product_a.img system_ext_a.img odm_a.img halium-super.img

echo "Halium blobs ready:"
ls -lh halium_system_a.img halium_vendor_a.img
```

### `.gitignore` entry

Add to `device/board/oniro/hybris_generic/.gitignore` (create if absent):

```
# Halium 12 vendor blobs, fetched by utils/host/pull-halium-blobs.sh
halium-blobs/
```

These are large (~600 MB combined) and Volla-licensed — not checked in.

---

## N5.2 — `lpunpack` availability

`lpunpack` is not in `kernel/linux/volla-vidofnir/build-dir/downloads/kernel-build-tools/linux-x86/bin/` (we ship `lpmake` there, used by `build_super_img.sh`, but not `lpunpack`). Two viable sources:

### Option A — Build from AOSP (preferred)

```bash
git clone https://android.googlesource.com/platform/system/extras
cd extras/partition_tools
# Build using standard AOSP build system, or pull a prebuilt binary
# from a recent release. lpunpack depends only on liblp.
```

A prebuilt aarch64 / x86_64 binary can be copied into `kernel/linux/volla-vidofnir/build-dir/downloads/kernel-build-tools/linux-x86/bin/lpunpack` alongside `lpmake`. Document in `pull-halium-blobs.sh` how to obtain it.

### Option B — Standalone Python implementation

The LP super format is fully documented; multiple permissive-licensed Python implementations exist (e.g. `lpunpack.py` floating around on GitHub). For our use we only need the read path. A ~200-line Python script reading `LP_METADATA_GEOMETRY_MAGIC` (0x616c4467) + the partition table is sufficient. Acceptable fallback if AOSP build is awkward.

For the plan: prefer Option A; document both in the script's README block so a developer hitting "lpunpack not found" knows the alternatives.

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
| Host fetcher script | `device/board/oniro/hybris_generic/utils/host/pull-halium-blobs.sh` | TODO |
| Gitignore | `device/board/oniro/hybris_generic/.gitignore` | TODO |
| `build_super_img.sh` Halium support | `device/board/oniro/hybris_generic/kernel/x23/build_super_img.sh` | TODO (patch) |
| Chainload Halium mount | `device/board/oniro/hybris_generic/launcher/init-chainload.sh` | TODO (patch, Stage 3b) |
| `/android/{system,vendor}` mkdir | `vendor/oniro/hybris_generic/etc/init/init.x23.cfg` | TODO (extend pre-init) |
| `lpunpack` prebuilt | `kernel/linux/volla-vidofnir/build-dir/downloads/kernel-build-tools/linux-x86/bin/lpunpack` | TODO (one-time fetch) |

## Entry condition for Phase N4 (androidd)

`/android/system` and `/android/vendor` both populated when OHOS init's `post-fs` trigger runs. Verify with `ls /android/system/bin/hwservicemanager` from an `hdc shell`.
