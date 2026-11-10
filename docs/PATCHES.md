# Patches applied to the upstream games

Both upstream sketches are vendored with the smallest possible changes: the
game logic and save formats are untouched, and the English UI still draws exactly
as it did. The only visual change is the new Korean mode, which is opt-in. Each
change below exists either to share one device or to add Korean.

## Shared (both games)

### `multiboot.h` (new file)

- Reads the lower case button on **BOOT / GPIO0**.
- `mboot::Hold::reached()` fires once after a **2 s** hold.
- `mboot::screenMessage()` draws the `SAVING` / `SAVED` overlay directly on the
  Poke/Digi canvas.
- `mboot::waitRelease()` blocks until the button is released. This is
  deliberate: the ESP32-S3 samples GPIO0 at reset, so restarting while the
  button is down enters the ROM serial-download mode instead of booting the
  hub.
- `mboot::returnToHub()` finds the `otadata` partition, erases it and calls
  `esp_restart()`. An erased otadata makes the 2nd-stage bootloader boot
  `factory` (TamaHub).
- `mboot::hubLooksValid()` reads the first byte of the `factory` partition. If
  it is not a valid app image (0xE9), the game shows `HUB MISSING` and stays
  running instead of erasing otadata and leaving the chip with nothing to boot.

### `sdmon.cpp` – serial commands for the web uploader

All three games answer the commands used by `web/index.html`:

- `WHOAMI` → `TAMAPOKE`, `TAMADIGI` or `TAMARUTO`; the page refuses to upload
  to the hub or to the wrong game.
- `SDSTAT` → `SD READY total=… used=…` or `SD ERR total=… used=…`, then `DONE`.
  The page shows free space and disables uploads until the card is ready.
- `SDTEST` → writes/read-verifies a 512-byte temporary file under the game's
  root, then removes it. This distinguishes a mounted-but-write-protected /
  fake / full card from a path problem.
- `SDFORMAT` → unmounts and remounts the card. The Arduino `SD_MMC.begin(...,
  formatIfMountFailed=true)` only formats a card it cannot mount, so a working
  FAT32 card keeps its data. The page warns that an unreadable card will be
  reformatted.
- `SDCLEAR` → deletes the files under the current game's `mons` root, so a
  partial/full card can be retried without reformatting the whole card.

Digi/Naruto keep their upstream `SD MOUNT [FORMAT]` and `SUM` commands; Poke's
`SDINFO` and `LS` remain.

### `pet.cpp` – `syncClock()`

TamaHub writes `resumeAt = now` into the game's NVS namespace right before
launching it. `syncClock()` now does:

```cpp
uint32_t resumeAt = prefs.getUInt("resumeAt", 0);
if (resumeAt) {
  if (resumeAt > lastSeenEpoch) lastSeenEpoch = resumeAt;  // Poke: seen
  prefs.remove("resumeAt");
}
```

before computing offline minutes. Time spent in the other game is therefore
not applied as offline progression. Power-off time is still counted normally
(see the caveat in the README about time spent sitting in the hub).

### `sdmon.cpp` – per-game SD root

Upstream both games use `/mons/...` for different sprite sets. Each file now
has a `MONS_ROOT` macro:

- TamaPoke: `/tamapoke/mons`
- TamaDigi: `/tamadigi/mons`
- TamaNaruto: `/tamaruto/mons`

All sprite loads, `thumbs.bin`, pack markers, `LS` and serial `PUT` paths use
it. `char path[24/28]` buffers were widened to `[32]` so the longer root cannot
truncate a name. `PUT` still accepts the upstream `mons/xxx.bin` form and
rewrites it to the game's own root, so the original upload tools keep working
without being able to write into the other game's sprites.

`SD_MMC.mkdir()` creates **one level only**. A freshly formatted card has no
`/tamapoke`, `/tamadigi` or `/tamaruto` parent, so `mkdir("/.../mons")` failed
and every write returned `ERR`. Each game now creates the parent and child in
`ensureMonsRoot()`, called from `sdBegin()`, `SDFORMAT`, `SDTEST`, `SDCLEAR` and
`PUT`.

