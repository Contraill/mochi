import argparse
import random
import sys
from pathlib import Path

import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "Faces"))

import gif_to_bin as g  # noqa: E402

BINS = sorted((ROOT / "Faces" / "bins").glob("*.bin"))
GIFS = sorted((ROOT / "Faces").glob("*.gif"))


def test_parse_resize():
    assert g.parse_resize("128x64") == (128, 64)
    assert g.parse_resize("64X32") == (64, 32)
    for bad in ("130x64", "abc", "0x64", "128"):
        with pytest.raises(argparse.ArgumentTypeError):
            g.parse_resize(bad)


def test_delay_units():
    assert g.delay_ms_to_units(0) == 1        # clamped to 10 ms
    assert g.delay_ms_to_units(100) == 10
    assert g.delay_ms_to_units(104) == 10
    assert g.delay_ms_to_units(99999) == 255  # clamped to 2550 ms


def test_rle_delta_roundtrip():
    rnd = random.Random(1)
    prev = bytes(1024)
    for _ in range(20):
        cur = bytearray(prev)
        for _ in range(rnd.randint(0, 200)):
            cur[rnd.randrange(1024)] = rnd.randrange(256)
        cur = bytes(cur)
        comp = g.rle_delta_encode(prev, cur)
        assert g.decode_moc3_frame(comp, prev, 1024) == cur
        prev = cur


def test_static_frame_compresses_small():
    frame = bytes(range(256)) * 4
    assert len(g.rle_delta_encode(frame, frame)) == 10  # 1024 zeros -> 5 (count, value) pairs


def _make_gif(path, delays):
    frames = []
    for i in range(len(delays)):
        im = Image.new("L", (128, 64), 0)
        im.paste(255, (i * 20, 10, i * 20 + 30, 40))
        frames.append(im)
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=delays, loop=0)


@pytest.mark.parametrize("fmt,magic", [("moc3", b"MOC3"), ("moc2", b"MOC2")])
def test_gif_to_bin_end_to_end(tmp_path, fmt, magic):
    src = tmp_path / "in"
    src.mkdir()
    _make_gif(src / "face.gif", [100, 200, 300])
    out = tmp_path / "out"

    assert g.main(["--input", str(src), "--output", str(out), "--format", fmt]) == 0

    data = (out / "face.bin").read_bytes()
    assert data[:4] == magic
    assert data[4:7] == bytes((3, 16, 64))
    g.validate_bin_file(out / "face.bin", 128, 64)


def test_validate_rejects_garbage(tmp_path):
    bad = tmp_path / "bad.bin"
    bad.write_bytes(b"MOC3" + bytes((1, 16, 64, 3)) + b"\x05\x02\x00\x01")
    with pytest.raises(ValueError):
        g.validate_bin_file(bad, 128, 64)


@pytest.mark.parametrize("path", BINS, ids=lambda p: p.stem)
def test_bundled_face_is_valid_moc3(path):
    assert path.read_bytes()[:4] == b"MOC3"
    g.validate_bin_file(path, 128, 64)


def test_every_gif_has_a_bin():
    assert BINS, "no bundled faces found"
    assert {p.stem for p in GIFS} == {p.stem for p in BINS}
