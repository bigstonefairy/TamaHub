#!/usr/bin/env python3
"""Assemble the GitHub Pages installer from a completed build.

ESP Web Tools writes every `parts` entry at its own offset.  That is the whole
point: a merged image flashed at 0 would overwrite NVS and destroy both saves.
`boot_app0.bin` is deliberately NOT included -- the stock Arduino copy lives at
0xE000, which is inside our 64 KB NVS partition.

Usage:
    python tools/build_all.ps1        # or compile manually
    python tools/make_installer.py
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import check_layout  # noqa: E402

# Every app image is relocatable: the slot is chosen by the web installer, not
# fixed by the build.  The file name is stable because the slot picker in
# web/index.html fetches each bin by name.
APPS = {
    "clock": {
        "folder": os.path.join("apps", "TamaClock"),
        "build": os.path.join("TamaClock", "TamaClock.ino.bin"),
        "bin": "tamaclock.bin",
        "name": "TamaClock",
        "version_macro": "TAMAHUB_APP_VERSION",
    },
    "poke": {
        "folder": os.path.join("games", "TamaPoke"),
        "build": os.path.join("TamaPoke", "TamaPoke.ino.bin"),
        "bin": "tamapoke.bin",
        "name": "TamaPoke",
        "version_macro": "TAMAHUB_APP_VERSION",
    },
    "digi": {
        "folder": os.path.join("games", "TamaDigi"),
        "build": os.path.join("TamaDigi", "TamaDigi.ino.bin"),
        "bin": "tamadigi.bin",
        "name": "TamaDigi",
        "version_macro": "FW_VERSION",
    },
    "idlepoke": {
        "folder": os.path.join("games", "IdlePoke"),
        "build": os.path.join("IdlePoke", "IdlePoke.ino.bin"),
        "bin": "idlepoke.bin",
        "name": "IdlePoke",
        "version_macro": "FW_VERSION",
    },
    "idledigi": {
        "folder": os.path.join("games", "IdleDigi"),
        "build": os.path.join("IdleDigi", "IdleDigi.ino.bin"),
        "bin": "idledigi.bin",
        "name": "IdleDigi",
        "version_macro": "FW_VERSION",
    },
}

APP_SLOTS = ("ota_0", "ota_1", "ota_2", "ota_3", "ota_4")

# First-install plan and the static per-slot update manifests.  Keep in sync
# with DEFAULT_LAYOUT in web/index.html; None leaves the slot empty.  The web
# picker can still assign any app to any slot.
DEFAULT_LAYOUT = ("clock", "idlepoke", "idledigi", "poke", "digi")

# Sprite packs served next to the installer for the Web Serial SD uploader.
# They are the upstream bundles, copied into the repository under web/sd/.
SD_ASSETS = [
    os.path.join("tamapoke", "sprites.pak"),
    os.path.join("tamapoke", "sprites-gen3-update.pak"),
    os.path.join("tamadigi", "sprites-ver1.pak"),
    os.path.join("tamadigi", "sprites-ver2.pak"),
    os.path.join("tamadigi", "sprites-ver3.pak"),
    os.path.join("tamadigi", "sprites-ver4.pak"),
    os.path.join("tamadigi", "sprites-ver5.pak"),
    os.path.join("tamaruto", "sprites-konoha.pak"),
    os.path.join("tamaruto", "sprites-suna.pak"),
    os.path.join("tamaruto", "sprites-kiri.pak"),
    os.path.join("tamaruto", "sprites-oto.pak"),
]


def app_version(path: str) -> str:
    """Read the app description written by the ESP-IDF image header."""
    try:
        with open(path, "rb") as f:
            head = f.read(0x50)
        if len(head) < 0x50 or struct.unpack_from("<I", head, 0x20)[0] != 0xABCD5432:
            return ""
        raw = head[0x30:0x50].split(b"\x00", 1)[0]
        return raw.decode("ascii", "ignore")
    except OSError:
        return ""


# The app description carries a build hash on this toolchain, which is not a
# useful version for the installer.  Read the human version from the sketch
# instead.
def source_version(folder: str, macro: str) -> str:
    ino = os.path.join(ROOT, folder, os.path.basename(folder) + ".ino")
    try:
        with open(ino, "r", encoding="utf-8", errors="ignore") as f:
            text = f.read()
    except OSError:
        return ""
    match = re.search(r'#define\s+' + macro + r'\s+"([^"]+)"', text)
    return match.group(1) if match else ""


def manifest(name: str, version: str, parts: list[dict], erase: bool) -> dict:
    m = {
        "name": name,
        "version": version or "1.0",
        "builds": [{"chipFamily": "ESP32-S3", "parts": parts}],
    }
    if erase:
        m["new_install_prompt_erase"] = True
    return m


def write_json(path: str, data: dict) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2)
        f.write("\n")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build"))
    ap.add_argument("--out", default=os.path.join(ROOT, "web", "dist"))
    args = ap.parse_args()

    table_path = os.path.join(ROOT, "partitions.csv")
    rows = {r["name"]: r for r in check_layout.parse_table(table_path)}
    errors = check_layout.check_table(list(rows.values()))

    os.makedirs(args.out, exist_ok=True)

    # Drop manifests from a previous run so a slot that became empty (or was
    # renamed) cannot leave a stale file behind.
    for name in os.listdir(args.out):
        if name.startswith("manifest") and name.endswith(".json"):
            os.remove(os.path.join(args.out, name))

    copied: dict[str, str] = {}
    versions: dict[str, str] = {}
    sizes: dict[str, int] = {}

    hub_src = os.path.join(args.build_dir, "hub", "hub.ino.bin")
    if not os.path.isfile(hub_src):
        errors.append(f"missing built app: {hub_src}")
    else:
        shutil.copy2(hub_src, os.path.join(args.out, "tamahub.bin"))
        copied["factory"] = "tamahub.bin"
        versions["factory"] = source_version("hub", "HUB_VERSION") or "1.0"
        sizes["factory"] = os.path.getsize(hub_src)

    for key, info in APPS.items():
        src = os.path.join(args.build_dir, info["build"])
        if not os.path.isfile(src):
            errors.append(f"missing built app: {src}")
            continue
        shutil.copy2(src, os.path.join(args.out, info["bin"]))
        copied[key] = info["bin"]
        versions[key] = (
            source_version(info["folder"], info["version_macro"])
            or app_version(src)
            or "1.0"
        )
        sizes[key] = os.path.getsize(src)

    boot_src = os.path.join(args.build_dir, "hub", "hub.ino.bootloader.bin")
    part_src = os.path.join(args.build_dir, "hub", "hub.ino.partitions.bin")
    for slot, src in (("bootloader", boot_src), ("partitions", part_src)):
        if not os.path.isfile(src):
            errors.append(f"missing {slot} image: {src}")
            continue
        shutil.copy2(src, os.path.join(args.out, f"{slot}.bin"))
        copied[slot] = f"{slot}.bin"

    sd_src = os.path.join(ROOT, "web", "sd")
    for rel in SD_ASSETS:
        path = os.path.join(sd_src, rel)
        if not os.path.isfile(path):
            errors.append(f"missing sprite pack: {path}")

    if errors:
        print("cannot build installer:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    # Sizes against the frozen slots.
    if sizes["factory"] > rows["factory"]["size"]:
        print(
            f"factory: {sizes['factory']:,} bytes does not fit "
            f"{rows['factory']['size']:,}",
            file=sys.stderr,
        )
        return 1
    app_slot_size = rows[APP_SLOTS[0]]["size"]
    for key in APPS:
        if sizes[key] > app_slot_size:
            print(
                f"{APPS[key]['name']}: {sizes[key]:,} bytes does not fit "
                f"{app_slot_size:,}",
                file=sys.stderr,
            )
            return 1

    plan = [
        (slot, key)
        for slot, key in zip(APP_SLOTS, DEFAULT_LAYOUT)
        if key is not None
    ]

    full_parts = [
        {"path": copied["bootloader"], "offset": check_layout.BOOTLOADER_OFFSET},
        {"path": copied["partitions"], "offset": check_layout.PARTITION_TABLE_OFFSET},
        {"path": copied["factory"], "offset": rows["factory"]["offset"]},
    ]
    for slot, key in plan:
        full_parts.append(
            {"path": copied[key], "offset": rows[slot]["offset"]}
        )

    hub_version = versions.get("factory") or "1.0"
    write_json(
        os.path.join(args.out, "manifest.json"),
        manifest("TamaHub (first install)", hub_version, full_parts, erase=True),
    )

    # Hub only: leaves the apps, NVS and otadata untouched.
    write_json(
        os.path.join(args.out, "manifest-factory.json"),
        manifest(
            "TamaHub update",
            hub_version,
            [{"path": copied["factory"], "offset": rows["factory"]["offset"]}],
            erase=False,
        ),
    )

    # Per-slot update manifests for the default plan. No erase, no partition
    # table, no bootloader: NVS and the other slots are untouched. Empty
    # default slots get no manifest.
    for slot, key in plan:
        write_json(
            os.path.join(args.out, f"manifest-{slot.replace('_', '')}.json"),
            manifest(
                f"{APPS[key]['name']} update",
                versions.get(key) or "1.0",
                [{"path": copied[key], "offset": rows[slot]["offset"]}],
                erase=False,
            ),
        )

    # The page is static; copy it next to the images.
    web_src = os.path.join(ROOT, "web")
    for name in ("index.html",):
        src = os.path.join(web_src, name)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(args.out, name))

    # Sprite packs for the SD uploader. The uploader fetches sd/... from the
    # same origin, so GitHub Pages needs no CORS handling.
    if os.path.isdir(sd_src):
        shutil.copytree(sd_src, os.path.join(args.out, "sd"), dirs_exist_ok=True)

    sd_bytes = 0
    for rel in SD_ASSETS:
        path = os.path.join(args.out, "sd", rel)
        if os.path.isfile(path):
            sd_bytes += os.path.getsize(path)

    print(f"installer written to {args.out}")
    print(
        f"  {'TamaHub':10} v{versions.get('factory', '?'):<8} "
        f"{sizes['factory']:>9,} bytes  @0x{rows['factory']['offset']:06X}"
    )
    for slot, key in zip(APP_SLOTS, DEFAULT_LAYOUT):
        if key is None:
            print(f"  {'(empty)':10} {'':9} {'':>9}        @0x{rows[slot]['offset']:06X}")
            continue
        print(
            f"  {APPS[key]['name']:10} v{versions.get(key, '?'):<8} "
            f"{sizes[key]:>9,} bytes  @0x{rows[slot]['offset']:06X}"
        )
    print("  bootloader @0x0, partition table @0x8000, boot_app0.bin omitted")
    print(f"  sprite packs  {len(SD_ASSETS)} files, {sd_bytes / (1024 * 1024):.1f} MB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
