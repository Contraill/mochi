#!/usr/bin/env python3
"""Mounts a LittleFS image and checks it holds every face and the config.

  python tools/check_fs_image.py <partitions.csv> <storage.bin>

Mounts with the same parameters as the firmware, so an image the firmware would reject
(and then format, losing the faces) fails here too:
  - block size 4096, block count = storage partition size / 4096 (esp_littlefs uses the
    partition size; littlefs rejects a superblock with a different block count)
  - name_max 255: esp_littlefs leaves cfg.name_max at 0, so littlefs uses LFS_NAME_MAX (255)
    and rejects images whose superblock allows longer names
"""
import json
import sys
from pathlib import Path

from full_image import storage_partition
from littlefs import LittleFS

ROOT = Path(__file__).resolve().parent.parent
BLOCK = 4096
NAME_MAX = 255


def main(csv_path, image_path):
    _, size = storage_partition(csv_path)
    data = Path(image_path).read_bytes()
    if len(data) != size:
        raise SystemExit(f"image is {len(data)} bytes, storage partition is {size}")
    fs = LittleFS(block_size=BLOCK, block_count=size // BLOCK, name_max=NAME_MAX, mount=False)
    fs.context.buffer = bytearray(data)
    fs.mount()

    expected = {p.name: p.read_bytes() for p in (ROOT / "Faces" / "bins").glob("*.bin")}
    found = set(fs.listdir("/faces"))
    if found != set(expected):
        raise SystemExit(f"faces differ: missing {sorted(set(expected) - found)}, extra {sorted(found - set(expected))}")
    for name, content in expected.items():
        with fs.open(f"/faces/{name}", "rb") as f:
            if f.read() != content:
                raise SystemExit(f"/faces/{name} content differs")
    with fs.open("/config.json", "r") as f:
        config = json.load(f)
    if config.get("currentFace") + ".bin" not in expected:
        raise SystemExit(f"config default face {config.get('currentFace')!r} is not in the image")
    print(f"{image_path}: {len(found)} faces + config.json OK")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
