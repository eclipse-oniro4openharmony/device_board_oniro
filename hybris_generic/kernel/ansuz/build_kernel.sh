#!/bin/bash
#
# Copyright (C) 2026 Oniro / Hybris Generic.
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Build the OHOS-patched android14-6.1-halium GKI kernel for the Volla
# Phone Plinius (ansuz) with the Halium generic adaptation build tools.
#
# Differences vs kernel/x23/build_kernel.sh (vendor 5.10 flow):
#   - The kernel is the *generic* GKI android14-6.1-halium tree cloned by
#     the port repo's build tools (no per-device kernel repo).
#   - OHOS drivers are applied as ONE git patch (ohos-adaptation.patch)
#     directly onto the kernel git tree (reset first — the tree is a git
#     clone, so a pristine copy is one `git checkout` away; no .orig copy
#     dance needed for a 1.7 GB tree).
#   - KMI GUARD: stock prebuilt MTK vendor modules must keep loading
#     (CONFIG_MODVERSIONS CRC matching). After the patched build, this
#     script diffs Module.symvers against the baseline (pristine) build
#     and FAILS if any pre-existing symbol CRC changed.
#
# Outputs (via the Halium tools):
#   kernel/linux/volla-ansuz/workdir/tmp/partitions/boot.img         (kernel-only, v4)
#   kernel/linux/volla-ansuz/workdir/tmp/partitions/init_boot.img    (generic ramdisk)
#   kernel/linux/volla-ansuz/workdir/tmp/partitions/vendor_boot.img  (DTB+modules+recovery)
#   kernel/linux/volla-ansuz/workdir/tmp/partitions/dtbo.img
#   copied to kernel/linux/volla-ansuz/out/
#
# Usage:
#   build_kernel.sh              # patched (OHOS) build + KMI guard
#   build_kernel.sh --baseline   # pristine build, records baseline symvers

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OHOS_ROOT="$(cd "$HERE/../../../../../.." && pwd)"
KERNEL_TREE="$OHOS_ROOT/kernel/linux/volla-ansuz"
KERNEL_SRC="$KERNEL_TREE/workdir/downloads/kernel-android-common"
KERNEL_OBJ="$KERNEL_TREE/workdir/downloads/KERNEL_OBJ"
OUT_DIR="$KERNEL_TREE/out"
BASELINE_SYMVERS="$KERNEL_TREE/workdir/baseline-Module.symvers"

# Pinned refs (recorded 2026-07-23; update consciously)
ANSUZ_REPO_URL="https://gitlab.com/ubports/porting/reference-device-ports/halium-14/volla-phone-plinius/volla-ansuz.git"
ANSUZ_REPO_SHA="f8c758198ed3b2735b9d5945a19b642f4dbbd6a3"
BUILDTOOLS_SHA="d5838d5c4cf90c7dbece749a451fb14271847dc9"
KERNEL_SHA="2d5a7bc8a21fc06e5ab80a149a8b01e958e1cb12"   # android14-6.1-halium tip (6.1.129 era)

MODE="${1:-patched}"

# ---------------------------------------------------------------------------
# 1. Port repo + sources (idempotent; heavy clone happens once)
# ---------------------------------------------------------------------------
if [ ! -d "$KERNEL_TREE" ]; then
    git clone "$ANSUZ_REPO_URL" "$KERNEL_TREE"
    git -C "$KERNEL_TREE" checkout "$ANSUZ_REPO_SHA"
fi
if [ ! -d "$KERNEL_SRC" ]; then
    ( cd "$KERNEL_TREE" && ./build.sh -b workdir -c )
fi
for repo_sha in "$KERNEL_TREE:$ANSUZ_REPO_SHA" "$KERNEL_TREE/build:$BUILDTOOLS_SHA" "$KERNEL_SRC:$KERNEL_SHA"; do
    repo="${repo_sha%%:*}"; want="${repo_sha##*:}"
    have="$(git -C "$repo" rev-parse HEAD)"
    [ "$have" = "$want" ] || echo "WARN: $repo at $have, pinned $want" >&2
done

# ---------------------------------------------------------------------------
# 2. Reset kernel tree to pristine, then (for patched builds) apply OHOS bits
# ---------------------------------------------------------------------------
git -C "$KERNEL_SRC" checkout -- .
git -C "$KERNEL_SRC" clean -fdq

# deviceinfo: run with the openharmony fragment appended for patched
# builds; baseline uses upstream's list. deviceinfo is sourced from the
# port-repo checkout — we edit a scratch copy via git, never permanently.
git -C "$KERNEL_TREE" checkout -- deviceinfo

