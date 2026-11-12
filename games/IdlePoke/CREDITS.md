# Credits

TamaPoke is a **non-commercial, personal-use** project. It does not sell or
commercially redistribute any copyrighted material. Pokémon and all related
names, designs and characters are trademarks and © of **Nintendo / Game Freak /
The Pokémon Company**.

This project is not affiliated with or endorsed by any of those companies.

## Sprites and data

| Resource | Source | Use in the project |
|---|---|---|
| **All sprites** (idle, walk, sleep, eat, hurt, attack…) | [PMD Sprite Collaboration (PMDCollab/SpriteCollab)](https://github.com/PMDCollab/SpriteCollab) | Mystery-Dungeon-style animated sprites used everywhere: main screen, stat card, minigame, and the Pokédex grid + detail view |
| **Pokémon data #1–386** | [PokéAPI](https://pokeapi.co) | Base HP/ATK/DEF/SPD, types and localized species names used by the generated Dex |

The **SpriteCollab** sprites are the work of its community of artists under their
own terms (Creative Commons Attribution-NonCommercial 4.0). Per-species/per-author
credit is in the original repository's
[tracker.json](https://github.com/PMDCollab/SpriteCollab/blob/master/tracker.json).
Huge thanks to that whole community for an enormous amount of work.

> **Important if you reuse this repo:** the packaged sprite files
> (`tools/sdcard/mons/*.bin`) are derived from the sources above. Don't
> redistribute them commercially. If you publish the project, the clean approach
> is to distribute **only the code and scripts**, and have each user download and
> package the sprites from the original sources with `tools/pack_*.py` (or the web
> installer).

## Korean UI

The English/Korean interface and the Korean species names were added for TamaHub.
The names are the official Korean Pokémon names (© Nintendo / Game Freak / The
Pokémon Company); the UI translations were written for this project. Hangul is
rendered with GNU Unifont through U8g2's font data, painted by Arduino_GFX.

## Software / hardware

| Component | Author / source |
|---|---|
| GFX Library for Arduino | [moononournation](https://github.com/moononournation/Arduino_GFX) |
| SensorLib (CST9217 touch, PCF85063 RTC) | [Lewis He / lewisxhe](https://github.com/lewisxhe/SensorLib) |
| XPowersLib (AXP2101 PMU) | [Lewis He / lewisxhe](https://github.com/lewisxhe/XPowersLib) |
| U8g2 (Korean font data) | [olikraus/u8g2](https://github.com/olikraus/u8g2), BSD-2-Clause |
| GNU Unifont glyphs (`unifont_t_korean2`) | [unifoundry.com/unifont](https://unifoundry.com/unifont/), SIL OFL 1.1 / GPLv2+ with font embedding exception |
| Board and pinout | [Waveshare ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75) |
| Web installer | [ESP Web Tools](https://esphome.github.io/esp-web-tools/) (Nabu Casa) |

## Testing notes

The AXP2101 charging configuration issue was spotted through ShadowEnemyx's
hardware test video: [TikTok](https://pro.tiktok.com/t/ZGdxJB3nr/).

TamaPoke's own code (firmware and tools) is original work.
