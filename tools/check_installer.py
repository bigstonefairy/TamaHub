#!/usr/bin/env python3
"""Check that the generated ESP Web Tools manifests only touch safe offsets.

A merged image flashed at offset 0 would overwrite NVS and destroy every save,
and `boot_app0.bin` lives at 0xE000 inside the 64 KB NVS partition.  This
script fails the build if either happens, and also verifies that every app part
matches the frozen partition table.

It also keeps the version numbers honest.  The same release number is shown in
three places -- the installer page header, the TamaHub slot list (read from the
"TAMAHUB_VER:" tag inside the image) and the app's own settings screen -- so a
second literal in a sketch silently made the badge disagree with the page.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import check_layout  # noqa: E402

NVS_START = 0x9000
NVS_END = 0x19000
BOOT_APP0 = 0xE000

# offset every app image must use, per slot, read from the frozen table.
_LAYOUT = {
    r["name"]: r
    for r in check_layout.parse_table(os.path.join(ROOT, "partitions.csv"))
}
# These follow the default layout (web/index.html DEFAULT_LAYOUT and
# tools/make_installer.py DEFAULT_LAYOUT); empty default slots have no manifest.
APP_OFFSETS = {
    "tamahub.bin": ("factory", _LAYOUT["factory"]["offset"]),
    "tamaclock.bin": ("ota_0", _LAYOUT["ota_0"]["offset"]),
    "idlepoke.bin": ("ota_1", _LAYOUT["ota_1"]["offset"]),
    "idledigi.bin": ("ota_2", _LAYOUT["ota_2"]["offset"]),
    "tamapoke.bin": ("ota_3", _LAYOUT["ota_3"]["offset"]),
    "tamadigi.bin": ("ota_4", _LAYOUT["ota_4"]["offset"]),
}

# The slot picker and the one-click batch install are the whole user flow; fail
# the build if a page edit drops them.
INDEX_MARKERS = (
    'id="slot-picker"',
    'id="plan-list"',
    'id="btn-migrate"',
    'id="btn-apps"',
)

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
    "tamaclock.bin",
    "idlepoke.bin",
    "idledigi.bin",
    "tamapoke.bin",
    "tamadigi.bin",
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


# Sketches that carry a release number.  The installer reads it from the
# #define, the hub reads it from the TAMAHUB_VER: tag, and the app prints it on
# its settings screen -- all three have to be the same string.
VERSION_SOURCES = (
    os.path.join("hub", "hub.ino"),
    os.path.join("apps", "TamaClock", "TamaClock.ino"),
    os.path.join("games", "IdlePoke", "IdlePoke.ino"),
    os.path.join("games", "IdleDigi", "IdleDigi.ino"),
    os.path.join("games", "TamaPoke", "TamaPoke.ino"),
    os.path.join("games", "TamaDigi", "TamaDigi.ino"),
)

# A literal (#define FW_VERSION "1.38.0"), not a derived one
# (#define FW_VERSION BASE "-local"), which must not count as a second source.
VERSION_LITERAL = re.compile(
    r'^[ \t]*#define[ \t]+([A-Z0-9_]*(?:VERSION|_VER))[ \t]+"([^"]*)"[ \t]*(?://.*)?$',
    re.MULTILINE,
)


def check_version_literals(errors: list[str]) -> None:
    """Every sketch must contain exactly one release-number literal."""
    for rel in VERSION_SOURCES:
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            errors.append(f"missing sketch {rel}")
            continue
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            text = f.read()
        found = VERSION_LITERAL.findall(text)
        values = {v for _, v in found}
        if not values:
            errors.append(f"{rel}: no version literal found")
        elif len(values) > 1:
            detail = ", ".join(f"{k}={v}" for k, v in found)
            errors.append(
                f"{rel}: {len(values)} different version strings ({detail}); "
                "derive the displayed version from the tagged one"
            )


def tagged_version(path: str) -> str:
    """Version the hub reads out of an app image (TAMAHUB_VER:<version>)."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return ""
    marker = b"TAMAHUB_VER:"
    at = data.rfind(marker)
    if at < 0:
        return ""
    start = at + len(marker)
    end = data.find(b"\x00", start)
    if end < 0:
        return ""
    return data[start:end].decode("ascii", "ignore")


def check_manifest_versions(dist: str, manifests: list[str], errors: list[str]) -> None:
    """The version a single-app manifest advertises must be in the image it writes."""
    for name in manifests:
        path = os.path.join(dist, name)
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            continue  # reported above
        parts = [p for build in data.get("builds") or [] for p in build.get("parts") or []]
        apps = [
            p for p in parts
            if str(p.get("path", "")) in APP_OFFSETS and p.get("path") != "tamahub.bin"
        ]
        if len(apps) != 1:
            continue  # batch manifests list several apps and carry a layout version
        bin_name = str(apps[0].get("path"))
        tagged = tagged_version(os.path.join(dist, bin_name))
        advertised = str(data.get("version") or "")
        if not tagged:
            errors.append(f"{name}: {bin_name} carries no TAMAHUB_VER tag")
        elif tagged != advertised:
            errors.append(
                f"{name}: advertises v{advertised} but {bin_name} says v{tagged} "
                "(stale build, or two version literals in the sketch)"
            )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dist", default=os.path.join(ROOT, "web", "dist"))
    args = ap.parse_args()

    errors: list[str] = []
    for name in REQUIRED_FILES:
        if not os.path.isfile(os.path.join(args.dist, name)):
            errors.append(f"missing {name}")

    index_path = os.path.join(args.dist, "index.html")
    if os.path.isfile(index_path):
        with open(index_path, "r", encoding="utf-8", errors="ignore") as f:
            index_html = f.read()
        for marker in INDEX_MARKERS:
            if marker not in index_html:
                errors.append(f"index.html is missing {marker}")

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

    check_version_literals(errors)
    check_manifest_versions(args.dist, manifests, errors)

    if errors:
        print("installer check FAILED:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print(f"installer check OK ({len(manifests)} manifests, no NVS writes, "
          "versions consistent)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