### `i18n.h` / `i18n.cpp` – English + Korean only

- `Lang` is now `{ LANG_EN, LANG_KO }`; the five other tables (ES/FR/DE/IT/PT)
  were deleted, which is what paid for the Korean glyphs. Every string still goes
  through `T(S_...)`, so no call site cares.
- Korean strings are UTF-8 and live in the same `STRINGS[LANG_COUNT][STR_COUNT]`
  table; `T()` picks the row for `gLang`.
- `dexName(dex)` returns the Korean name or the English one from the roster
  (TamaPoke: `DEX_NAMES[2][…]`; TamaDigi: `DEX_NAME_KO[]` next to `DEX_TBL`).
  TamaDigi's `typeName()` / `attrName()` do the same with `ELEMENT_NAME_KO` /
  `ATTR_NAME_KO`.
- The language is stored under the NVS key **`lang2`** (namespaces `tamapoke` /
  `tamadigi`). The old `lang` key is deliberately ignored, because there `1`
  meant English: the first boot after updating starts in English.
- Move names (`moveName()` / `MOVE_NAME_TBL`) and the help screens stay English.

### Text layer (`TamaPoke.ino`, `TamaDigi.ino`)

Arduino_GFX's 5×7 font has no hangul, so Korean uses U8g2's
`u8g2_font_unifont_t_korean2` (16 px) through Arduino_GFX's own U8g2 support.
The upstream drawing code was written against "8 px tall, 6 px per character",
so a thin layer keeps every old call site valid:

- `setSize(n)` divides by 2 in Korean (16 px unifont ≈ 5×7 at scale 2) and sets
  the real scale.
- `setCur(x, y)` adds the font ascent: U8g2 anchors on the baseline, the 5×7 on
  the top-left.
- `textW(s, size)` returns `strlen(s) * 6 * size` in English — the exact old
  arithmetic, so English layouts do not move — and `getTextBounds()` in Korean.
- `printT()` draws the string and, in Korean only, repaints it shifted 1 px to
  fake bold: the scaled-down unifont is thin.
- `applyLangFont()` is the only place that touches the font. It runs after
  `gfx->begin()` and after every `setLang()`.

Auto-shrink decisions that used to count characters (`strlen(name) <= 11`) now
measure `textW`, otherwise a 5-syllable Korean name would be sized by its 10
ASCII-looking bytes. Buffers that receive a name (`btlMsg`, `partyBannerName`,
`Combatant::name`, `LINK_NAME_LEN`, dialog lines) were widened to hold up to
18 UTF-8 bytes.

## TamaPoke (expanded fork v1.36.2)

| File | Change |
|---|---|
| `TamaPoke.ino` | force `TAMAPOKE_GEN3_RELEASE` (386 species); add `TAMAHUB_APP_VERSION` + `TAMAHUB_APP/VER` tags; include `multiboot.h`; `mboot::begin()` in `setup()`; 2 s hold handler in `loop()`; short press calls `physicalBack()`; Korean text layer and `textW()`-based centering/auto-shrink; language pill cycles `EN`/`KO` |
| `i18n.h`, `i18n.cpp` | English + Korean only; `lang2` NVS key; Korean string table; `dexName()` |
| `dex.h` | `DEX_NAMES[2][DEX_COUNT + 1]` (English row untouched + 386 Korean names) and `DEX_LANG_COUNT` |
| `pet.h` | public `saveNow()` |
| `pet.cpp` | `saveNow()` implementation; `resumeAt` in `syncClock()` |
| `sdmon.cpp` | `MONS_ROOT`; `SD_MMC.begin(..., false)`; path rewrite; `WHOAMI`/`SDSTAT`/`SDTEST`/`SDFORMAT`/`SDCLEAR`; safe `PUT` |
| `audio.cpp` | synth master gain 45% (quieter default) |

## TamaDigi

