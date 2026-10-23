#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gif_to_bin.py - Mochi GIF/PNG/JPG -> .bin converter

The Mochi firmware reads two formats:
  - MOC2: raw frame data. Large files, simple format.
  - MOC3: delta + RLE compressed. Default and recommended.

The script never shortens animations. The header stores frame_count in one byte,
so inputs with more than 255 frames are rejected by default. Use --allow-trim to
resample them to 255 frames on purpose.

Basic usage:
  python gif_to_bin.py
  python gif_to_bin.py --input ./gifs --output ./bins
  python gif_to_bin.py playful.gif smirk.gif
  python gif_to_bin.py --format moc2 love.gif
  python gif_to_bin.py --preview --threshold 120 *.gif

Notes:
  - Default output is MOC3.
  - Default size is 128x64; the Mochi firmware expects exactly that.
  - Width must be a multiple of 8.
  - The delay byte is in 10 ms units: 10 = 100 ms.
"""

from __future__ import annotations

import argparse
import glob
import os
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Tuple

try:
    from PIL import Image, ImageSequence, ImageOps
except ImportError:
    print("ERROR: the Pillow library is required.")
    print("Install it with: python -m pip install Pillow")
    sys.exit(1)


MOC2_MAGIC = b"MOC2"
MOC3_MAGIC = b"MOC3"
MOC3_FORMAT_RLE_DELTA = 3
HEADER_SIZE = 8

DEFAULT_WIDTH = 128
DEFAULT_HEIGHT = 64
DEFAULT_THRESHOLD = 128
DEFAULT_SPEED = 1.0
DEFAULT_DELAY_MS = 100
MIN_DELAY_MS = 10
MAX_DELAY_MS = 2550
MAX_FRAMES = 255

IMAGE_EXTENSIONS = {".gif", ".png", ".jpg", ".jpeg", ".bmp", ".webp"}

try:
    RESAMPLE_LANCZOS = Image.Resampling.LANCZOS
    RESAMPLE_NEAREST = Image.Resampling.NEAREST
except AttributeError:  # Pillow < 9
    RESAMPLE_LANCZOS = Image.LANCZOS
    RESAMPLE_NEAREST = Image.NEAREST


@dataclass
class EncodedFrame:
    delay_units: int
    data: bytes


@dataclass
class ConversionResult:
    input_path: Path
    output_path: Path
    fmt: str
    frame_count: int
    file_size: int
    total_duration_ms: int


def parse_resize(value: str) -> Tuple[int, int]:
    try:
        w_s, h_s = value.lower().split("x", 1)
        width = int(w_s)
        height = int(h_s)
    except Exception as exc:
        raise argparse.ArgumentTypeError(
            f"Invalid size: {value!r}. Example: 128x64"
        ) from exc

    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError("Width and height must be positive.")
    if width % 8 != 0:
        raise argparse.ArgumentTypeError("Width must be a multiple of 8.")
    if width // 8 > 255 or height > 255:
        raise argparse.ArgumentTypeError("Because of the header format, width/8 and height can be at most 255.")
    return width, height


def clamp_delay_ms(delay_ms: int) -> int:
    return max(MIN_DELAY_MS, min(MAX_DELAY_MS, int(delay_ms)))


def delay_ms_to_units(delay_ms: int) -> int:
    # The firmware multiplies the delay byte by 10 ms. round() keeps GIF timings closer than floor().
    return max(1, min(255, int(round(clamp_delay_ms(delay_ms) / 10.0))))


def units_to_ms(delay_units: int) -> int:
    return int(delay_units) * 10


def flatten_to_background(img: Image.Image, background: Tuple[int, int, int]) -> Image.Image:
    """Flattens transparency onto a background, like the OLED's black."""
    rgba = img.convert("RGBA")
    bg = Image.new("RGBA", rgba.size, background + (255,))
    bg.alpha_composite(rgba)
    return bg.convert("RGB")


