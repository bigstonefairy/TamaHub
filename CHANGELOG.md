# Changelog

## v2.3.0 — 2026-11-10

TamaNav fills the reserved fifth slot: turn-by-turn guidance forwarded from a
phone over BLE.

### Added

- **TamaNav 1.0** (`ota_4`, `0xA20000`): a BLE peripheral that draws a turn
  arrow, the distance to the next manoeuvre, the remaining time and the ETA.
  There is no GPS and no map, so the phone's navigation app does the routing and
  the watch only renders numbers — no Korean font or road parsing on the device.
- BLE service `4e415600-…` with RX `…01` (write) and TX `…02` (notify), a 16-byte
  guidance packet (icon, distance, remaining time, flags, speed, source, ETA,
  trip distance/time) and 8-byte `HELLO` / `STATUS` replies.
- `docs/NAVIGATION.md`: the packet layout, icon table, a Kotlin
  `NotificationListenerService` sketch for Android, and a no-code Tasker +
  AutoNotification recipe.
- Serial commands for hardware-free checks: `DEMO`, `ICON=`, `DIST=`, `REMAIN=`,
  `SPD=`, `SRC=`, `ARRIVE`, `OFFROUTE`, `CLR`, `STATUS`, `BRIGHT=`.

### Changed

- The installer's default layout now fills `ota_4` with TamaNav instead of
  leaving it empty. Choose **비움** for that slot if you do not need it.
- The clock-style ring (radius 224) is used on the nav screens too, so nothing
  sits outside the display's circle.
- `tools/build_all.ps1`, `tools/make_installer.py`, `tools/check_installer.py`
  and the CI workflow know about the fifth app (`tamanav.bin`,
  `manifest-ota4.json`).
- **Fixed** `tools/setup_windows_toolchain.ps1`: running the build from inside
  the `C:\Users\Public\tamahub` junction made it "repoint" the junction onto
  itself and delete it. It now compares against the resolved repository path.

### Firmware

| App | Version | Size | Default slot |
|---|---|---:|---|
| TamaHub | 1.2 | 435,632 B | `0x20000` (`factory`) |
| TamaPoke | 1.36.2 | 783,232 B | `0x220000` (`ota_0`) |
| TamaDigi | 1.0.1 | 1,409,728 B | `0x420000` (`ota_1`) |
| TamaNaruto | 1.2.0 | 1,720,064 B | `0x620000` (`ota_2`) |
| TamaClock | 1.1.0 | 530,352 B | `0x820000` (`ota_3`) |
| TamaNav | 1.0.0 | 721,040 B | `0xA20000` (`ota_4`) |

### Notes

- The Android companion app is not bundled: the repository documents the
  protocol and the two ways to drive it. Until that app exists the slot shows
  the pairing screen, and `DEMO` on the serial port exercises the UI.
- iOS cannot read other apps' notifications, so TamaNav is Android-only for now.
- Hardware checks live in `docs/TESTING.md` §9.

## v2.2.0 — 2026-11-10

TamaClock 1.1: bigger time, themed faces with hourly animations, battery, and
touch / power gestures.

### Added

- **Battery indicator** on the clock face: icon plus percentage, green above
  30 %, orange down to 15 %, red below that. With USB power the cell turns green
  and a bolt is drawn.
- **Four themed faces** — Pokémon, Digimon, Mario and Zelda — drawn procedurally
  so nothing copyrighted ships in the firmware. Each plays a ~20 second
  animation at the top of every hour (`minute == 0 && second < 20`).
- **SD animation packs**: `/tamaclock/<face>.tnm` replaces a built-in scene with
  a raw RGB565 animation streamed from the card on demand. Missing or invalid
  packs fall back to the procedural scene, so the card is never required.
  `tools/make_clock_pack.py` builds a pack from a GIF or a folder of images.
- `docs/CLOCK.md` documents the controls, the pack format and the date offset.
- New serial commands: `FACE=`, `COLOR=`, `BRIGHT=`, `SLEEP`, `WAKE`, `SHOW`,
  `SD`, `PACK`, `AUTO`, `REAL`.

### Changed

- **Layout**: the ring grew from radius 196 to 224, the time from text size 8 to
  12, and every text row is sized to stay inside the circle. The bottom help
  lines no longer clip.
- **Gestures**: swipe up / down changes face, long touch (0.8 s) cycles the
  theme colour, a tap switches 12 / 24 hour, and **PWR short press** blanks the
  panel (touch, PWR, BOOT or serial wakes it). BOOT still returns to TamaHub
  after 2 seconds.
