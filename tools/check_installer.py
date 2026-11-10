#!/usr/bin/env python3
"""Check that the generated ESP Web Tools manifests only touch safe offsets.

A merged image flashed at offset 0 would overwrite NVS and destroy every save,
and `boot_app0.bin` lives at 0xE000 inside the 64 KB NVS partition.  This
script fails the build if either happens, and also verifies that every app part
matches the frozen partition table.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import check_layout  # noqa: E402

NVS_START = 0x9000
NVS_END = 0x19000
BOOT_APP0 = 0xE000

# offset every app image must use, per slot, read from the frozen table
_LAYOUT = {
    r["name"]: r
    for r in check_layout.parse_table(os.path.join(ROOT, "partitions.csv"))
}
APP_OFFSETS = {
    "tamahub.bin": ("factory", _LAYOUT["factory"]["offset"]),
    "tamapoke.bin": ("ota_0", _LAYOUT["ota_0"]["offset"]),
    "tamadigi.bin": ("ota_1", _LAYOUT["ota_1"]["offset"]),
    "tamanaruto.bin": ("ota_2", _LAYOUT["ota_2"]["offset"]),
    "tamaclock.bin": ("ota_3", _LAYOUT["ota_3"]["offset"]),
    "tamanav.bin": ("ota_4", _LAYOUT["ota_4"]["offset"]),
}

REQUIRED_FILES = (
    "manifest.json",
    "manifest-factory.json",
    "manifest-ota0.json",
    "manifest-ota1.json",
    "manifest-ota2.json",
    "manifest-ota3.json",
    "manifest-ota4.json",
    "index.html",
    "bootloader.bin",
    "partitions.bin",
    "tamahub.bin",
    "tamapoke.bin",
    "tamadigi.bin",
    "tamanaruto.bin",
    "tamaclock.bin",
    "tamanav.bin",
)

# Sprite bundles used by the Web Serial uploader, relative to web/dist.
SD_ASSETS = (
    "sd/tamapoke/sprites.pak",
    "sd/tamapoke/sprites-gen3-update.pak",
    "sd/tamadigi/sprites-ver1.pak",
    "sd/tamadigi/sprites-ver2.pak",
    "sd/tamadigi/sprites-ver3.pak",
    "sd/tamadigi/sprites-ver4.pak",
    "sd/tamadigi/sprites-ver5.pak",
    "sd/tamaruto/sprites-konoha.pak",
    "sd/tamaruto/sprites-suna.pak",
    "sd/tamaruto/sprites-kiri.pak",
    "sd/tamaruto/sprites-oto.pak",
)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dist", default=os.path.join(ROOT, "web", "dist"))
    args = ap.parse_args()

    errors: list[str] = []
    for name in REQUIRED_FILES:
        if not os.path.isfile(os.path.join(args.dist, name)):
            errors.append(f"missing {name}")

    for rel in SD_ASSETS:
        path = os.path.join(args.dist, rel)
        if not os.path.isfile(path):
            errors.append(f"missing sprite pack {rel}")
            continue
        if os.path.getsize(path) < 128 * 1024:
            errors.append(f"{rel} is unexpectedly small")
            continue
        with open(path, "rb") as f:
            if f.read(4) != b"TPAK":
                errors.append(f"{rel} does not start with the TPAK magic")

    manifests = sorted(
        p for p in os.listdir(args.dist) if p.startswith("manifest") and p.endswith(".json")
    )
    if not manifests:
        errors.append("no manifest*.json found")

    seen_apps: set[str] = set()
    for name in manifests:
        path = os.path.join(args.dist, name)
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError) as exc:
            errors.append(f"{name}: {exc}")
            continue
        builds = data.get("builds") or []
        if not builds:
            errors.append(f"{name}: no builds")
        for build in builds:
            if build.get("chipFamily") != "ESP32-S3":
                errors.append(f"{name}: chipFamily is {build.get('chipFamily')!r}")
            if name == "manifest.json":
                by_path = {
                    p.get("path"): p.get("offset")
                    for p in build.get("parts") or []
                }
                if by_path.get("bootloader.bin") != 0:
                    errors.append("manifest.json: bootloader.bin must be at offset 0")
            for part in build.get("parts") or []:
                fn = part.get("path", "")
                off = part.get("offset", -1)
                if not isinstance(off, int):
                    errors.append(f"{name}: {fn}: offset is not an integer")
                    continue
                if NVS_START <= off < NVS_END:
                    errors.append(f"{name}: {fn} at 0x{off:X} lands inside NVS")
                if off == BOOT_APP0:
                    errors.append(f"{name}: {fn} at 0xE000 (boot_app0) is inside NVS")
                if off == 0 and name == "manifest.json" and not fn.endswith("bootloader.bin"):
                    errors.append(f"{name}: unexpected file {fn} at offset 0")
                if fn in APP_OFFSETS:
                    slot, expected = APP_OFFSETS[fn]
                    if off != expected:
                        errors.append(
                            f"{name}: {fn} at 0x{off:X}, expected 0x{expected:X}"
                        )
                    seen_apps.add(fn)
                if fn == "partitions.bin" and off != 0x8000:
                    errors.append(f"{name}: partitions.bin at 0x{off:X}, expected 0x8000")

    missing = set(APP_OFFSETS) - seen_apps
    if missing:
        errors.append(f"manifests never flash: {', '.join(sorted(missing))}")

    if errors:
        print("installer check FAILED:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print(f"installer check OK ({len(manifests)} manifests, no NVS writes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
