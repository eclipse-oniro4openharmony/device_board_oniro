#!/usr/bin/env python3
"""Minimal AOSP LP-format super-image extractor.

Pure-Python (stdlib only) implementation of the `lpunpack` half of AOSP's
system/extras/partition_tools.  Reads an LP-formatted super.img and
extracts individual logical partitions to raw images.

We ship this so the Halium blob fetcher doesn't depend on having AOSP's
C++ lpunpack on $PATH (it isn't in the kernel-build-tools we ship, and
building it from AOSP requires the AOSP build system).

Format reference: AOSP system/core/fs_mgr/liblp/include/liblp/metadata_format.h.
Supported: LP metadata v1.0 through v1.2 (single block device, linear
extents — what the UBports bootstrap super.img uses)."""

import argparse
import os
import struct
import sys

LP_SECTOR_SIZE = 512
LP_PARTITION_RESERVED_BYTES = 4096
LP_METADATA_GEOMETRY_SIZE = 4096
LP_METADATA_GEOMETRY_MAGIC = 0x616C4467
LP_METADATA_HEADER_MAGIC = 0x414C5030
LP_TARGET_TYPE_LINEAR = 0


def _read_geometry(f):
    f.seek(LP_PARTITION_RESERVED_BYTES)
    data = f.read(LP_METADATA_GEOMETRY_SIZE)
    magic, struct_size = struct.unpack_from("<II", data, 0)
    if magic != LP_METADATA_GEOMETRY_MAGIC:
        raise SystemExit(f"bad geometry magic 0x{magic:08x}")
    # offsets: 0 magic(4) 4 struct_size(4) 8 checksum(32) 40 metadata_max_size(4)
    #          44 metadata_slot_count(4) 48 logical_block_size(4)
    metadata_max_size, metadata_slot_count, logical_block_size = \
        struct.unpack_from("<III", data, 40)
    return metadata_max_size, metadata_slot_count, logical_block_size


def _read_header(f, geo_max_size, slot=0):
    offset = (LP_PARTITION_RESERVED_BYTES
              + 2 * LP_METADATA_GEOMETRY_SIZE
              + geo_max_size * slot)
    f.seek(offset)
    base = f.read(80)
    magic, major, minor, header_size = struct.unpack_from("<IHHI", base, 0)
    if magic != LP_METADATA_HEADER_MAGIC:
        raise SystemExit(f"bad metadata header magic 0x{magic:08x}")
    tables_size, = struct.unpack_from("<I", base, 44)
    f.seek(offset)
    full = f.read(header_size)
    # Header table descriptors start at offset 80 (after 80-byte v1.0 header
    # prefix).  Each LpMetadataTableDescriptor = (offset:u32, num_entries:u32,
    # entry_size:u32) = 12 bytes.
    partitions    = struct.unpack_from("<III", full, 80)
    extents       = struct.unpack_from("<III", full, 92)
    groups        = struct.unpack_from("<III", full, 104)
    block_devices = struct.unpack_from("<III", full, 116)
    tables = f.read(tables_size)
    return {
        "version":       (major, minor),
        "partitions":    partitions,
        "extents":       extents,
        "groups":        groups,
        "block_devices": block_devices,
        "tables":        tables,
    }


def list_partitions(super_path):
    with open(super_path, "rb") as f:
        geo_max_size, _, _ = _read_geometry(f)
        hdr = _read_header(f, geo_max_size, slot=0)
        p_off, p_num, p_size = hdr["partitions"]
        e_off, e_num, e_size = hdr["extents"]
        tables = hdr["tables"]
        partitions = []
        for i in range(p_num):
            entry = tables[p_off + i * p_size: p_off + (i + 1) * p_size]
            name = entry[0:36].rstrip(b"\x00").decode("ascii", "replace")
            attributes, first_extent_index, num_extents, group_index = \
                struct.unpack_from("<IIII", entry, 36)
            total_sectors = 0
            extents = []
            for j in range(num_extents):
                ext_off = e_off + (first_extent_index + j) * e_size
                num_sectors, target_type, target_data, target_source = \
                    struct.unpack_from("<QIQI", tables, ext_off)
                extents.append((num_sectors, target_type, target_data))
                total_sectors += num_sectors
            partitions.append({
                "name": name,
                "size_bytes": total_sectors * LP_SECTOR_SIZE,
                "extents": extents,
            })
        return partitions


def extract(super_path, partition_name, out_path):
    parts = list_partitions(super_path)
    p = next((x for x in parts if x["name"] == partition_name), None)
    if p is None:
        names = ", ".join(x["name"] for x in parts)
        raise SystemExit(f"partition {partition_name!r} not found in {super_path} "
                         f"(have: {names})")
    with open(super_path, "rb") as src, open(out_path, "wb") as dst:
        for num_sectors, target_type, target_data in p["extents"]:
            if target_type != LP_TARGET_TYPE_LINEAR:
                raise SystemExit(f"unsupported extent target_type {target_type}")
            src.seek(target_data * LP_SECTOR_SIZE)
            remaining = num_sectors * LP_SECTOR_SIZE
            while remaining > 0:
                chunk = src.read(min(remaining, 1 << 20))
                if not chunk:
                    raise SystemExit("short read while extracting "
                                     f"{partition_name}")
                dst.write(chunk)
                remaining -= len(chunk)
    print(f"  extracted {partition_name} → {out_path} ({p['size_bytes']} bytes)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip())
    ap.add_argument("super_img")
    ap.add_argument("out_dir", nargs="?", default=".")
    ap.add_argument("--partition", action="append",
                    help="extract this partition (repeatable); default = list")
    args = ap.parse_args()

    if not args.partition:
        for p in list_partitions(args.super_img):
            print(f"{p['name']:24s} {p['size_bytes']:>14d}")
        return

    os.makedirs(args.out_dir, exist_ok=True)
    for name in args.partition:
        out = os.path.join(args.out_dir, f"{name}.img")
        extract(args.super_img, name, out)


if __name__ == "__main__":
    main()
