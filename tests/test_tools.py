"""Tests for the release tools that build the optional full image (firmware + faces)."""
import subprocess
import sys
from pathlib import Path

import pytest
from littlefs import LittleFS

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
OFFSET, SIZE = 0x190000, 0x270000  # storage partition in partitions.csv


def run(tool, *args):
    return subprocess.run([sys.executable, str(TOOLS / tool), *map(str, args)], capture_output=True, text=True)


def make_image(path, name_max=32, faces=True):
    # name_max=32 is what the esp32 core's mklittlefs writes; the firmware accepts up to 64.
    fs = LittleFS(block_size=4096, block_count=SIZE // 4096, name_max=name_max)
    fs.mkdir("/faces")
    if faces:
        for p in (ROOT / "Faces" / "bins").glob("*.bin"):
            with fs.open(f"/faces/{p.name}", "wb") as f:
                f.write(p.read_bytes())
    with fs.open("/config.json", "w") as f:
        f.write((ROOT / "mochi_config.json").read_text())
    path.write_bytes(fs.context.buffer)


def test_check_accepts_complete_image(tmp_path):
    make_image(tmp_path / "fs.bin")
    r = run("check_fs_image.py", tmp_path / "fs.bin")
    assert r.returncode == 0, r.stderr


@pytest.mark.parametrize("kwargs", [{"faces": False}, {"name_max": 255}], ids=["missing-faces", "unmountable"])
def test_check_rejects_bad_image(tmp_path, kwargs):
    make_image(tmp_path / "fs.bin", **kwargs)
    assert run("check_fs_image.py", tmp_path / "fs.bin").returncode != 0


def test_full_image_places_storage_at_partition(tmp_path):
    make_image(tmp_path / "fs.bin")
    (tmp_path / "fw.bin").write_bytes(b"\x11" * OFFSET + b"\xff" * SIZE)
    r = run("full_image.py", ROOT / "Mochi_EN" / "partitions.csv", tmp_path / "fw.bin", tmp_path / "fs.bin", tmp_path / "out.bin")
    assert r.returncode == 0, r.stderr
    out = (tmp_path / "out.bin").read_bytes()
    assert out[:OFFSET] == b"\x11" * OFFSET
    assert out[OFFSET:] == (tmp_path / "fs.bin").read_bytes()


def test_full_image_refuses_to_overwrite_data(tmp_path):
    make_image(tmp_path / "fs.bin")
    (tmp_path / "fw.bin").write_bytes(b"\x11" * (OFFSET + SIZE))
    r = run("full_image.py", ROOT / "Mochi_EN" / "partitions.csv", tmp_path / "fw.bin", tmp_path / "fs.bin", tmp_path / "out.bin")
    assert r.returncode != 0
    assert not (tmp_path / "out.bin").exists()
