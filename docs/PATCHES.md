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

All sprite loads, `thumbs.bin`, pack markers, `LS` and serial `PUT` paths use
it. `char path[24/28]` buffers were widened to `[32]` so the longer root cannot
truncate a name. `PUT` still accepts the upstream `mons/xxx.bin` form and
rewrites it to the game's own root, so the original upload tools keep working
without being able to write into the other game's sprites.

## TamaPoke

| File | Change |
|---|---|
| `TamaPoke.ino` | include `multiboot.h`; `mboot::begin()` in `setup()`; 2 s hold handler in `loop()` (save → release → verify hub → erase otadata → restart) |
| `pet.h` | public `saveNow()` |
| `pet.cpp` | `saveNow()` implementation; `resumeAt` in `syncClock()` |
| `sdmon.cpp` | `MONS_ROOT`; `SD_MMC.begin(..., false)` so a mount failure can no longer format the card |

## TamaDigi

| File | Change |
|---|---|
| `TamaDigi.ino` | include `multiboot.h`; `mboot::begin()` in `setup()`; 2 s hold handler in `loop()` (uses the existing `pet.saveNow()`; save → release → verify hub → restart) |
| `pet.cpp` | `resumeAt` in `syncClock()` |
| `sdmon.cpp` | `MONS_ROOT`; pack marker and `thumbs.bin` comparison use it; `PUT` is restricted to `/tamadigi/mons/` (upstream `mons/…` is rewritten) |

## Not patched on purpose

- **Save formats / keys**: unchanged. Existing `tamapoke` / `tamadigi` NVS
  data remains readable.
- **PWR button**: still only power / screen-off / flush. The 4 s AXP2101
  hardware cutoff is untouched.
- **In-game UI**: no menu entry was added to either game. Switching is a
  physical-button gesture only, so touch targets and layouts stay identical.
- **Internal FATFS**: still unused by both games; sprites come from microSD.

## Manual operations that can still wipe shared data

- TamaDigi keeps its explicit `SD ... FORMAT` serial command. It formats the
  whole card, which also erases `/tamapoke/mons`. It is never called
  automatically: `sdBegin()` defaults to `formatIfUnreadable = false`, and the
  only true comes from that command.
- TamaPoke no longer formats on mount failure at all.
