#!/usr/bin/env python3
"""Validate the frozen TamaHub partition table and, optionally, the built apps.

The layout is deliberately hard-coded.  NVS lives at 0x9000, so changing any
offset before it silently wipes both games' saves.  This script refuses to let
that happen unnoticed.

Usage:
    python tools/check_layout.py
    python tools/check_layout.py --bin factory=build/hub/hub.ino.bin \
                                 --bin ota_0=build/TamaPoke/TamaPoke.ino.bin
"""

from __future__ import annotations

import argparse
import os
import sys

FLASH_SIZE = 16 * 1024 * 1024

# name -> (offset, size, type, subtype)
EXPECTED = {
    "nvs": (0x9000, 0x10000, "data", "nvs"),
    "otadata": (0x19000, 0x2000, "data", "ota"),
    "factory": (0x20000, 0x200000, "app", "factory"),
    "ota_0": (0x220000, 0x200000, "app", "ota_0"),
    "ota_1": (0x420000, 0x200000, "app", "ota_1"),
    "ota_2": (0x620000, 0x200000, "app", "ota_2"),
    "ota_3": (0x820000, 0x200000, "app", "ota_3"),
    "ota_4": (0xA20000, 0x200000, "app", "ota_4"),
}

# Offsets used by the web installer.  ESP32-S3 bootloader is at 0x0 (not
# 0x1000 as on the ESP32), the partition table at 0x8000.
BOOTLOADER_OFFSET = 0x0
PARTITION_TABLE_OFFSET = 0x8000
BOOT_APP0_OFFSET = 0xE000  # never write this: it is inside the 64 KB NVS


def parse_table(path: str) -> list[dict]:
    rows: list[dict] = []
    with open(path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) < 5:
                raise ValueError(f"{path}:{lineno}: expected 5 columns")
            name, ptype, subtype, offset, size = cols[:5]
            rows.append(
                {
                    "name": name,
                    "type": ptype,
                    "subtype": subtype,
                    "offset": int(offset, 0),
                    "size": int(size, 0),
                    "lineno": lineno,
                }
            )
    return rows


def check_table(rows: list[dict]) -> list[str]:
    errors: list[str] = []
    by_name = {r["name"]: r for r in rows}

    if len(rows) != len(EXPECTED):
        errors.append(
            f"expected {len(EXPECTED)} partitions, found {len(rows)}: "
            f"{', '.join(r['name'] for r in rows)}"
        )

    for name, (offset, size, ptype, subtype) in EXPECTED.items():
        r = by_name.get(name)
        if r is None:
            errors.append(f"missing partition '{name}'")
            continue
        if r["offset"] != offset:
            errors.append(
                f"{name}: offset 0x{r['offset']:X}, expected 0x{offset:X}"
            )
        if r["size"] != size:
            errors.append(f"{name}: size 0x{r['size']:X}, expected 0x{size:X}")
        if r["type"] != ptype:
            errors.append(f"{name}: type {r['type']}, expected {ptype}")
        if r["subtype"] != subtype:
            errors.append(f"{name}: subtype {r['subtype']}, expected {subtype}")

    ordered = sorted(rows, key=lambda r: r["offset"])
    for a, b in zip(ordered, ordered[1:]):
        end = a["offset"] + a["size"]
        if end > b["offset"]:
            errors.append(
                f"{a['name']} (0x{a['offset']:X}+0x{a['size']:X}) overlaps "
                f"{b['name']} at 0x{b['offset']:X}"
            )
        if a["offset"] % 0x1000:
            errors.append(f"{a['name']}: offset is not 4 KB aligned")

    end = max(r["offset"] + r["size"] for r in rows)
    if end > FLASH_SIZE:
        errors.append(f"table ends at 0x{end:X}, beyond 16 MB flash")
    if end > FLASH_SIZE - 0x10000:
        errors.append(f"table ends at 0x{end:X}: less than 64 KB of slack left")

    return errors


def check_bins(rows: list[dict], bins: list[str]) -> list[str]:
    errors: list[str] = []
    by_name = {r["name"]: r for r in rows}
    for spec in bins:
        if "=" not in spec:
            errors.append(f"--bin expects SLOT=PATH, got {spec!r}")
            continue
        slot, path = spec.split("=", 1)
        r = by_name.get(slot)
        if r is None:
            errors.append(f"--bin refers to unknown slot {slot!r}")
            continue
        if not os.path.isfile(path):
            errors.append(f"{slot}: missing file {path}")
            continue
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            magic = f.read(1)
        if magic != b"\xE9":
            errors.append(
                f"{slot}: {path} does not start with the ESP app magic 0xE9"
            )
        if size > r["size"]:
            errors.append(
                f"{slot}: {path} is {size:,} bytes, slot is only "
                f"{r['size']:,} bytes"
            )
    return errors


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    default_csv = os.path.join(os.path.dirname(here), "partitions.csv")

    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default=default_csv)
    ap.add_argument(
        "--bin",
        action="append",
        default=[],
        metavar="SLOT=PATH",
        help="also check that an app fits its slot (repeatable)",
    )
    args = ap.parse_args()

    try:
        rows = parse_table(args.csv)
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    errors = check_table(rows)
    errors += check_bins(rows, args.bin)

    print(f"partition table: {args.csv}")
    print(f"{'name':10} {'type':5} {'subtype':8} {'offset':>10} {'size':>10}  end")
    for r in sorted(rows, key=lambda r: r["offset"]):
        print(
            f"{r['name']:10} {r['type']:5} {r['subtype']:8} "
            f"0x{r['offset']:08X} 0x{r['size']:08X}  0x{r['offset'] + r['size']:08X}"
        )
    print(
        f"\nflash used: 0x{max(r['offset'] + r['size'] for r in rows):X} "
        f"of 0x{FLASH_SIZE:X}"
    )
    print(f"boot_app0.bin target 0x{BOOT_APP0_OFFSET:X} is inside nvs -> never flash it")

    if errors:
        print("\nFAILED:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print("\nOK: layout matches the frozen v2 table.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