- **Theme colours**: yellow, cyan, green, orange, pink, white, purple.
- **Date**: the games seed a virgin RTC with a fixed date, so the clock used to
  show `2026-01-02`. TamaClock now keeps a whole-day display offset derived from
  its build date and recomputed on every reflash. The RTC is never written, so
  the games do not apply a 14-day offline-progression jump.
- Face, colour, 12/24 h, brightness and the date offset persist in the new NVS
  namespace `tamaclock`.
- The clock caches the RTC at 5 Hz instead of polling I2C on every loop.

### Firmware

| App | Version | Size | Default slot |
|---|---|---:|---|
| TamaHub | 1.2 | 435,632 B | `0x20000` (`factory`) |
| TamaPoke | 1.36.2 | 783,232 B | `0x220000` (`ota_0`) |
| TamaDigi | 1.0.1 | 1,409,728 B | `0x420000` (`ota_1`) |
| TamaNaruto | 1.2.0 | 1,720,064 B | `0x620000` (`ota_2`) |
| TamaClock | 1.1.0 | 530,352 B | `0x820000` (`ota_3`) |

### Notes

- The install flow and URLs are unchanged. Existing saves and the RTC are
  untouched; NVS `tamaclock` starts with defaults on first boot (digital face,
  yellow, 24 h, brightness 200).
- Hardware checks for the new clock live in `docs/TESTING.md` §8.

## v2.1.0 — 2026-11-10

English + Korean UI for TamaPoke and TamaDigi, and a much smaller TamaDigi image.

### Added

- **Korean UI** in TamaPoke and TamaDigi. Every UI string, status line, button,
  dialog and battle message is translated, along with the **Pokémon and Digimon
  names** (386 + 87 entries) and the element/attribute labels. English stays the
  default; the selector is the language pill on TamaPoke's clock screen and the
  `LANG` row on TamaDigi's settings page.
- Korean text is drawn with U8g2's 16 px unifont through Arduino_GFX's U8g2
  support. English keeps the original 5×7 font, pixel for pixel.
- Documentation of the text layer, the NVS key and the checks to run on hardware
  (`docs/PATCHES.md`, `docs/TESTING.md`).

### Changed

- **Only English and Korean remain** (was ES/EN/FR/DE/IT/PT). Dropping five
  copies of every string paid for the Korean glyphs.
- Move names and the help screens stay English in both languages.
- Layouts that used to count characters now measure the rendered width, so long
  Korean labels shrink instead of overflowing. Battle/dialog/name buffers were
  widened for up to 18 UTF-8 bytes per name.
- **TamaDigi no longer embeds its battle backdrops** (`backs.h`, 722 KB). Battles
  reuse the procedural `drawScene()` the explore screen already had.

### Firmware

| App | Version | Size | Default slot |
|---|---|---:|---|
| TamaHub | 1.2 | 435,632 B | `0x20000` (`factory`) |
| TamaPoke | 1.36.2 | 783,232 B | `0x220000` (`ota_0`) |
| TamaDigi | 1.0.1 | 1,409,728 B | `0x420000` (`ota_1`) |
| TamaNaruto | 1.2.0 | 1,720,064 B | `0x620000` (`ota_2`) |
| TamaClock | 1.0.0 | 435,616 B | `0x820000` (`ota_3`) |

### Notes

- Existing saves are untouched. The language lives under the new NVS key
  `lang2`, so the old `lang` value (where `1` meant English) is ignored once and
  the game starts in English after the update.
- The link handshake hashes `sizeof(LinkMon)`, which grew with the name buffers:
  a v2.1 build will refuse to pair with a v2.0 build rather than misread names.
  Update both devices.

---

## v2.0.0 — 2026-11-09

Five-slot, user-arrangeable build with a clock and quieter audio.

### Added

- **v2 partition layout**: `factory` + `ota_0..ota_4`, each 2 MB. NVS (`0x9000`)
  and otadata (`0x19000`) keep their v1 offsets, so reflashing the table without
  Erase preserves every save.
- **User-selectable slot layout**: the installer page lets you place each app in
  any slot. The hub no longer hardcodes offsets; it scans each image for a
  `TAMAHUB_APP:<name>` / `TAMAHUB_VER:<version>` tag and labels the card.
- **TamaClock v1.0.0**: RTC clock, battery/USB status, `SET=`/`TIME=`/`12H`/`24H`/
  `STATUS` serial commands.
