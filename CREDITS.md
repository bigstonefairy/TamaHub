# Credits

This is a fan project that combines two existing open-source Tamagotchi-style
firmwares for one board. The original authors did the hard part.

## Firmware

- **TamaPoke expanded** — © 2026 Quique Tortosa, ShadowEnemyx and the fork
  contributors
  https://github.com/ShadowEnemyx/TamaPoke (branch `tamapoke-expanded-update`) — MIT
- **TamaDigi** — © 2026 Dennis and the TamaDigi contributors
  https://github.com/joop0810/tamadigi — MIT
- **TamaNaruto (TamaRuto)** — © 2026 joop0810 and the TamaNaruto contributors
  https://github.com/joop0810/tamaruto — MIT
- **TamaHub** (game selector, build tooling, installer) — © 2026 TamaHub
  contributors — MIT

## Artwork and data

- Pokémon names, designs and sprites are © Nintendo, Game Freak, Creatures and
  The Pokémon Company.
- PMD-style Pokémon sprites come from the Pokémon Mystery Dungeon sprite
  community (SpriteCollab), distributed under **CC BY-NC**. Non-commercial use
  only.
- Digimon names, designs and sprites are © Bandai, Toei Animation and their
  respective rights holders.
- Naruto names, designs and sprites are © Masashi Kishimoto, Shueisha and Bandai.
  TamaNaruto's Naruto sprites are ripped from *Naruto: Ninja Council 1 & 2*.
- Sprite packs are not authored by this project; they are the same assets the
  upstream projects distribute.
- `web/sd/tamapoke/sprites.pak` + `sprites-gen3-update.pak` (773 unique files),
  `web/sd/tamadigi/sprites-ver1..5.pak` (175 unique files) and
  `web/sd/tamaruto/sprites-{konoha,suna,kiri,oto}.pak` (67 unique files) are
  byte-for-byte copies of the upstream web bundles. They are **not** covered by
  the MIT licence.

## Web installer / SD uploader

- The TPAK bundle format, the `PUT` serial protocol and the original browser
  uploader logic come from **TamaPoke** and **TamaDigi** (MIT). TamaHub's page
  reimplements the client for its own per-game SD roots and adds resume/retry.

## Libraries

- [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX) —
  BSD-2-Clause
- [SensorLib](https://github.com/lewisxhe/SensorLib) — MIT
- [XPowersLib](https://github.com/lewisxhe/XPowersLib) — MIT
- [U8g2](https://github.com/olikraus/u8g2) — BSD-2-Clause (TamaPoke CJK font data)
- [ESP32 Arduino core](https://github.com/espressif/arduino-esp32) — LGPL-2.1
- [ESP Web Tools](https://espressif.github.io/esp-web-tools/) — Apache-2.0

## Licence note

The **code** in this repository is MIT. The **artwork** is not: it remains the
property of its rights holders or under CC BY-NC. This project must stay
non-commercial. Do not sell devices with these assets preloaded, do not put the
installer behind a paywall, and do not use it in advertising.
