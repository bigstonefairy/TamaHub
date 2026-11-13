#!/usr/bin/env python3
"""Render IdlePoke screens from the real SD sprite packs.

The idle firmware reuses the TamaPoke scene (procedural sky/ground), the
TamaClock 5x7 HUD and the PMD sprites already on the card.  This script decodes
the same TPK2 files and lays the frame out with the coordinates from
games/IdlePoke/IdlePoke.ino, so the PNG/GIF in docs/preview/ shows what the
panel will show before flashing.

    python tools/render_idle_preview.py
    python tools/render_idle_preview.py --pokemon 6 --out docs/preview

Only Python + Pillow are needed.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))

from render_clock_preview import (  # noqa: E402
    FONT_DEFAULT,
    ROOT,
    W,
    H,
    Act,
    accent_color,
    draw_text,
    idle_act,
    load_font,
    parse_tpk2,
    read_pak,
    rgb565,
    save_gif,
    text_center,
)

# ---------------------------------------------------------------- firmware ---
CX = 233
HORIZON = 232
GROUND = 400

UI_BG_DAY = 0xF77C
UI_INK = 0x2946
UI_INK_NIGHT = 0xDEFE
UI_TRACK = 0xDE97
UI_BAR_OK = 0x5DCD
UI_BAR_WARN = 0xED07
UI_BAR_BAD = 0xEA87
UI_WHITE = 0xFFFF
UI_BG_NIGHT = 0x0842  # dark outline behind the big clock

TIME_Y = 100
TIME_SIZE = 12
DATE_Y = 78
EVO_Y = 50
BAR_X, BAR_Y, BAR_W, BAR_H = CX - 55, EVO_Y + 8 + 3, 110, 4
BAT_X, BAT_Y, BAT_W, BAT_H = 198, 28, 26, 14
BAT_PCT = 86
LABEL_Y = 416
HINT_Y = 438


def c565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def lerp565(a: int, b: int, i: int, n: int) -> int:
    if n <= 0:
        return a
    ar, ag, ab = (a >> 11) & 31, (a >> 5) & 63, a & 31
    br, bg, bb = (b >> 11) & 31, (b >> 5) & 63, b & 31
    return (((ar + (br - ar) * i // n) << 11)
            | ((ag + (bg - ag) * i // n) << 5)
            | (ab + (bb - ab) * i // n))


# ------------------------------------------------------------------ sprites --
def visible_height(a: Act) -> int:
    if not a.frames or not a.data:
        return a.h
    bottom = a.base or a.h
    for r in range(min(bottom, a.h)):
        row = a.data[r * a.w:(r + 1) * a.w]
        if any(i != 0xFF for i in row):
            return max(1, bottom - r)
    return a.h


def idle_scale(a: Act, idle: Act) -> int:
    span = visible_height(idle)
    s = (175 + span // 2) // span if span else 3
    s = max(2, s)
    while s > 1 and (a.w * s > 400 or span * s > 178):
        s -= 1
    return min(s, 6)


def draw_act(img: Image.Image, pal: list[int], a: Act, idle: Act, frame: int,
             cx: int, ground_y: int = GROUND, sil: bool = False) -> None:
    s = idle_scale(a, idle)
    x0 = cx - a.w * s // 2
    y0 = ground_y - (a.base or a.h) * s
    px = img.load()
    cols = [rgb565(c) for c in pal]
    dark = rgb565(UI_INK)
    off = frame * a.w * a.h
    data = a.data[off:off + a.w * a.h]
    for r in range(a.h):
        row = data[r * a.w:(r + 1) * a.w]
        for c, idx in enumerate(row):
            if idx == 0xFF:
                continue
            col = dark if sil else cols[idx]
            for dy in range(s):
                yy = y0 + r * s + dy
                if not (0 <= yy < H):
                    continue
                for dx in range(s):
                    xx = x0 + c * s + dx
                    if 0 <= xx < W:
                        px[xx, yy] = col


# ------------------------------------------------------------------- scene ---
def draw_scene(img: Image.Image, draw: ImageDraw.ImageDraw, night: bool = False,
               now: int = 0) -> None:
    if night:
        top, bot = c565(0x0C, 0x12, 0x24), c565(0x1E, 0x26, 0x46)
    else:
        top, bot = c565(0x8F, 0xC8, 0xEA), c565(0xDC, 0xEE, 0xE6)
    for y in range(0, HORIZON, 8):
        draw.rectangle([0, y, W, y + 7], fill=rgb565(lerp565(top, bot, y, HORIZON)))

    if night:
        draw.ellipse([370 - 18, 70 - 18, 370 + 18, 70 + 18],
                     fill=rgb565(c565(0xE8, 0xEE, 0xF5)))
        draw.ellipse([378 - 16, 64 - 16, 378 + 16, 64 + 16],
                     fill=rgb565(lerp565(top, bot, 78, HORIZON)))
        for sx, sy in ((120, 140), (330, 120), (370, 210), (95, 230),
                       (280, 90), (160, 95)):
            draw.rectangle([sx, sy, sx + 3, sy + 3], fill=rgb565(UI_WHITE))
    else:
        draw.ellipse([372 - 18, 74 - 18, 372 + 18, 74 + 18],
                     fill=rgb565(c565(0xFF, 0xE7, 0x9F)))
        for k in range(2):
            cx = (now // 50 + k * 250) % 560 - 40
            cy = 70 + k * 34
            for ox, oy, r in ((0, 0, 16), (18, 3, 13), (-15, 4, 12)):
                draw.ellipse([cx + ox - r, cy + oy - r, cx + ox + r, cy + oy + r],
                             fill=rgb565(UI_WHITE))

    soil = c565(0x7E, 0xC0, 0x7F)
    if night:
        soil = lerp565(soil, c565(0x16, 0x1C, 0x30), 9, 16)
    draw.rectangle([0, HORIZON, W, H], fill=rgb565(soil))
    hill = lerp565(soil, c565(0x0C, 0x12, 0x24) if night else UI_WHITE, 3, 16)
    draw.rounded_rectangle([-60, HORIZON - 14, -60 + 586, HORIZON - 14 + 60],
                           radius=30, fill=rgb565(hill))
    dk = lerp565(soil, c565(0x10, 0x18, 0x20), 11 if night else 7, 16)
    for gx in (80, 175, 300, 395):
        for b in (-1, 0, 1):
            draw.rectangle([gx + b * 5, HORIZON + 6,
                            gx + b * 5 + 1, HORIZON + 6 + 8 + (4 if b == 0 else 0) - 1],
                           fill=rgb565(dk))


# --------------------------------------------------------------------- HUD ---
def text_outlined(draw, font, s: str, y: int, size: int, color: int) -> None:
    x = CX - len(s) * 3 * size
    for dy in (-2, 0, 2):
        for dx in (-2, 0, 2):
            if dx or dy:
                draw_text(draw, font, s, x + dx, y + dy, size, UI_BG_NIGHT)
    draw_text(draw, font, s, x, y, size, color)


def draw_battery(draw: ImageDraw.ImageDraw, font: bytes, pct: int = BAT_PCT,
                 usb: bool = False) -> None:
    """Same 26x14 gauge + "%d%%" label TamaClock/IdlePoke draw at the top."""
    ink = rgb565(UI_INK)
    level = UI_BAR_BAD if pct <= 15 else UI_BAR_WARN if pct <= 30 else UI_BAR_OK
    draw.rectangle([BAT_X, BAT_Y, BAT_X + BAT_W, BAT_Y + BAT_H],
                   outline=rgb565(UI_TRACK))
    draw.rectangle([BAT_X + BAT_W, BAT_Y + 4, BAT_X + BAT_W + 3, BAT_Y + 10],
                   fill=rgb565(UI_TRACK))
    if usb:
        draw.rectangle([BAT_X + 2, BAT_Y + 2, BAT_X + BAT_W - 2, BAT_Y + BAT_H - 2],
                       fill=rgb565(UI_TRACK))
        draw.polygon([(BAT_X + 15, BAT_Y + 2), (BAT_X + 9, BAT_Y + 8),
                      (BAT_X + 14, BAT_Y + 8)], fill=ink)
        draw.polygon([(BAT_X + 10, BAT_Y + 12), (BAT_X + 16, BAT_Y + 6),
                      (BAT_X + 11, BAT_Y + 6)], fill=ink)
    elif pct >= 0:
        inner = (BAT_W - 4) * pct // 100
        if inner > 0:
            draw.rectangle([BAT_X + 2, BAT_Y + 2, BAT_X + 2 + inner - 1,
                            BAT_Y + BAT_H - 2], fill=rgb565(level))
    label = f"{pct}%" if pct >= 0 else "--"
    draw_text(draw, font, label, BAT_X + BAT_W + 8, BAT_Y + 2, 2,
              UI_BAR_OK if usb else UI_INK)


def draw_hud(img: Image.Image, draw: ImageDraw.ImageDraw, font: bytes,
             name: str, dex: int, accent: int, remain: str, permille: int,
             night: bool = False, toast: str = "") -> None:
    ink = UI_INK_NIGHT if night else UI_INK
    date = "2026-11-12"

    draw_battery(draw, font)

    text_center(draw, font, f"EVO {remain}", EVO_Y, 1, ink)
    draw.rounded_rectangle([BAR_X, BAR_Y, BAR_X + BAR_W, BAR_Y + BAR_H],
                           radius=2, fill=rgb565(UI_TRACK))
    fw = BAR_W * permille // 1000
    if fw > 2:
        draw.rounded_rectangle([BAR_X, BAR_Y, BAR_X + fw, BAR_Y + BAR_H],
                               radius=2, fill=rgb565(accent))

    text_center(draw, font, date, DATE_Y, 2, ink)
    text_outlined(draw, font, "12:34", TIME_Y, TIME_SIZE, accent)

    label = f"{name}  #{dex:03d}"
    text_center(draw, font, label, LABEL_Y, 1, ink)
    text_center(draw, font, "L: DEX  R: SET  HOLD: SET", 430, 1, ink)

    if toast:
        tw = len(toast) * 6 + 28
        draw.rounded_rectangle([CX - tw // 2, 50, CX + tw // 2, 76],
                               radius=10, fill=rgb565(UI_BG_NIGHT))
        text_center(draw, font, toast, 58, 1, accent)


def main_scene(font: bytes, pal: list[int], act: Act, idle: Act, name: str,
               dex: int, accent: int, frame: int, remain: str, permille: int,
               night: bool = False, toast: str = "", round_mask: bool = False) -> Image.Image:
    img = Image.new("RGB", (W, H), rgb565(UI_BG_DAY))
    draw = ImageDraw.Draw(img)
    draw_scene(img, draw, night=night, now=frame * 120)
    draw_hud(img, draw, font, name, dex, accent, remain, permille,
             night=night, toast=toast)
    draw_act(img, pal, act, idle, frame, CX)
    if round_mask:
        img = apply_round_mask(img)
    return img


def apply_round_mask(img: Image.Image, radius: int = 231) -> Image.Image:
    """Cut the frame to the round AMOLED area and keep the rest black."""
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).ellipse([CX - radius, CX - radius,
                                  CX + radius, CX + radius], fill=255)
    out = Image.new("RGB", (W, H), (0, 0, 0))
    out.paste(img, (0, 0), mask)
    ImageDraw.Draw(out).ellipse(
        [CX - radius, CX - radius, CX + radius, CX + radius],
        outline=rgb565(0x39E7), width=1)
    return out


# ---------------------------------------------------------------- settings ---
def pill(draw: ImageDraw.ImageDraw, font: bytes, x: int, y: int, w: int, h: int,
         label: str, fill: int, text_color: int, size: int = 2) -> None:
    draw.rounded_rectangle([x, y, x + w, y + h], radius=8, fill=rgb565(fill),
                           outline=rgb565(UI_INK), width=1)
    text_center_x(draw, font, label, x + (w - len(label) * 6 * size) // 2,
                  y + 7, size, text_color)


def text_center_x(draw, font, s: str, x: int, y: int, size: int, color: int) -> None:
    draw_text(draw, font, s, x, y, size, color)


def settings_scene(font: bytes, speed: int, armed: bool = False,
                   round_mask: bool = False, digi: bool = False,
                   dex: int = 25, name: str = "PIKACHU") -> Image.Image:
    img = Image.new("RGB", (W, H), rgb565(UI_BG_DAY))
    draw = ImageDraw.Draw(img)
    text_center(draw, font, "SETTINGS", 40, 3, UI_INK)
    draw_text(draw, font, "12:34", CX - 105, 96, 7, UI_INK)

    for bx, lbl in ((104, "-H"), (170, "+H"), (252, "-M"), (318, "+M")):
        draw.rounded_rectangle([bx, 170, bx + 58, 170 + 58], radius=12,
                               fill=rgb565(UI_WHITE), outline=rgb565(UI_INK), width=1)
        draw_text(draw, font, lbl, bx + (58 - len(lbl) * 6 * 4) // 2, 185,
                  4, UI_INK)

    # evolution speed: 1x -> 2x -> 4x -> 8x
    sp = f"SPEED {speed}x"
    pill(draw, font, 236, 244, 196, 30, sp,
         UI_BAR_WARN if speed > 1 else UI_WHITE,
         UI_BG_DAY if speed > 1 else UI_INK)

    if digi:
        pill(draw, font, 24, 244, 200, 30, "BRT 7", UI_WHITE, UI_INK)
        pill(draw, font, 24, 284, 116, 30, "SND ON", UI_BAR_OK, UI_BG_DAY)
        pill(draw, font, 336, 284, 96, 30, "KO >", UI_WHITE, UI_INK)
        text_center(draw, font, f"#{dex:03d} {name}", 330, 1, UI_INK)
        text_center(draw, font, "SAVE OK  SD OK  v0.1.0", 344, 1, UI_TRACK)
        draw.rounded_rectangle([242, 378, 242 + 112, 378 + 44], radius=13,
                               fill=rgb565(UI_BAR_OK))
        text_center_x(draw, font, "OK", 242 + (112 - 36) // 2, 388, 3, UI_BG_DAY)
        if round_mask:
            img = apply_round_mask(img)
        return img

    # release arms on the first tap
    rl = "RELEASE?" if armed else "RELEASE"
    pill(draw, font, 24, 244, 200, 30, rl,
         UI_BAR_BAD if armed else UI_WHITE,
         UI_WHITE if armed else UI_BAR_BAD)

    # sound / power save / language
    pill(draw, font, 24, 284, 116, 30, "SND ALL", UI_BAR_OK, UI_BG_DAY)
    pill(draw, font, 150, 284, 176, 30, "SAVE OFF", UI_WHITE, UI_INK)
    pill(draw, font, 336, 284, 96, 30, "EN >", UI_WHITE, UI_INK)

    text_center(draw, font, "#025 PIKACHU", 330, 1, UI_INK)
    text_center(draw, font, "SAVE OK  SD OK  v0.1.0", 344, 1, UI_TRACK)

    draw.rounded_rectangle([120, 378, 120 + 112, 378 + 44], radius=13,
                           fill=rgb565(UI_WHITE), outline=rgb565(UI_INK), width=1)
    text_center_x(draw, font, "HELP", 120 + (112 - 24) // 2, 391, 2, UI_INK)
    draw.rounded_rectangle([242, 378, 242 + 112, 378 + 44], radius=13,
                           fill=rgb565(UI_BAR_OK))
    text_center_x(draw, font, "OK", 242 + (112 - 36) // 2, 388, 3, UI_BG_DAY)
    if round_mask:
        img = apply_round_mask(img)
    return img


# -------------------------------------------------------------------- main ---
def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "docs" / "preview"))
    ap.add_argument("--font", default=str(FONT_DEFAULT))
    ap.add_argument("--pokemon", type=int, default=25)  # Pikachu
    ap.add_argument("--pak", default="web/sd/tamapoke/sprites.pak")
    ap.add_argument("--dex", type=int, default=None,
                    help="override the sprite number (e.g. 3 = Agumon)")
    ap.add_argument("--name", default=None, help="label shown in the HUD")
    ap.add_argument("--prefix", default="idlepoke",
                    help="output file prefix, e.g. idledigi")
    ap.add_argument("--frames", type=int, default=6)
    args = ap.parse_args()

    font_path = Path(args.font)
    if not font_path.exists():
        raise SystemExit(f"font not found: {font_path} (pass --font)")
    font = load_font(font_path)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    dex = args.dex if args.dex is not None else args.pokemon
    poke_pak = read_pak(ROOT / args.pak)
    key = f"mons/p{dex:03d}.bin"
    if key not in poke_pak:
        raise SystemExit(f"{key} missing from {args.pak}")
    pal, acts = parse_tpk2(poke_pak[key])
    act = idle_act(acts)
    idle = acts[0] if acts and acts[0] else act
    acc = accent_color(pal, act)
    name = args.name or ("PIKACHU" if dex == 25 else f"MON {dex:03d}")
    pre = args.prefix

    still = main_scene(font, pal, act, idle, name, dex, acc, 0,
                       "42:17", 412, round_mask=True)
    still.save(out / f"{pre}-main.png")

    night = main_scene(font, pal, act, idle, name, dex, acc, 0,
                       "42:17", 412, night=True, round_mask=True)
    night.save(out / f"{pre}-night.png")

    toast = main_scene(font, pal, act, idle, name, dex, acc, 0,
                       "59:59", 997, toast="EVOLVED!", round_mask=True)
    toast.save(out / f"{pre}-evolve.png")

    n = min(args.frames, act.frames)
    frames = [
        main_scene(font, pal, act, idle, name, dex, acc, i,
                   "42:17", 412, round_mask=True)
        for i in range(n)
    ]
    save_gif(frames, act.ms[:n], out / f"{pre}-main.gif")

    is_digi = pre == "idledigi"
    settings_scene(font, 2, round_mask=True, digi=is_digi, dex=dex,
                   name=name).save(out / f"{pre}-settings.png")
    settings_scene(font, 8, armed=True, round_mask=True, digi=is_digi,
                   dex=dex, name=name).save(out / f"{pre}-settings-armed.png")

    print(f"{name}: {act.w}x{act.h}, {act.frames} idle frames, "
          f"scale {idle_scale(act, idle)}, accent #{acc:04X}")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
