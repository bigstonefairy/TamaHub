# Changelog

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