def resize_image(img: Image.Image, width: int, height: int, fit: str, background: Tuple[int, int, int]) -> Image.Image:
    """Resizes the image to the output raster (128x64).

    stretch: stretch the image to fit exactly (original behavior).
    contain: keep the aspect ratio, fill the rest with the background.
    cover: keep the aspect ratio, crop what doesn't fit.
    """
    if fit == "stretch":
        return img.resize((width, height), RESAMPLE_LANCZOS)

    if fit == "contain":
        contained = ImageOps.contain(img, (width, height), method=RESAMPLE_LANCZOS)
        canvas = Image.new("RGB", (width, height), background)
        x = (width - contained.width) // 2
        y = (height - contained.height) // 2
        canvas.paste(contained, (x, y))
        return canvas

    if fit == "cover":
        return ImageOps.fit(img, (width, height), method=RESAMPLE_LANCZOS, centering=(0.5, 0.5))

    raise ValueError(f"Unknown fit mode: {fit}")


def image_to_oled_bytes(
    img: Image.Image,
    width: int,
    height: int,
    threshold: int,
    invert: bool,
    fit: str,
    background: Tuple[int, int, int],
) -> bytes:
    """PIL image -> horizontal byte layout compatible with U8g2 XBM.

    Rows go left to right, each byte holds 8 pixels, least significant bit first:
    the pixel at x=0 is bit 0.
    """
    img = flatten_to_background(img, background)
    img = resize_image(img, width, height, fit, background)
    gray = img.convert("L")

    pixels = gray.load()
    bytes_per_row = width // 8
    out = bytearray(bytes_per_row * height)

    for y in range(height):
        row_base = y * bytes_per_row
        for x in range(width):
            white = pixels[x, y] >= threshold
            if invert:
                white = not white
            if white:
                out[row_base + (x // 8)] |= 1 << (x & 7)

    return bytes(out)


def read_image_frames(
    path: Path,
    width: int,
    height: int,
    threshold: int,
    invert: bool,
    speed: float,
    fit: str,
    background: Tuple[int, int, int],
    allow_trim: bool,
) -> List[EncodedFrame]:
    """Reads a GIF/PNG/JPG file into a list of frames."""
    if speed <= 0:
        raise ValueError("--speed must be positive. Example: 1.0 normal, 2.0 twice as fast.")

    with Image.open(path) as im:
        is_animated = bool(getattr(im, "is_animated", False))
        source_frames: List[Tuple[int, bytes]] = []

        if is_animated:
            for frame in ImageSequence.Iterator(im):
                frame_copy = frame.copy()
                duration = frame_copy.info.get("duration", im.info.get("duration", DEFAULT_DELAY_MS))
                if not duration or duration <= 0:
                    duration = DEFAULT_DELAY_MS
                delay_ms = clamp_delay_ms(int(round(float(duration) / speed)))
                data = image_to_oled_bytes(frame_copy, width, height, threshold, invert, fit, background)
                source_frames.append((delay_ms, data))
        else:
            delay_ms = clamp_delay_ms(int(round(DEFAULT_DELAY_MS / speed)))
            data = image_to_oled_bytes(im.copy(), width, height, threshold, invert, fit, background)
            source_frames.append((delay_ms, data))

    if not source_frames:
        raise ValueError("No frames could be read.")

    if len(source_frames) > MAX_FRAMES:
        if not allow_trim:
            raise ValueError(
                f"{len(source_frames)} frames; the MOC header supports at most {MAX_FRAMES}. "
                "The animation can't be converted without shortening it. Use --allow-trim to resample it."
            )
        source_frames = sample_frames_preserving_duration(source_frames, MAX_FRAMES)

    return [EncodedFrame(delay_ms_to_units(ms), data) for ms, data in source_frames]


def sample_frames_preserving_duration(frames: Sequence[Tuple[int, bytes]], target_count: int) -> List[Tuple[int, bytes]]:
    """Resamples animations over 255 frames while roughly keeping the total duration.

    Not used by default, so animations are never shortened unless asked for.
    """
    if len(frames) <= target_count:
        return list(frames)

    total = len(frames)
    selected: List[Tuple[int, bytes]] = []
    for i in range(target_count):
        start = round(i * total / target_count)
        end = round((i + 1) * total / target_count)
        bucket = frames[start:max(start + 1, end)]
        delay_sum = sum(ms for ms, _ in bucket)
        selected.append((clamp_delay_ms(delay_sum), bucket[0][1]))
    return selected


def rle_delta_encode(prev: bytes, cur: bytes) -> bytes:
    if len(prev) != len(cur):
        raise ValueError("Frames must be the same size for delta encoding.")

    delta = bytes(a ^ b for a, b in zip(prev, cur))
    out = bytearray()
    i = 0
    while i < len(delta):
        value = delta[i]
        run = 1
        while i + run < len(delta) and delta[i + run] == value and run < 255:
            run += 1
        out.append(run)
        out.append(value)
        i += run
    return bytes(out)


def write_moc2(frames: Sequence[EncodedFrame], output_path: Path, bytes_per_row: int, height: int) -> None:
    if not (1 <= len(frames) <= MAX_FRAMES):
        raise ValueError("Frame count must be between 1 and 255.")
    blob = bytearray(MOC2_MAGIC + bytes((len(frames), bytes_per_row, height, 0)))
    expected_size = bytes_per_row * height
    for frame in frames:
        if len(frame.data) != expected_size:
            raise ValueError("Frame data size does not match the header.")
        blob.append(frame.delay_units)
        blob.extend(frame.data)
    output_path.write_bytes(blob)


def write_moc3(frames: Sequence[EncodedFrame], output_path: Path, bytes_per_row: int, height: int) -> None:
    if not (1 <= len(frames) <= MAX_FRAMES):
        raise ValueError("Frame count must be between 1 and 255.")
    frame_size = bytes_per_row * height
    blob = bytearray(MOC3_MAGIC + bytes((len(frames), bytes_per_row, height, MOC3_FORMAT_RLE_DELTA)))
    prev = bytes(frame_size)

    for frame in frames:
        if len(frame.data) != frame_size:
            raise ValueError("Frame data size does not match the header.")
        comp = rle_delta_encode(prev, frame.data)
        if len(comp) > 0xFFFF:
            raise ValueError("A compressed frame exceeds the 65535 byte limit.")
        blob.append(frame.delay_units)
        blob.extend(struct.pack("<H", len(comp)))
        blob.extend(comp)
        prev = frame.data

    output_path.write_bytes(blob)


def decode_moc3_frame(comp: bytes, prev: bytes, frame_size: int) -> bytes:
    if len(comp) % 2:
        raise ValueError("MOC3 compLen cannot be odd.")
    out = bytearray()
    i = 0
    while i < len(comp):
        count = comp[i]
        value = comp[i + 1]
        if count == 0:
            raise ValueError("MOC3 RLE count=0 is invalid.")
        if len(out) + count > frame_size:
            raise ValueError("MOC3 RLE run goes past the end of the frame.")
        start = len(out)
        out.extend(bytes([value]) * count)
        i += 2
    if len(out) != frame_size:
        raise ValueError("MOC3 RLE frame decoded short.")
    return bytes(prev[j] ^ out[j] for j in range(frame_size))


def validate_bin_file(path: Path, width: int, height: int) -> None:
    data = path.read_bytes()
    if len(data) < HEADER_SIZE:
        raise ValueError("Output is too short for a header.")

    magic = data[:4]
    frame_count = data[4]
    bytes_per_row = data[5]
    h = data[6]
    fmt = data[7]

    if frame_count == 0:
        raise ValueError("frame_count=0 is invalid.")
    if bytes_per_row != width // 8 or h != height:
        raise ValueError("Header size does not match the expected output size.")

    frame_size = bytes_per_row * h

    if magic == MOC2_MAGIC:
        expected = HEADER_SIZE + frame_count * (1 + frame_size)
        if len(data) != expected:
            raise ValueError(f"Wrong MOC2 size: {len(data)} != {expected}")
        return

    if magic == MOC3_MAGIC:
        if fmt != MOC3_FORMAT_RLE_DELTA:
            raise ValueError("Wrong MOC3 format byte.")
        offset = HEADER_SIZE
        prev = bytes(frame_size)
        for _ in range(frame_count):
            if offset + 3 > len(data):
                raise ValueError("MOC3 frame header is incomplete.")
            delay_units = data[offset]
            comp_len = data[offset + 1] | (data[offset + 2] << 8)
            offset += 3
            if delay_units == 0:
                raise ValueError("Delay byte cannot be 0.")
            if offset + comp_len > len(data):
                raise ValueError("MOC3 compLen goes past the end of the file.")
            comp = data[offset:offset + comp_len]
            offset += comp_len
            prev = decode_moc3_frame(comp, prev, frame_size)
        if offset != len(data):
            raise ValueError("Extra bytes at the end of the MOC3 file.")
        return

    raise ValueError("Magic is not MOC2/MOC3.")


def render_preview_png(frame_data: bytes, width: int, height: int, scale: int = 4) -> Image.Image:
    bytes_per_row = width // 8
    img = Image.new("RGB", (width, height), (0, 0, 0))
    px = img.load()
    for y in range(height):
        row_base = y * bytes_per_row
        for x in range(width):
            if (frame_data[row_base + (x // 8)] >> (x & 7)) & 1:
                px[x, y] = (255, 255, 255)
    return img.resize((width * scale, height * scale), RESAMPLE_NEAREST)


def save_previews(frames: Sequence[EncodedFrame], preview_dir: Path, stem: str, width: int, height: int) -> None:
    preview_dir.mkdir(parents=True, exist_ok=True)
    for i, frame in enumerate(frames):
        render_preview_png(frame.data, width, height).save(preview_dir / f"{stem}_frame_{i:03d}.png")


def parse_delays(value: Optional[str]) -> Optional[List[int]]:
    if not value:
        return None
    try:
        delays = [int(part.strip()) for part in value.split(",") if part.strip()]
    except ValueError as exc:
        raise argparse.ArgumentTypeError("--delays must be comma-separated integers.") from exc
    return [clamp_delay_ms(x) for x in delays]


def expand_patterns(patterns: Sequence[str]) -> List[Path]:
    """Expands patterns like *.gif here, because the Windows shell doesn't."""
    out: List[Path] = []
    for item in patterns:
        matches = glob.glob(item)
        if matches:
            out.extend(Path(m) for m in matches)
        else:
            out.append(Path(item))
    # Keep order, drop duplicates.
    seen = set()
    unique: List[Path] = []
    for p in out:
        key = str(p.resolve()) if p.exists() else str(p)
        if key not in seen:
            seen.add(key)
            unique.append(p)
    return unique


def collect_inputs(args: argparse.Namespace) -> List[Path]:
    if args.files:
        return expand_patterns(args.files)

    input_dir = Path(args.input)
    if not input_dir.is_dir():
        raise ValueError(f"Not an input folder: {input_dir}")

    return sorted(p for p in input_dir.iterdir() if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS)


def combine_images_to_frames(
    paths: Sequence[Path],
    width: int,
    height: int,
    threshold: int,
    invert: bool,
    speed: float,
    fit: str,
    background: Tuple[int, int, int],
    delays_ms: Optional[Sequence[int]],
) -> List[EncodedFrame]:
    if len(paths) > MAX_FRAMES:
        raise ValueError(f"{len(paths)} frames; --combine supports at most {MAX_FRAMES}.")
    frames: List[EncodedFrame] = []
    for i, path in enumerate(paths):
        with Image.open(path) as img:
            base_delay = delays_ms[i] if delays_ms and i < len(delays_ms) else DEFAULT_DELAY_MS
            delay_ms = clamp_delay_ms(int(round(base_delay / speed)))
            data = image_to_oled_bytes(img.copy(), width, height, threshold, invert, fit, background)
            frames.append(EncodedFrame(delay_ms_to_units(delay_ms), data))
    return frames


def convert_one(
    input_path: Path,
    output_dir: Path,
    fmt: str,
    width: int,
    height: int,
    threshold: int,
    invert: bool,
    speed: float,
    fit: str,
    background: Tuple[int, int, int],
    preview_dir: Optional[Path],
    allow_trim: bool,
) -> ConversionResult:
    if not input_path.exists():
        raise FileNotFoundError(input_path)
    if input_path.suffix.lower() not in IMAGE_EXTENSIONS:
        raise ValueError(f"Unsupported file type: {input_path.suffix}")

    frames = read_image_frames(input_path, width, height, threshold, invert, speed, fit, background, allow_trim)
    output_path = output_dir / f"{input_path.stem}.bin"
    bytes_per_row = width // 8

    if fmt == "moc2":
        write_moc2(frames, output_path, bytes_per_row, height)
    elif fmt == "moc3":
        write_moc3(frames, output_path, bytes_per_row, height)
    else:
        raise ValueError(f"Unknown format: {fmt}")

    validate_bin_file(output_path, width, height)

    if preview_dir:
        save_previews(frames, preview_dir, input_path.stem, width, height)

    return ConversionResult(
        input_path=input_path,
        output_path=output_path,
        fmt=fmt.upper(),
        frame_count=len(frames),
        file_size=output_path.stat().st_size,
        total_duration_ms=sum(units_to_ms(f.delay_units) for f in frames),
    )


def parse_background(value: str) -> Tuple[int, int, int]:
    value = value.strip().lower()
    if value == "black":
        return (0, 0, 0)
    if value == "white":
        return (255, 255, 255)
    if value.startswith("#") and len(value) == 7:
        try:
            return (int(value[1:3], 16), int(value[3:5], 16), int(value[5:7], 16))
        except ValueError:
            pass
    raise argparse.ArgumentTypeError("Background must be 'black', 'white' or #RRGGBB.")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Converts GIF/PNG/JPG files to the Mochi 128x64 .bin format.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python gif_to_bin.py
  python gif_to_bin.py --input ./gifs --output ./bins
  python gif_to_bin.py playful.gif smirk.gif speed.gif
  python gif_to_bin.py --format moc2 love.gif
  python gif_to_bin.py --preview --threshold 110 --fit contain face.gif
  python gif_to_bin.py --combine blink blink_0.png blink_1.png blink_2.png --delays 120,40,120
        """.strip(),
    )
    parser.add_argument("files", nargs="*", help="Files or wildcard patterns. If empty, the --input folder is scanned.")
    parser.add_argument("--input", "-i", default=".", help="Input folder. Default: current folder")
    parser.add_argument("--output", "-o", default="bins", help="Output folder. Default: ./bins")
    parser.add_argument("--format", choices=("moc3", "moc2"), default="moc3", help="Output format. Default: moc3")
    parser.add_argument("--raw", action="store_true", help="Shortcut for --format moc2")
    parser.add_argument("--threshold", "-t", type=int, default=DEFAULT_THRESHOLD, help="White threshold 0-255. Default: 128")
    parser.add_argument("--invert", action="store_true", help="Invert black and white")
    parser.add_argument("--resize", type=parse_resize, default=(DEFAULT_WIDTH, DEFAULT_HEIGHT), help="Output size. Default: 128x64")
    parser.add_argument("--fit", choices=("stretch", "contain", "cover"), default="stretch", help="Resize mode. Default: stretch")
    parser.add_argument("--background", type=parse_background, default=(0, 0, 0), help="Background for transparent areas: black, white or #RRGGBB")
    parser.add_argument("--speed", "-s", type=float, default=DEFAULT_SPEED, help="Playback speed. 1.0 normal, 2.0 twice as fast, 0.5 half speed")
    parser.add_argument("--preview", action="store_true", help="Also write frame preview PNGs")
    parser.add_argument("--combine", metavar="OUTPUT_NAME", help="Combine the given still images into one animation .bin")
    parser.add_argument("--delays", metavar="MS,MS,MS", help="Frame durations for --combine")
    parser.add_argument("--allow-trim", action="store_true", help="Resample animations over 255 frames to 255, roughly keeping the total duration")
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    if args.raw:
        args.format = "moc2"

    if not (0 <= args.threshold <= 255):
        print("ERROR: --threshold must be between 0 and 255.")
        return 1
    if args.speed <= 0:
        print("ERROR: --speed must be positive.")
        return 1

    width, height = args.resize
    if (width, height) != (128, 64):
        print("WARNING: the Mochi firmware expects 128x64. Other sizes will be rejected on upload.")

    try:
        input_files = collect_inputs(args)
    except Exception as exc:
        print(f"ERROR: {exc}")
        return 1

    if not input_files:
        print("No files to convert.")
        return 0

    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    preview_dir = output_dir / "previews" if args.preview else None
    if preview_dir:
        preview_dir.mkdir(parents=True, exist_ok=True)

    print("Mochi BIN converter")
    print(f"  Format: {args.format.upper()}")
    print(f"  Size: {width}x{height}")
    print(f"  Threshold: {args.threshold}")
    print(f"  Speed: {args.speed}x")
    print(f"  Fit: {args.fit}")
    print(f"  Output: {output_dir.resolve()}")
    print(f"  Files: {len(input_files)}")
    print()

    if args.combine:
        try:
            delays = parse_delays(args.delays)
            frames = combine_images_to_frames(
                input_files, width, height, args.threshold, args.invert,
                args.speed, args.fit, args.background, delays,
            )
            output_path = output_dir / f"{Path(args.combine).stem}.bin"
            if args.format == "moc2":
                write_moc2(frames, output_path, width // 8, height)
            else:
                write_moc3(frames, output_path, width // 8, height)
            validate_bin_file(output_path, width, height)
            if preview_dir:
                save_previews(frames, preview_dir, output_path.stem, width, height)
            size = output_path.stat().st_size
            duration = sum(units_to_ms(f.delay_units) for f in frames)
            print(f"Done: {output_path.name} | {args.format.upper()} | {len(frames)} frames | {duration} ms | {size:,} bytes")
            return 0
        except Exception as exc:
            print(f"ERROR: {exc}")
            if args.verbose:
                import traceback
                traceback.print_exc()
            return 1

    success = 0
    fail = 0
    total_frames = 0
    total_size = 0

    for path in input_files:
        try:
            result = convert_one(
                path, output_dir, args.format, width, height, args.threshold,
                args.invert, args.speed, args.fit, args.background,
                preview_dir, args.allow_trim,
            )
            success += 1
            total_frames += result.frame_count
            total_size += result.file_size
            print(
                f"  OK: {result.input_path.name} -> {result.output_path.name} "
                f"[{result.fmt}, {result.frame_count}f, {result.total_duration_ms}ms, {result.file_size:,}b]"
            )
        except Exception as exc:
            fail += 1
            print(f"  ERROR: {path} -> {exc}")
            if args.verbose:
                import traceback
                traceback.print_exc()

    print()
    print(f"Done: {success} succeeded, {fail} failed")
    print(f"Total: {total_frames} frames, {total_size:,} bytes ({total_size / 1024:.1f} KB)")
    return 0 if fail == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
