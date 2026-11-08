# Changelog

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
