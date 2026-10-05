#!/usr/bin/env python3
"""Publish a tagged build into the OTA feed layout the web UI reads.

The web UI's "Check for updates" (data/app.js) fetches
    https://raw.githubusercontent.com/<repo>/ota/Releases/releases.json
and then Releases/V<x.y.z>/firmware.bin + littlefs.bin from the same branch.
This script writes that layout into a checkout of the `ota` branch:

    Releases/releases.json          index: latest + one entry per release
    Releases/V<x.y.z>/firmware.bin  app image (OTA slot)
    Releases/V<x.y.z>/littlefs.bin  web UI image (filesystem partition)
    Releases/V<x.y.z>/manifest.json ESP Web Tools manifest for a USB flash

The index format matches upstream OpenHaldex-C6's Releases/releases.json so
the ported OTA code reads it unchanged.

Usage (from CI, after the release binaries are built):
    python scripts/publish_ota_feed.py --tag v9.01.0 --dist dist \
        --feed ota-feed --changelog CHANGELOG.md
"""

import argparse
import datetime
import hashlib
import json
import re
import shutil
import struct
import sys
from pathlib import Path

PRODUCT = "OpenHaldex-Edge"
PARTITION_MAGIC = b"\xaa\x50"
APP_TYPE, DATA_TYPE = 0x00, 0x01
SUBTYPE_OTA_0, SUBTYPE_SPIFFS = 0x10, 0x82


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def partition_offsets(partitions_bin: Path) -> tuple:
    """Return (app_offset, fs_offset) read from the built partition table."""
    data = partitions_bin.read_bytes()
    app = fs = None
    for i in range(0, len(data), 32):
        entry = data[i:i + 32]
        if len(entry) < 32 or entry[0:2] != PARTITION_MAGIC:
            break
        ptype, subtype = entry[2], entry[3]
        offset = struct.unpack("<I", entry[4:8])[0]
        if (ptype, subtype) == (APP_TYPE, SUBTYPE_OTA_0):
            app = offset
        elif (ptype, subtype) == (DATA_TYPE, SUBTYPE_SPIFFS):
            fs = offset
    if app is None or fs is None:
        sys.exit("error: partitions.bin has no ota_0 or spiffs entry")
    return app, fs


def release_notes(changelog: Path, version: str) -> str:
    """First '> ' summary line under '## v<version>', else a pointer to the changelog."""
    if changelog.is_file():
        lines = changelog.read_text(encoding="utf-8").splitlines()
        heading = re.compile(r"^##\s+v?" + re.escape(version) + r"\b")
        for i, line in enumerate(lines):
            if heading.match(line):
                for nxt in lines[i + 1:]:
                    if nxt.startswith("## "):
                        break
                    if nxt.startswith("> "):
                        return nxt[2:].strip()
                break
    return "See CHANGELOG.md for what changed in v" + version + "."


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True, help="git tag, e.g. v9.01.0 or v9.01.0-beta1")
    ap.add_argument("--dist", required=True, type=Path, help="dir with firmware.bin, littlefs.bin, partitions.bin")
    ap.add_argument("--feed", required=True, type=Path, help="checkout of the ota branch")
    ap.add_argument("--changelog", type=Path, default=Path("CHANGELOG.md"))
    args = ap.parse_args()

    version = args.tag[1:] if args.tag.startswith("v") else args.tag
    channel = "beta" if "-" in version else "stable"

    firmware = args.dist / "firmware.bin"
    littlefs = args.dist / "littlefs.bin"
    partitions = args.dist / "partitions.bin"
    for p in (firmware, littlefs, partitions):
        if not p.is_file():
            sys.exit(f"error: missing {p}")

    rel_dir = args.feed / "Releases" / ("V" + version)
    rel_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(firmware, rel_dir / "firmware.bin")
    shutil.copyfile(littlefs, rel_dir / "littlefs.bin")
    for extra in ("bootloader.bin", "partitions.bin"):
        if (args.dist / extra).is_file():
            shutil.copyfile(args.dist / extra, rel_dir / extra)

    app_off, fs_off = partition_offsets(partitions)
    manifest = {
        "name": PRODUCT,
        "version": version,
        "builds": [{
            "chipFamily": "ESP32-C6",
            "parts": [
                {"path": "bootloader.bin", "offset": 0},
                {"path": "partitions.bin", "offset": 0x8000},
                {"path": "firmware.bin", "offset": app_off},
                {"path": "littlefs.bin", "offset": fs_off},
            ],
        }],
    }
    (rel_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    index_path = args.feed / "Releases" / "releases.json"
    if index_path.is_file():
        index = json.loads(index_path.read_text(encoding="utf-8"))
    else:
        index = {"product": PRODUCT, "latest": None, "releases": []}

    entry = {
        "version": version,
        "channel": channel,
        "date": datetime.date.today().isoformat(),
        "ota": True,
        "notes": release_notes(args.changelog, version),
        "firmware": {"path": f"V{version}/firmware.bin", "size": firmware.stat().st_size, "sha256": sha256(firmware)},
        "filesystem": {"path": f"V{version}/littlefs.bin", "size": littlefs.stat().st_size, "sha256": sha256(littlefs)},
    }
    releases = [r for r in index.get("releases", []) if r.get("version") != version]
    releases.append(entry)

    def vkey(r):
        core = r["version"].split("-")[0]
        return tuple(int(x) for x in core.split(".") if x.isdigit())

    releases.sort(key=vkey, reverse=True)
    index["product"] = PRODUCT
    index["releases"] = releases
    stable = [r for r in releases if r.get("channel") != "beta"]
    index["latest"] = stable[0]["version"] if stable else releases[0]["version"]
    index_path.write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    print(f"published {version} ({channel}) to {rel_dir}; latest = {index['latest']}")


if __name__ == "__main__":
    main()
