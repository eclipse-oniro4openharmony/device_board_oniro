#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# hyperhold_patch.sh — graft OpenHarmony HYPERHOLD onto the MT6789 kernel.
#
# HYPERHOLD is OHOS's background anonymous-memory reclaim/compression stack:
#   * zswapd          — a per-node kthread that compresses cold anon pages
#                       into zram under memory pressure
#   * zram_group      — tracks zram objects per-memcg (so a memcg's compressed
#                       pages can later be evicted as a unit)
#   * hyperhold drv   — the "eswap" backing-device layer zram_group writes out
#                       to (only active with CONFIG_ZRAM_GROUP_WRITEBACK + a
#                       backing block device)
#   * memcg ext       — per-cgroup app-score/reclaim-ratio knobs the userspace
#                       memory manager (memmgr) drives
#
# Unlike HDF/hmdfs/sharefs, HYPERHOLD is delivered as TWO parts:
#   1. ~24 self-contained NEW source files, copied verbatim from the in-tree
#      OHOS reference kernel ($ROOT/kernel/linux/linux-5.10) — DRY, stays in
#      sync with OHOS, same idea as apply_qos_auth.sh / hdf_patch.sh copying
#      framework files from $ROOT.
#   2. ~80 guarded hunks into existing core mm/zram files, carried in
#      hyperhold.patch (generated against the pristine MT6789 tree).
#
# This script does both, plus two anchored inserts into drivers/{Kconfig,
# Makefile} (those two files are also touched by ohos-adaptation.patch, which
# runs first — anchoring on the stable upstream `most/` lines keeps us robust
# against its additions instead of fighting patch context).
#
# Args (mirrors hdf_patch.sh): $1=ROOT_DIR  $2=KERNEL_SRC  $3=hyperhold.patch
set -e

ROOT_DIR="$1"
KSRC="$2"
PATCH="$3"
REF="$ROOT_DIR/kernel/linux/linux-5.10"

if [ ! -d "$REF/drivers/hyperhold" ]; then
	echo "ERROR: OHOS reference kernel not found at $REF" >&2
	echo "       (HYPERHOLD new-file sources are copied from there)" >&2
	exit 1
fi

echo "  HYPERHOLD: copying new source files from OHOS reference..."

# --- whole new directories ---------------------------------------------------
mkdir -p "$KSRC/drivers/hyperhold" "$KSRC/drivers/block/zram/zram_group"
cp -af "$REF/drivers/hyperhold/." "$KSRC/drivers/hyperhold/"
cp -af "$REF/drivers/block/zram/zram_group/." "$KSRC/drivers/block/zram/zram_group/"

# --- MT6789 5.10.209 GKI adaptations to the copied hyperhold driver ----------
# (1) blk_crypto_init_key() gained `raw_key_size` + `is_hw_wrapped` parameters in
#     the Android GKI blk-crypto backport (7 args vs the OHOS tree's 5).
perl -0pi -e 's/blk_crypto_init_key\(blk_key, key, HP_CIPHER_MODE, dun_bytes, PAGE_SIZE\)/blk_crypto_init_key(blk_key, key, HP_KEY_SIZE, false, HP_CIPHER_MODE, dun_bytes, PAGE_SIZE)/' \
	"$KSRC/drivers/hyperhold/hp_device.c"
# (2) `hp_endio` is typedef'd identically in BOTH hp_iotab.h and hyperhold.h; the
#     OHOS kernel builds -std=gnu11 (identical typedef redefinition is allowed),
#     but the MTK GKI clang build errors under -Wtypedef-redefinition.  Guard both
#     with a sentinel so only the first include defines it.
for f in hp_iotab.h hyperhold.h; do
	perl -0pi -e 's{^typedef void \(\*hp_endio\)\(struct hpio \*\);}{#ifndef _HP_ENDIO_T\n#define _HP_ENDIO_T\ntypedef void (*hp_endio)(struct hpio *);\n#endif}m' \
		"$KSRC/drivers/hyperhold/$f"
done

# --- individual new mm/ files ------------------------------------------------
for f in zswapd.c zswapd_control.c zswapd_internal.h memcg_control.c memcg_reclaim.c; do
	cp -af "$REF/mm/$f" "$KSRC/mm/$f"
done

# --- individual new include/ files ------------------------------------------
for f in zswapd.h memcg_policy.h hyperhold_inf.h; do
	cp -af "$REF/include/linux/$f" "$KSRC/include/linux/$f"
done

# --- core-edit patch (existing mm/zram files) --------------------------------
echo "  HYPERHOLD: applying core-edit patch..."
patch -p1 -d "$KSRC" < "$PATCH"

# --- drivers/{Kconfig,Makefile} wiring (overlaps ohos-adaptation.patch) ------
# Kconfig: insert our source line after the stable upstream `most` source.
if ! grep -q 'drivers/hyperhold/Kconfig' "$KSRC/drivers/Kconfig"; then
	sed -i '/source "drivers\/most\/Kconfig"/a source "drivers/hyperhold/Kconfig"' \
		"$KSRC/drivers/Kconfig"
fi
# Makefile: obj order is irrelevant, so just append (after ohos-adaptation's
# own trailing additions). printf keeps the literal $(CONFIG_..) for make.
if ! grep -q 'obj-$(CONFIG_HYPERHOLD)' "$KSRC/drivers/Makefile"; then
	printf '\nobj-$(CONFIG_HYPERHOLD)\t\t+= hyperhold/\n' >> "$KSRC/drivers/Makefile"
fi

echo "  HYPERHOLD: done."
