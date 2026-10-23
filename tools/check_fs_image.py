#!/usr/bin/env python3
"""Mounts a LittleFS image and checks it holds every face and the config.

  python tools/check_fs_image.py <storage.bin>

Uses the same limits as the firmware's LittleFS (4096-byte blocks, 64-character names),
so an image the firmware couldn't mount fails here too.
"""
import json
import sys
from pathlib import Path

from littlefs import LittleFS

ROOT = Path(__file__).resolve().parent.parent
BLOCK = 4096


def main(image_path):
    data = Path(image_path).read_bytes()
    fs = LittleFS(block_size=BLOCK, block_count=len(data) // BLOCK, name_max=64, mount=False)
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
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])
