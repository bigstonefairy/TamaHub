#!/usr/bin/env python3
"""Render TamaClock preview images from the real SD sprite packs.

The clock firmware draws its creature faces with the sprites the games already
put on the card, using Arduino_GFX's 5x7 font.  This script decodes the same
TPK2 files out of the TPAK bundles, maps colours with the same RGB565 palette,
and lays the scene out with the exact coordinates from TamaClock.ino, so the
PNG/GIF in docs/preview/ shows what the panel will show before flashing.

    python tools/render_clock_preview.py
    python tools/render_clock_preview.py --out docs/preview --pokemon 25 --digimon 3

Only Python + Pillow are needed.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent

W = H = 466
RING_R = 224
RING_COLOR = 0x39E7
BG = 0x0000
FG = 0xFFFF
DIM = 0x8410
WARN = 0xFD20
GO = 0x07E0
LINE = 0x39E7
SHADOW = 0x18C3
ACCENT = 0xFFE0  # THEMES[0] yellow

# Layout copied from TamaClock.ino (drawCreatureFace / drawDigitalFace).
GROUND_Y = 398
TIME_Y = 58
TIME_SIZE = 9
DATE_Y = 128
DATE_SIZE = 2
LABEL_Y = 416
HINT_Y = 438
BATTERY_X, BATTERY_Y = 199, 40
CREATURE_BATTERY_Y = 24

FONT_DEFAULT = Path(
    r"C:\Users\Public\ArduinoUser\libraries\GFX_Library_for_Arduino"
    r"\src\font\glcdfont.h"
)


# ------------------------------------------------------------------ helpers --
def rgb565(c: int) -> tuple[int, int, int]:
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)


def load_font(path: Path) -> bytes:
    text = path.read_text(encoding="utf-8", errors="replace")
    marker = text.find("font[]")
    if marker < 0:
        raise SystemExit(f"no font[] array in {path}")
    body = text[text.find("{", marker) + 1 :]
    body = body[: body.find("}")]
    data = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body))
    if len(data) < 1280:
        raise SystemExit(f"font table too short in {path} ({len(data)} bytes)")
    return data[:1280]


def draw_text(draw: ImageDraw.ImageDraw, font: bytes, s: str, x: int, y: int,
              size: int, color: int) -> None:
    col = rgb565(color)
    for n, ch in enumerate(s):
        code = ord(ch) & 0xFF
        base = code * 5
        ox = x + n * 6 * size
        for i in range(5):
            bits = font[base + i]
            for j in range(8):
                if bits & (1 << j):
                    draw.rectangle(
                        [ox + i * size, y + j * size,
                         ox + i * size + size - 1, y + j * size + size - 1],
                        fill=col,
                    )


def text_center(draw, font, s, y, size, color) -> None:
    draw_text(draw, font, s, (W - len(s) * 6 * size) // 2, y, size, color)


def draw_battery(draw: ImageDraw.ImageDraw, font: bytes, pct: int = 78,
                 y: int = BATTERY_Y) -> None:
    x, w, h = BATTERY_X, 26, 14
    draw.rectangle([x, y, x + w - 1, y + h - 1], outline=rgb565(DIM))
    draw.rectangle([x + w, y + 4, x + w + 2, y + 9], fill=rgb565(DIM))
    inner = (w - 4) * pct // 100
    if inner > 0:
        draw.rectangle([x + 2, y + 2, x + 1 + inner, y + h - 3],
                       fill=rgb565(GO))
    draw_text(draw, font, f"{pct}%", x + w + 8, y + 1, 2, DIM)


def draw_ring(draw: ImageDraw.ImageDraw) -> None:
    cx = cy = W // 2
    draw.ellipse([cx - RING_R, cy - RING_R, cx + RING_R, cy + RING_R],
                 outline=rgb565(RING_COLOR))


def draw_toast(draw, font) -> None:
    # The firmware only shows a toast for ~0.9 s after a gesture; the still
    # preview keeps it clean, so this stays unused unless asked for.
    pass


# ---------------------------------------------------------------- TPAK/TPK2 --
def read_pak(path: Path) -> dict[str, bytes]:
    data = path.read_bytes()
    if data[:4] != b"TPAK":
        raise SystemExit(f"{path} is not TPAK")
    count = struct.unpack_from("<H", data, 4)[0]
    pos = 6
    items = []
    for _ in range(count):
        nlen = data[pos]
        pos += 1
        name = data[pos : pos + nlen].decode("utf-8", "replace")
        pos += nlen
        size = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        items.append((name, size))
    out = {}
    for name, size in items:
        out[name] = data[pos : pos + size]
        pos += size
    return out


class Act:
    __slots__ = ("w", "h", "frames", "ms", "data", "base", "top")

    def __init__(self, w, h, ms, data):
        self.w = w
        self.h = h
        self.frames = len(ms)
        self.ms = ms
        self.data = data
        self.base = 0
        self.top = 0


def parse_tpk2(blob: bytes) -> tuple[list[int], list[Act]]:
    if blob[:4] != b"TPK2":
        raise SystemExit("not a TPK2 sprite")
    n_acts = blob[4]
    pal_count = struct.unpack_from("<H", blob, 5)[0]
    pal = list(struct.unpack_from(f"<{pal_count}H", blob, 7))
    pos = 7 + pal_count * 2
    acts: list[Act | None] = [None] * 12
    for _ in range(n_acts):
        act_id, w, h, nf = blob[pos], blob[pos + 1], blob[pos + 2], blob[pos + 3]
        pos += 4
        ms = list(struct.unpack_from(f"<{nf}H", blob, pos))
        pos += nf * 2
        size = w * h * nf
        acts[act_id] = Act(w, h, ms, blob[pos : pos + size])
        pos += size
    for act in acts:
        if act is None:
            continue
        base = 1
        top = act.h
        for f in range(act.frames):
            frame = act.data[f * act.w * act.h : (f + 1) * act.w * act.h]
            for r in range(act.h - 1, -1, -1):
                row = frame[r * act.w : (r + 1) * act.w]
                if any(v != 0xFF for v in row):
                    base = max(base, r + 1)
                    break
            for r in range(act.h):
                row = frame[r * act.w : (r + 1) * act.w]
                if any(v != 0xFF for v in row):
                    top = min(top, r)
                    break
        act.base = base
        act.top = top
    return pal, acts


def idle_act(acts: list[Act | None]) -> Act:
    act = acts[0] if acts and acts[0] else next(a for a in acts if a)
    return act


def sprite_scale(idle: Act, act: Act, max_scale: int = 6) -> int:
    span = idle.base - idle.top if idle.base > idle.top else idle.h
    s = (235 + span // 2) // span if span else 3
    s = max(s, 2)
    while s > 1 and (act.w * s > 400 or span * s > 245):
        s -= 1
    return min(s, max_scale)


def draw_sprite(img: Image.Image, pal: list[int], act: Act, idle: Act,
                frame: int, cx: int, ground_y: int, max_scale: int = 6) -> None:
    s = sprite_scale(idle, act, max_scale)
    x0 = cx - act.w * s // 2
    y0 = ground_y - (act.base or act.h) * s
    px = img.load()
    cols = [rgb565(c) for c in pal]
    frame_data = act.data[frame * act.w * act.h : (frame + 1) * act.w * act.h]
    for r in range(act.h):
        row = frame_data[r * act.w : (r + 1) * act.w]
        for c, idx in enumerate(row):
            if idx == 0xFF:
                continue
            col = cols[idx]
            for dy in range(s):
                yy = y0 + r * s + dy
                if 0 <= yy < H:
                    for dx in range(s):
                        xx = x0 + c * s + dx
                        if 0 <= xx < W:
                            px[xx, yy] = col


# ------------------------------------------------------------------- scenes --
def creature_scene(font: bytes, pal: list[int], act: Act, idle: Act, label: str,
                   frame: int, time_s: str = "12:34",
                   date_s: str = "2026-11-12") -> Image.Image:
    img = Image.new("RGB", (W, H), rgb565(BG))
    draw = ImageDraw.Draw(img)
    draw_ring(draw)
    draw_battery(draw, font, y=CREATURE_BATTERY_Y)

    text_center(draw, font, time_s, TIME_Y, TIME_SIZE, ACCENT)
    text_center(draw, font, f"{date_s}  {label.split()[0]}", DATE_Y, DATE_SIZE, DIM)

    # fillEllipse(233, ground + 6, 74, 9)
    cx, cy, rx, ry = W // 2, GROUND_Y + 6, 74, 9
    draw.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=rgb565(SHADOW))

    draw_sprite(img, pal, act, idle, frame, W // 2, GROUND_Y)
    text_center(draw, font, label, LABEL_Y, 1, DIM)
    text_center(draw, font, "HOLD PET: NEXT   SWIPE: FACE", HINT_Y, 1, DIM)
    return img


def digital_scene(font: bytes, sec: int = 34) -> Image.Image:
    img = Image.new("RGB", (W, H), rgb565(BG))
    draw = ImageDraw.Draw(img)
    draw_ring(draw)
    draw_battery(draw, font)

    text_center(draw, font, "12:34", 120, 12, ACCENT)

    # 60 tick marks, every other one, radius 213.
    import math
    cx = cy = W // 2
    for i in range(0, 60, 2):
        a = math.radians(i * 6 - 90)
        x = cx + int(math.cos(a) * 213)
        y = cy + int(math.sin(a) * 213)
        r = 3 if i <= sec else 2
        col = rgb565(ACCENT if i <= sec else LINE)
        draw.ellipse([x - r, y - r, x + r, y + r], fill=col)

    text_center(draw, font, "2026-11-12", 228, 3, FG)
    text_center(draw, font, "THURSDAY", 260, 2, DIM)
    text_center(draw, font, "SWIPE: POKEMON / DIGIMON", 348, 1, DIM)
    text_center(draw, font, "TAP: 12/24   HOLD: COLOR", 362, 1, DIM)
    text_center(draw, font, "TamaClock v1.2.0", 396, 1, DIM)
    return img


def save_gif(frames: list[Image.Image], ms: list[int], path: Path) -> None:
    out = [f.convert("P", palette=Image.Palette.ADAPTIVE, colors=64) for f in frames]
    out[0].save(path, save_all=True, append_images=out[1:],
                duration=[max(40, m) for m in ms], loop=0, optimize=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "docs" / "preview"))
    ap.add_argument("--font", default=str(FONT_DEFAULT))
    ap.add_argument("--pokemon", type=int, default=25)   # Pikachu
    ap.add_argument("--digimon", type=int, default=3)    # Agumon
    ap.add_argument("--frames", type=int, default=6)
    args = ap.parse_args()

    font_path = Path(args.font)
    if not font_path.exists():
        raise SystemExit(f"font not found: {font_path} (pass --font)")
    font = load_font(font_path)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    poke_pak = read_pak(ROOT / "web/sd/tamapoke/sprites.pak")
    digi_pak = read_pak(ROOT / "web/sd/tamadigi/sprites-ver1.pak")

    def entry(pak: dict[str, bytes], dex: int) -> bytes:
        key = f"mons/p{dex:03d}.bin"
        if key not in pak:
            raise SystemExit(f"{key} missing from pack")
        return pak[key]

    jobs = [
        ("pokemon", "POKEMON", entry(poke_pak, args.pokemon), args.pokemon),
        ("digimon", "DIGIMON", entry(digi_pak, args.digimon), args.digimon),
    ]

    for slug, label, blob, dex in jobs:
        pal, acts = parse_tpk2(blob)
        act = idle_act(acts)
        idle = acts[0] if acts and acts[0] else act
        n = min(args.frames, act.frames)
        still = creature_scene(font, pal, act, idle, f"{label} #{dex:03d}", 0)
        still.save(out / f"clock-{slug}.png")
        frames = [
            creature_scene(font, pal, act, idle, f"{label} #{dex:03d}", i)
            for i in range(n)
        ]
        save_gif(frames, act.ms[:n], out / f"clock-{slug}.gif")
        print(f"{label}: {act.w}x{act.h}, {act.frames} idle frames, "
              f"scale {sprite_scale(idle, act)}")

    digital_scene(font).save(out / "clock-digital.png")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
