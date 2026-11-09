# Upstream sources

Vendored on 2026-11-08, before the local patches described in
[`PATCHES.md`](PATCHES.md).

| Project | Repository | Commit | Date | Firmware |
|---|---|---|---|---|
| TamaPoke (expanded fork) | https://github.com/ShadowEnemyx/TamaPoke (`tamapoke-expanded-update`) | `b91a02ed1e1188ba815beaec1fd7e31cbc2fc7d5` | 2026-11-06 | v1.36.2 (Gen3, 386) |
| TamaDigi | https://github.com/joop0810/tamadigi | `8d1878adc102b74be9a92dd9a2cc7619b743c556` | 2026-11-03 | v1.0.1 |
| TamaNaruto | https://github.com/joop0810/tamaruto | `3756c8ad7be7ec1ca793a4503a65c78a3fa765dc` | 2026-11-04 | v1.2.0 |

The original TamaPoke v1.18 by socquique (`1fff020…`) was used for the first
TamaHub release; slot `ota_0` now carries the expanded Gen-3 fork.

Only the sketch sources (`*.ino`, `*.cpp`, `*.h`), `LICENSE`, `CREDITS.md` and
`CHANGELOG.md` (when upstream has one) are vendored. The upstream `tools/` tree is
not copied; fetch it from the repositories above when you need to regenerate
sprites or SD packs. Upstream `README.md` files are also left out on purpose:
this repository's README describes the multi-game build.

The one exception is the **TPAK sprite bundles** used by the Web Serial SD
uploader: `web/sd/tamapoke/sprites.pak` + `sprites-gen3-update.pak`,
`web/sd/tamadigi/sprites-ver1..5.pak`, and
`web/sd/tamaruto/sprites-{konoha,suna,kiri,oto}.pak` are byte-for-byte copies of
the upstream `web/` bundles. Update them together with the game sources when
upstream changes its sprite set. TamaPoke is split into a base pack (#1–251) and
a Gen-3 update pack (#252–386 + the full 386-entry `thumbs.bin`); both are
required.

## Updating a game

1. Copy the new upstream sources over `games/<Game>/`, keeping `multiboot.h`
   and the patched files.
2. Re-apply the patches from `PATCHES.md` (or diff against the previous
   commit).
3. Build and check the app still fits its 3 MB slot:

   ```powershell
   .\tools\build_all.ps1 -SkipHub
   ```

4. Update this table with the new commit and version.
