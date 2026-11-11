# TamaClock v1.1

Standalone clock for TamaHub v2. It uses the same PCF85063 the games keep
running, the AXP2101 for battery state and the CST9217 touch panel. Any 2 MB
`ota_*` slot works; the default layout puts it in `ota_3`.

## Controls

| Gesture | Action |
|---|---|
| Swipe up / down | Shows `DIGITAL ONLY` (themed faces are parked) |
| Long press (0.8 s) | Next theme colour |
| Tap | 12 / 24 hour |
| PWR short press | Screen off / on |
| BOOT short press | 12 / 24 hour (or wake) |
| BOOT hold 2 s | Save and return to TamaHub |

A short toast shows the new face or colour name. Everything (face, colour,
12/24 h, brightness) is stored in NVS namespace `tamaclock`, so it survives a
reflash that keeps NVS.

The ring is drawn at radius 224 on the 466 px panel, and every text row is
sized so its width stays inside the circle at that height. The old layout used
radius 196, which left the bottom help line outside the ring.

## Faces

Only the digital face is enabled in v1.1.1. The themed scenes below still
exist in `scenes.h`, but `THEMED_FACES_ENABLED` is `0`: swiping shows
`DIGITAL ONLY` and `FACE=` forces face 0. The NVS `face` value is left
untouched so a later firmware can restore the chosen face. The plan is to
replace the procedural art with SD packs, which `clockpack.h` already loads.

| # | Face | Scene (parked) |
|---|---|---|
| 0 | `DIGITAL` | Large 12× time, date, weekday, battery, second ticks |
| 1 | `POKEMON` | Poké Ball, splits open once an hour |
| 2 | `DIGIMON` | Egg wobbles, cracks and hatches once an hour |
| 3 | `MARIO` | Platform scene; the walker jumps and a coin pops out |
| 4 | `ZELDA` | Triforce, sword and pedestal; pieces light in sequence |

The digital layout: time y 120 (size 12), date y 228, weekday y 260, hints at
y 348/362 (size 1) and the version at y 396. Those rows keep every glyph corner
inside the radius-224 ring, and the block reads slightly above centre.

The themed faces used to play their animation for the first 20 seconds of every
hour (`minute == 0 && second < 20`). `SHOW` replays it on demand.

### SD animation packs (optional)

Put a pack at `/tamaclock/poke.tnm` (also `digi`, `mario`, `zelda`) and the
firmware draws it instead of the procedural scene. If the file is missing or
invalid, the built-in scene is used, so the clock never depends on the card.

Format (little-endian):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `TNM1` |
| 4 | 1 | format version = 1 |
| 5 | 1 | theme: 1 Pokemon, 2 Digimon, 3 Mario, 4 Zelda (0 = any) |
| 6 | 1 | frames per second, 1–30 |
| 7 | 1 | reserved |
| 8 | 2 | frame count |
| 10 | 2 | frame width (8–280) |
| 12 | 2 | frame height (8–200) |
| 14 | 2 | first hourly frame (0 = no hourly animation) |
| 16 | 2 | hourly frame count |
| 18 | 2 | reserved |
| 20 | … | RGB565 frames, `width × height × 2` bytes each |

Frames are read with `seek()` on demand into one PSRAM buffer, so pack length is
limited by the card, not by RAM. Frames are centred on the scene area at
(233, 238); keep important art away from the outer 8 pixels.

Generate a pack from PNGs or a GIF with:

```
pip install pillow
python tools/make_clock_pack.py art/pokemon.gif -o web/sd/tamaclock/poke.tnm --theme poke --fps 10 --hour 20
```

The last `--hour` frames are used for the hourly show; the earlier frames loop
while idle.

## Date: why the games' clock and the date differ

TamaPoke seeds a virgin RTC with a fixed date (`1767225600`, 2026-01-01) because
it only compares elapsed time. The RTC therefore holds a valid *time of day* but
a meaningless *date*; this is why the clock used to show `2026-01-02`.

TamaClock keeps the RTC untouched (changing it would make the games apply a
large offline-progression jump) and adds a whole-day display offset:

```
offset = (build date) − (RTC date), in whole days
```

The offset is recomputed whenever the firmware build stamp changes, so
reflashing fixes the date again. Because it is a whole number of days the
displayed time of day stays exactly the RTC time the games maintain.

- `AUTO` — recompute the offset from the build date now.
- `REAL` — clear the offset (display follows the RTC exactly).
- `SET=YYYY-MM-DD HH:MM:SS` / `TIME=<epoch>` — move only the displayed clock.

The build date is used, not the flash date. If you flash a build made days
earlier, run `SET=` once (or rebuild) to correct the remaining days.

## Battery

The top row shows an icon plus percentage. The icon is green above 30 %, orange
down to 15 %, red below that; with USB power the cell turns green and a small
bolt is drawn. The PMU is sampled every 5 s, which is also the status the games
cache.

## Sleep

`PWR` short press blanks the panel (`displayOff`) without touching the RTC,
Wi-Fi or NVS; touch, `PWR`, `BOOT`, `WAKE` or any serial traffic wakes it. The
AXP2101 is configured so a 4-second PWR hold still powers the board off.

## Serial (115200)

```
STATUS
SET=YYYY-MM-DD HH:MM:SS
TIME=<epoch>
12H | 24H
AUTO | REAL
FACE=0..4 | FACE=NEXT | FACE=PREV   (forces DIGITAL while themed faces are parked)
COLOR=0..6 | COLOR=NEXT
BRIGHT=1..255
SLEEP | WAKE
SHOW          replay the hourly animation
SD            remount the card
PACK          reload the current face's pack
```
