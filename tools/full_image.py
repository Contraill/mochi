#!/usr/bin/env python3
"""Puts a LittleFS image (faces + config) into the storage partition of a merged firmware image.

  python tools/full_image.py <partitions.csv> <firmware.merged.bin> <storage.bin> <output.bin>

The merged image from arduino-cli covers the whole 4MB flash and leaves the storage
partition erased (0xFF). This replaces that region with the LittleFS image, so one
flash gives a ready-to-use device.
"""
import sys
from pathlib import Path


def storage_partition(csv_path):
    for line in Path(csv_path).read_text().splitlines():
        cols = [c.strip() for c in line.split("#", 1)[0].split(",")]
        if len(cols) >= 5 and cols[0] == "storage":
            return int(cols[3], 0), int(cols[4], 0)
    raise SystemExit(f"no 'storage' partition in {csv_path}")


def main(csv_path, merged_path, storage_path, out_path):
    offset, size = storage_partition(csv_path)
    merged = bytearray(Path(merged_path).read_bytes())
    storage = Path(storage_path).read_bytes()
    if len(storage) != size:
        raise SystemExit(f"storage image is {len(storage)} bytes, partition is {size}")
    if len(merged) < offset + size:
        raise SystemExit(f"merged image is {len(merged)} bytes, needs at least {offset + size}")
    if any(b != 0xFF for b in merged[offset:offset + size]):
        raise SystemExit("storage region of the merged image is not empty; refusing to overwrite")
    merged[offset:offset + size] = storage
    Path(out_path).write_bytes(merged)
    print(f"{out_path}: storage image at 0x{offset:X} ({size} bytes)")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