if [ "$MODE" != "--baseline" ]; then
    # ohos-adaptation.patch owns drivers/ + include/; hmdfs.patch owns
    # fs/hmdfs/ and two anchor hunks in fs/Kconfig + fs/Makefile.  The two
    # never touch the same file, so order does not matter.  The staged
    # patches (2026-08-02) were diffed against a tree with the first two
    # applied, so they must come AFTER them: ohos-fs-staged owns
    # fs/{sharefs,epfs,dec} + anchors + one selinux SE_SBGENFS line;
    # ohos-dfx-staged owns drivers/staging/{hisysevent,zerohung,hungtask,
    # blackbox,ucollection} + anchors + include/dfx + blackbox headers +
    # the kernel/hung_task.c handoff hunks.
    for p in ohos-adaptation hmdfs ohos-fs-staged ohos-dfx-staged; do
        echo "Applying $p.patch..."
        git -C "$KERNEL_SRC" apply --whitespace=nowarn "$HERE/patches/kernel-source/$p.patch"
    done
    cp "$HERE/config/openharmony.config" "$KERNEL_SRC/arch/arm64/configs/openharmony.config"
    sed -i 's/^deviceinfo_kernel_defconfig=.*/deviceinfo_kernel_defconfig="gki_defconfig halium.config openharmony.config"/' \
        "$KERNEL_TREE/deviceinfo"
fi

# ---------------------------------------------------------------------------
# 3. Build (kernel + boot images)
# ---------------------------------------------------------------------------
( cd "$KERNEL_TREE" && ./build.sh -b workdir -k )

# ---------------------------------------------------------------------------
# 4. KMI guard / baseline bookkeeping
# ---------------------------------------------------------------------------
if [ "$MODE" = "--baseline" ]; then
    cp "$KERNEL_OBJ/Module.symvers" "$BASELINE_SYMVERS"
    echo "Baseline Module.symvers recorded ($(wc -l < "$BASELINE_SYMVERS") symbols)."
else
    # The authoritative contract is kmi/stock-module-versions.txt — the
    # union of every (crc, symbol) the stock prebuilt modules' __versions
    # tables expect (vendor_boot prebuilts + vendor_dlkm + system_dlkm).
    # It is tracked in-tree (see kmi/README.md); the gitignored recon
    # dump is kept as a fallback for older checkouts.  Any expectation
    # naming a vmlinux export must match our CRC exactly; expectations we
    # don't provide are inter-module symbols (stock modules exporting to
    # each other) and are fine.
    STOCK_VERS="$HERE/kmi/stock-module-versions.txt"
    [ -f "$STOCK_VERS" ] || \
        STOCK_VERS="$OHOS_ROOT/device/board/oniro/hybris_generic/halium-blobs/ansuz/recon/stock-module-versions.txt"
    if [ ! -f "$STOCK_VERS" ]; then
        echo "WARN: $STOCK_VERS missing — KMI contract unchecked" >&2
    else
        python3 - "$KERNEL_OBJ/Module.symvers" "$STOCK_VERS" <<'PYEOF'
import sys
ours = {}
for line in open(sys.argv[1]):
    f = line.split('\t')
    if len(f) >= 2:
        ours[f[1]] = f[0].lower()
mismatch = []
for line in open(sys.argv[2]):
    p = line.split()
    if len(p) == 2 and p[1] in ours and ours[p[1]] != p[0].lower():
        mismatch.append((p[1], p[0].lower(), ours[p[1]]))
if mismatch:
    print("ERROR: KMI contract violated — stock modules would reject:", file=sys.stderr)
    for n, want, have in mismatch[:20]:
        print(f"  {n}: stock wants {want}, kernel has {have}", file=sys.stderr)
    sys.exit(1)
print(f"KMI guard: OK ({len(ours)} exports, 0 stock-expectation mismatches).")
PYEOF
    fi
fi

# ---------------------------------------------------------------------------
# 5. Artifacts
# ---------------------------------------------------------------------------
mkdir -p "$OUT_DIR"
for img in boot.img init_boot.img vendor_boot.img dtbo.img; do
    [ -f "$KERNEL_TREE/workdir/tmp/partitions/$img" ] && \
        cp "$KERNEL_TREE/workdir/tmp/partitions/$img" "$OUT_DIR/"
done
tar -czf "$OUT_DIR/modules.tar.gz" -C "$KERNEL_TREE/workdir/tmp/system/lib" modules 2>/dev/null || true

echo "Build complete ($MODE). Artifacts:"
ls -l "$OUT_DIR"