| File | Change |
|---|---|
| `TamaDigi.ino` | add `TAMAHUB_APP/VER` tags; include `multiboot.h`; `mboot::begin()` in `setup()`; 2 s hold handler in `loop()` (save → release → verify hub → restart); short press calls `onSwipeV(1)`; Korean text layer, `textW()`-based layouts, UTF-8-safe battle message wrap, `dexName()` in every name slot |
| `i18n.h`, `i18n.cpp` | English + Korean only; `lang2` NVS key; Korean string table and medal names; `dexName()` |
| `dex.h` | `DEX_NAME_KO` (87 names + the index-0 placeholder) plus `ELEMENT_NAME_KO` / `ATTR_NAME_KO`; English tables untouched |
| `types.h` | `typeName()` / `attrName()` return the active language |
| `battle.h`, `link.h` | name fields widened from 12 to 24 bytes for Korean names; the link build tag hashes `sizeof(LinkMon)`, so old and new peers refuse to pair instead of misreading |
| `backs.h` | **deleted**: the 722 KB of embedded backdrop art is replaced by the procedural `drawScene()` the explore screen already used |
| `pet.cpp` | `resumeAt` in `syncClock()` |
| `sdmon.cpp` | `MONS_ROOT`; pack marker and `thumbs.bin` comparison use it; `PUT` is restricted to `/tamadigi/mons/` (upstream `mons/…` is rewritten); `PUT` removes an existing file first so a resumed/retried upload replaces it instead of appending |
| `audio.cpp` | default volume 4/10; one-time `volfix` migration moves the old default 7 → 4 |

## TamaNaruto

| File | Change |
|---|---|
| `TamaNaruto.ino` | add `TAMAHUB_APP_VERSION` + `TAMAHUB_APP/VER` tags; include `multiboot.h`; `mboot::begin()` in `setup()`; 2 s hold handler in `loop()`; short press calls `onSwipeV(1)` |
| `pet.cpp` | `resumeAt` in `syncClock()` (NVS namespace `tamanaruto`) |
| `sdmon.cpp` | `MONS_ROOT = /tamaruto/mons`; path rewrite; `PUT` removes an existing file; `WHOAMI`/`SDSTAT`/`SDTEST`/`SDFORMAT`/`SDCLEAR` |
| `audio.cpp` | default volume 4/10; one-time `volfix` migration moves the old default 7 → 4 |

## TamaClock (`apps/TamaClock`)

A new, self-contained app that can live in any v2 slot.

| File | Change |
|---|---|
| `TamaClock.ino` | RTC clock via `SensorPCF85063`; battery/USB status; `TAMAHUB_APP/VER` tags; serial `SET=`, `TIME=`, `12H`, `24H`, `STATUS`; 2 s hold returns to hub; short press toggles 12/24h |
| `multiboot.h` | copy of the shared hold/tap + return-to-hub glue |

## TamaHub (`hub/hub.ino`)

| File | Change |
|---|---|
| `hub.ino` | v2: 5 equal slots, `HUB_VERSION 1.2`; scans `TAMAHUB_APP:` / `TAMAHUB_VER:` from each image; compact 5-card layout; serial `1`–`5`, `SLOTS`, `R` |

## Not patched on purpose

- **Save formats / keys**: the pet, party, box and badge formats are unchanged.
  Existing `tamapoke` / `tamadigi` / `tamanaruto` NVS data remains readable. The
  only new key is `lang2` (UI language); the old `lang` key is left untouched.
- **PWR button**: still only power / screen-off / flush. The 4 s AXP2101
  hardware cutoff is untouched.
- **In-game UI**: no screen was added or removed. The language selector that
  upstream already had now offers two languages instead of six; switching is
  still a physical-button gesture for the hub.
- **Internal FATFS**: still unused by all games; sprites come from microSD.

## Manual operations that can still wipe shared data

- TamaDigi keeps its explicit `SD ... FORMAT` serial command. It formats the
  whole card, which also erases `/tamapoke/mons`. It is never called
  automatically: `sdBegin()` defaults to `formatIfUnreadable = false`, and the
  only true comes from that command.
- TamaPoke no longer formats on mount failure at all.