- **Installer**: dynamic manifests generated from the slot picker, plus separate
  "layout (keep saves)", "apps only", and "erase" actions.
- **BOOT short press = Back** in every app. The 2 s hold still saves and returns
  to TamaHub. Battles/minigames are never abandoned by a short press.

### Changed

- **Default volume is quieter**: TamaPoke synth master gain 45%; TamaDigi and
  TamaNaruto default to 4/10. A one-time `volfix` migration moves the old default
  7 → 4 and leaves any user-chosen value alone.
- Hub is now **1.2** with five compact cards; serial commands are `1`–`5`.

### Firmware

| App | Version | Size | Default slot (v2) |
|---|---|---:|---|
| TamaHub | 1.2 | 435,632 B | `0x20000` (`factory`) |
| TamaPoke | 1.36.2 | 730,960 B | `0x220000` (`ota_0`) |
| TamaDigi | 1.0.1 | 1,667,968 B | `0x420000` (`ota_1`) |
| TamaNaruto | 1.2.0 | 1,720,064 B | `0x620000` (`ota_2`) |
| TamaClock | 1.0.0 | 435,616 B | `0x820000` (`ota_3`) |

`ota_4` (`0xA20000`) is reserved for TamaNav and is empty until a data source is
chosen.

### Migration

Existing v1 users: run **v2 레이아웃 설치 (세이브 유지)** in the installer.
Do **not** erase; NVS is untouched. Manual esptool offsets are in
[`README.md`](README.md#1-설치) and [`docs/FLASHING.md`](docs/FLASHING.md).

---

## v1.1.1 — 2026-11-09

### Fixed

- **Hub 1.1**: the version title was clipped on the round 466×466 panel
  (`TAMA HUB v1.0` at size 4 near the top). The title is now size 3 at `y=40`
  with an auto-shrink `textCenterFit()` that respects the usable chord width.
  Hub serial/installer version is **1.1**.

### Firmware

| App | Version | Size | Slot |
|---|---|---:|---|
| TamaHub | 1.1 | 435,008 B | `0x20000` |
| TamaPoke | 1.36.2 | 730,608 B | `0x320000` |
| TamaDigi | 1.0.1 | 1,667,744 B | `0x620000` |
| TamaNaruto | 1.2.0 | 1,719,824 B | `0x920000` |

---

## v1.1.0 — 2026-11-09

Four-app build: expanded Pokémon, Digimon, Naruto, and version-aware SD tools.

### Added

- **TamaPoke expanded v1.36.2** (`ota_0`, Gen-3, 386 Pokémon) from
  ShadowEnemyx's `tamapoke-expanded-update` branch. SD needs the base pack
  (#1–251) and the Gen-3 update pack (#252–386), 773 files / ~105 MB total.
- **TamaNaruto v1.2.0** (`ota_2`, slot 3) from `joop0810/tamaruto`; hub card
  `NARUTO`, NVS namespace `tamanaruto`, SD root `/tamaruto/mons`, 67 files
  merged from the four village packs.
- **Version display**: hub screen shows `TAMA HUB v1.0`; the installer page
  shows Hub/Pokémon/Digimon/Naruto versions from the deployed manifests.
- **SD diagnostics**: the page now runs `SDTEST` (512-byte write/read/delete)
  before enabling uploads, shows free space, reports the failing block on a
  failed PUT, and offers per-game **SDCLEAR** to free space without formatting.
- **SD root creation bug fix**: the two-level roots (`/tamapoke/mons`,
  `/tamadigi/mons`, `/tamaruto/mons`) need their parent directory created too.
  A freshly formatted card previously failed every write with `SDTEST ERR`;
  now the parent and child are created on mount, format and before each PUT.
- **Four-app installer**: full install and per-slot updates for `factory`,
  `ota_0`, `ota_1`, `ota_2`; CI builds all four and checks all offsets.
- **Sprite packs**: `web/sd/` now contains 11 bundles (122.7 MB): Pokémon
  base+Gen3, five Digimon regions, four Naruto villages.

### Changed

- `WHOAMI`/`SDSTAT`/`SDTEST`/`SDFORMAT`/`SDCLEAR` added to all three games.
- SD uploader identifies the running game and only enables the matching button.
- Serial reads have a real timeout; a missing/old firmware no longer freezes on
  the first file. GitHub Pages gzip no longer produces a bogus total in the log.
- Digimon/Naruto `PUT` removes an existing file before writing.

### Firmware

| App | Version | Size | Slot |
|---|---|---:|---|
| TamaHub | 1.0 | 435,008 B | `0x20000` |
| TamaPoke | 1.36.2 | 730,608 B | `0x320000` |
| TamaDigi | 1.0.1 | 1,667,744 B | `0x620000` |
| TamaNaruto | 1.2.0 | 1,719,824 B | `0x920000` |

### Known limitations

- Pokémon needs ~105 MB free on the SD card (two packs).
- SD uploads run only while the selected game is running; TamaHub cannot upload.
- Offline progression needs a valid RTC reading (fit the PCF85063 backup cell).
- `ota_2` is now filled; adding a fourth game needs a new partition layout.

---

## v1.0.0 — 2026-11-08

First multi-game release for the Waveshare ESP32-S3-Touch-AMOLED-1.75.

### Added

- **TamaHub** (`factory`): boot-time game selector with three cards
  (Pokémon, Digimon, extra slot), touch and BOOT-button input, RTC clock and
  battery/USB status.
- **Multi-app partition layout** (frozen): 64 KB NVS, 8 KB otadata, then four
  3 MB app slots (`factory`, `ota_0`, `ota_1`, `ota_2`).
- **In-game switching**: hold the lower BOOT/GPIO0 button for 2 s, the game
  shows `SAVING`, forces a save, waits for release, erases otadata and reboots
  into TamaHub.
- **Pause protocol**: TamaHub writes `resumeAt` into the target game's NVS
  namespace; the game's `syncClock()` skips the paused interval. Power-off time
  is still applied as offline progression.
- **Automatic resume**: after a game has been picked once, power-on boots
  straight back into it through otadata.
- **Per-game SD roots**: `/tamapoke/mons` and `/tamadigi/mons`, so the two
  sprite sets can share one card. Serial `PUT` still accepts the upstream
  `mons/…` form and rewrites it.
- **Web installer**: ESP Web Tools page with a full first install and per-slot
  update manifests; every part is written at its own offset and `boot_app0.bin`
  is never used.
- **Web Serial SD uploader**: the installer page can copy the sprite packs over
  USB while the selected game is running. It parses the upstream TPAK bundles,
  merges the five Digimon regions (one shared `thumbs.bin`), resumes interrupted
  uploads and supports manual `.bin` files. The bundles live in `web/sd/` and
  are served from the same origin as the page.
- **Safe connection handling**: the page asks the firmware `WHOAMI`, so it
  refuses to upload to TamaHub or to the wrong game; `SDSTAT` exposes the card
  state; `SDFORMAT` remounts (or formats an unreadable) card. Serial reads now
  have a real timeout, so an unplugged/old firmware no longer freezes the first
  file. The page also shows received MB correctly when GitHub Pages gzips a pack.
- **Digimon PUT safety**: `PUT` removes an existing file before writing, so a
  resumed or retried upload replaces it instead of appending to a partial file.
- **Tooling**: `build_all.ps1`, `check_layout.py`, `check_installer.py`,
  `make_installer.py`, `prepare_sd.ps1`, and a Windows ASCII-path staging
  script for profiles such as `C:\Users\레오`.
- **CI**: GitHub Actions builds all three apps, checks the frozen layout and app
  sizes, validates the installer offsets and deploys the page to Pages.

### Firmware

| App | Version | Size | Slot |
|---|---|---:|---|
| TamaHub | 1.0 | 434,992 B | `0x20000` |
| TamaPoke | 1.18 (upstream `1fff020`) | 853,040 B | `0x320000` |
| TamaDigi | 1.0.1 (upstream `8d1878a`) | 1,666,768 B | `0x620000` |

### Known limitations

- Time spent powered off **while sitting in TamaHub** is treated as paused, not
  offline progression. Normal use (game → hold → hub → game) is unaffected.
- The third slot (`ota_2`) is reserved but has no game in v1.
- SD sprite uploads run over Web Serial **only while TamaPoke or TamaDigi is
  running**; TamaHub does not mount the SD, so the hub screen cannot upload.
- Offline progression and pause timestamps require a valid RTC reading; fit the
  PCF85063 backup cell.
- Arduino IDE's Upload button must not be used: it writes the app to `0x10000`
  and `boot_app0.bin` to `0xE000`, both inside NVS.

### Licensing

Code is MIT. Pokémon and Digimon artwork remains the property of its rights
holders; see `CREDITS.md`. Non-commercial fan project only.
