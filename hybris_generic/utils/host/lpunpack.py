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
extents — what the UBports bootstrap super.img uses).

Android *sparse* super images are read transparently (see SparseReader),
so the caller never has to materialise the multi-GB raw expansion that
`simg2img` would write."""

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

SPARSE_HEADER_MAGIC = 0xED26FF3A
CHUNK_TYPE_RAW = 0xCAC1
CHUNK_TYPE_FILL = 0xCAC2
CHUNK_TYPE_DONT_CARE = 0xCAC3
CHUNK_TYPE_CRC32 = 0xCAC4


class SparseReader:
    """Read-only seekable view of an Android sparse image as its raw expansion.

    Only the chunks a read actually touches are pulled from disk, so
    extracting a 900 MB partition out of a sparse super never
    materialises the 9 GB raw image.
    """

    def __init__(self, f):
        self._f = f
        f.seek(0)
        hdr = f.read(28)
        (magic, major, _minor, file_hdr_sz, chunk_hdr_sz, blk_sz,
         total_blks, total_chunks, _checksum) = struct.unpack("<IHHHHIIII", hdr)
        if magic != SPARSE_HEADER_MAGIC:
            raise ValueError("not a sparse image")
        if major != 1:
            raise SystemExit(f"unsupported sparse major version {major}")
        self.size = total_blks * blk_sz
        self._pos = 0
        # chunks: (out_start, out_end, kind, payload) sorted by out_start
        self._chunks = []
        off = file_hdr_sz
        out = 0
        for _ in range(total_chunks):
            f.seek(off)
            chunk_type, _res, chunk_sz, total_sz = struct.unpack("<HHII", f.read(12))
            data_off = off + chunk_hdr_sz
            data_len = total_sz - chunk_hdr_sz
            out_len = chunk_sz * blk_sz
            if chunk_type == CHUNK_TYPE_RAW:
                self._chunks.append((out, out + out_len, "raw", data_off))
            elif chunk_type == CHUNK_TYPE_FILL:
                f.seek(data_off)
                self._chunks.append((out, out + out_len, "fill", f.read(4)))
            elif chunk_type == CHUNK_TYPE_DONT_CARE:
                self._chunks.append((out, out + out_len, "zero", None))
            elif chunk_type == CHUNK_TYPE_CRC32:
                out_len = 0
            else:
                raise SystemExit(f"unknown sparse chunk type 0x{chunk_type:04x}")
            out += out_len
            off += chunk_hdr_sz + data_len

    def seek(self, offset, whence=os.SEEK_SET):
        if whence == os.SEEK_SET:
            self._pos = offset
        elif whence == os.SEEK_CUR:
            self._pos += offset
        else:
            self._pos = self.size + offset
        return self._pos

    def tell(self):
        return self._pos

    def _find(self, pos):
        lo, hi = 0, len(self._chunks) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            start, end, _, _ = self._chunks[mid]
            if pos < start:
                hi = mid - 1
            elif pos >= end:
                lo = mid + 1
            else:
                return mid
        return None

    def read(self, size=-1):
        if size < 0:
            size = self.size - self._pos
        size = max(0, min(size, self.size - self._pos))
        out = bytearray()
        while size > 0:
            i = self._find(self._pos)
            if i is None:
                break
            start, end, kind, payload = self._chunks[i]
            n = min(size, end - self._pos)
            if kind == "raw":
                self._f.seek(payload + (self._pos - start))
                out += self._f.read(n)
            elif kind == "fill":
                skew = (self._pos - start) % 4
                out += ((payload * (n // 4 + 2))[skew:skew + n])
            else:
                out += bytes(n)
            self._pos += n
            size -= n
        return bytes(out)

    def close(self):
        self._f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def open_super(path):
    """Open an LP super image, transparently unsparsing if needed."""
    f = open(path, "rb")
    if f.read(4) == struct.pack("<I", SPARSE_HEADER_MAGIC):
        return SparseReader(f)
    f.seek(0)
    return f


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
    with open_super(super_path) as f:
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
    with open_super(super_path) as src, open(out_path, "wb") as dst:
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
