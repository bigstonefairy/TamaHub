# Upstream sources

Vendored on 2026-11-08, before the local patches described in
[`PATCHES.md`](PATCHES.md).

| Project | Repository | Commit | Date | Firmware |
|---|---|---|---|---|
| TamaPoke | https://github.com/socquique/TamaPoke | `1fff020c158282a384cc9b9357ada111e3eddba8` | 2026-10-27 | v1.18 |
| TamaDigi | https://github.com/joop0810/tamadigi | `8d1878adc102b74be9a92dd9a2cc7619b743c556` | 2026-11-03 | v1.0.1 |

Only the sketch sources (`*.ino`, `*.cpp`, `*.h`), `LICENSE`, `CREDITS.md` and
`CHANGELOG.md` (when upstream has one) are vendored. The upstream `tools/` tree is
not copied; fetch it from the repositories above when you need to regenerate
sprites or SD packs. Upstream `README.md` files are also left out on purpose:
this repository's README describes the multi-game build.

The one exception is the **TPAK sprite bundles** used by the Web Serial SD
uploader: `web/sd/tamapoke/sprites.pak` and `web/sd/tamadigi/sprites-ver1..5.pak`
are byte-for-byte copies of the upstream `web/` bundles. Update them together
with the game sources when upstream changes its sprite set.

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
