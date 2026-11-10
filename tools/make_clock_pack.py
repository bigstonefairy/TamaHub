#!/usr/bin/env python3
"""Build a TamaClock SD animation pack (.tnm) from PNGs or a GIF.

The pack format is documented in docs/CLOCK.md.  Frames are stored as raw
RGB565 little-endian so the firmware can draw them with
draw16bitRGBBitmap() without a decoder on the device.

Usage:
    pip install pillow
    python tools/make_clock_pack.py art/pokemon.gif -o poke.tnm --theme poke --fps 10 --hour 20
    python tools/make_clock_pack.py art/zelda/ -o zelda.tnm --theme zelda

Copy the result to /tamaclock/<name>.tnm on the SD card, where <name> is one of
poke, digi, mario or zelda.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

THEMES = {"any": 0, "poke": 1, "digi": 2, "mario": 3, "zelda": 4}
MAX_W, MAX_H = 280, 200
DEFAULT_W, DEFAULT_H = 260, 190


def load_frames(path: str):
    try:
        from PIL import Image, ImageSequence
    except ImportError:
        print("Pillow is required: pip install pillow", file=sys.stderr)
        return None

    if os.path.isdir(path):
        names = sorted(
            n
            for n in os.listdir(path)
            if n.lower().endswith((".png", ".jpg", ".jpeg", ".bmp", ".webp"))
        )
        if not names:
            print(f"no images in {path}", file=sys.stderr)
            return None
        frames = [Image.open(os.path.join(path, n)).convert("RGBA") for n in names]
        return frames

    im = Image.open(path)
    return [f.convert("RGBA") for f in ImageSequence.Iterator(im)]


def fit(frame, width: int, height: int):
    from PIL import Image

    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 255))
    scale = min(width / frame.width, height / frame.height)
    w = max(1, int(round(frame.width * scale)))
    h = max(1, int(round(frame.height * scale)))
    resized = frame.resize((w, h), Image.LANCZOS)
    canvas.alpha_composite(resized, ((width - w) // 2, (height - h) // 2))
    return canvas.convert("RGB")


def rgb565_le(pixels: bytes) -> bytes:
    out = bytearray(len(pixels) // 3 * 2)
    j = 0
    for i in range(0, len(pixels) - 2, 3):
        r, g, b = pixels[i], pixels[i + 1], pixels[i + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[j] = v & 0xFF
        out[j + 1] = v >> 8
        j += 2
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", help="GIF or a directory of PNG/JPG frames")
    ap.add_argument("-o", "--output", required=True, help="output .tnm file")
    ap.add_argument("--theme", choices=sorted(THEMES), default="any")
    ap.add_argument("--fps", type=int, default=8)
    ap.add_argument("--hour", type=int, default=0,
                    help="trailing frames used for the hourly animation")
    ap.add_argument("--width", type=int, default=DEFAULT_W)
    ap.add_argument("--height", type=int, default=DEFAULT_H)
    args = ap.parse_args()

    frames = load_frames(args.input)
    if not frames:
        return 2
    if not (8 <= args.width <= MAX_W and 8 <= args.height <= MAX_H):
        print(f"size must be 8..{MAX_W} x 8..{MAX_H}", file=sys.stderr)
        return 2
    if not 1 <= args.fps <= 30:
        print("fps must be 1..30", file=sys.stderr)
        return 2
    if args.hour < 0 or args.hour > len(frames):
        print(f"--hour must be 0..{len(frames)}", file=sys.stderr)
        return 2

    hour_start = len(frames) - args.hour if args.hour else 0
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)

    with open(args.output, "wb") as f:
        header = bytearray(20)
        header[0:4] = b"TNM1"
        header[4] = 1
        header[5] = THEMES[args.theme]
        header[6] = args.fps
        struct.pack_into("<H", header, 8, len(frames))
        struct.pack_into("<H", header, 10, args.width)
        struct.pack_into("<H", header, 12, args.height)
        struct.pack_into("<H", header, 14, hour_start)
        struct.pack_into("<H", header, 16, args.hour)
        f.write(header)
        for frame in frames:
            f.write(rgb565_le(fit(frame, args.width, args.height).tobytes()))

    size = os.path.getsize(args.output)
    print(
        f"{args.output}: {len(frames)} frames, {args.width}x{args.height}, "
        f"{args.fps} fps, {args.hour} hourly -> {size / 1024:.0f} KiB"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
