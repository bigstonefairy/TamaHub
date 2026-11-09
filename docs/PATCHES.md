# Patches applied to the upstream games

Both upstream sketches are vendored with the smallest possible changes: the
game logic, rendering and save formats are untouched. Each change below exists
only to make two independent firmwares share one device.

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

## TamaPoke (expanded fork v1.36.2)

| File | Change |
|---|---|
| `TamaPoke.ino` | force `TAMAPOKE_GEN3_RELEASE` (386 species); add `TAMAHUB_APP_VERSION`; include `multiboot.h`; `mboot::begin()` in `setup()`; 2 s hold handler in `loop()` |
| `pet.h` | public `saveNow()` |
| `pet.cpp` | `saveNow()` implementation; `resumeAt` in `syncClock()` |
| `sdmon.cpp` | `MONS_ROOT`; `SD_MMC.begin(..., false)`; path rewrite; `WHOAMI`/`SDSTAT`/`SDTEST`/`SDFORMAT`/`SDCLEAR`; safe `PUT` |

## TamaDigi

| File | Change |
|---|---|
| `TamaDigi.ino` | include `multiboot.h`; `mboot::begin()` in `setup()`; 2 s hold handler in `loop()` (uses the existing `pet.saveNow()`; save → release → verify hub → restart) |
| `pet.cpp` | `resumeAt` in `syncClock()` |
| `sdmon.cpp` | `MONS_ROOT`; pack marker and `thumbs.bin` comparison use it; `PUT` is restricted to `/tamadigi/mons/` (upstream `mons/…` is rewritten); `PUT` removes an existing file first so a resumed/retried upload replaces it instead of appending |

## TamaNaruto (slot 3)

| File | Change |
|---|---|
| `TamaNaruto.ino` | add `TAMAHUB_APP_VERSION`; include `multiboot.h`; `mboot::begin()` in `setup()`; 2 s hold handler in `loop()` |
| `pet.cpp` | `resumeAt` in `syncClock()` (NVS namespace `tamanaruto`) |
| `sdmon.cpp` | `MONS_ROOT = /tamaruto/mons`; path rewrite; `PUT` removes an existing file; `WHOAMI`/`SDSTAT`/`SDTEST`/`SDFORMAT`/`SDCLEAR` |
| `hub/hub.ino` | slot 3 is now `ota_2` / `tamanaruto` / `NARUTO` |

## Not patched on purpose

- **Save formats / keys**: unchanged. Existing `tamapoke` / `tamadigi` /
  `tamanaruto` NVS data remains readable.
- **PWR button**: still only power / screen-off / flush. The 4 s AXP2101
  hardware cutoff is untouched.
- **In-game UI**: no menu entry was added to any game. Switching is a
  physical-button gesture only, so touch targets and layouts stay identical.
- **Internal FATFS**: still unused by all games; sprites come from microSD.

## Manual operations that can still wipe shared data

- TamaDigi keeps its explicit `SD ... FORMAT` serial command. It formats the
  whole card, which also erases `/tamapoke/mons`. It is never called
  automatically: `sdBegin()` defaults to `formatIfUnreadable = false`, and the
  only true comes from that command.
- TamaPoke no longer formats on mount failure at all.
