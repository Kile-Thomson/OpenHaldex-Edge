#!/usr/bin/env python3
"""Merge the OpenHaldex-C6 build output into one flashable binary.

PlatformIO emits the app, bootloader, partition table and LittleFS image as
separate files, each destined for a different flash offset. This resolves the
real offsets from the built partition table (so it tracks src/partitions_4mb.csv
automatically) and calls `esptool merge_bin` to produce a single
firmware-merged.bin that flashes at offset 0x0:

    esptool.py --chip esp32c6 write_flash 0x0 firmware-merged.bin

or drag-and-drop into a web flasher. Also writes SHA256SUMS.txt over the
individual files and the merged image.

Usage:
    python scripts/merge_firmware.py --build-dir .pio/build/esp32c6-release \
        [--boot-app0 /path/to/boot_app0.bin] [--out firmware-merged.bin]
"""
import argparse
import hashlib
import struct
import subprocess
import sys
from pathlib import Path

# ESP32-C6 flashes the second-stage bootloader at offset 0 (unlike the classic
# ESP32 which uses 0x1000). The partition table always lives at 0x8000.
BOOTLOADER_OFFSET = 0x0
PARTITION_TABLE_OFFSET = 0x8000

# esp-idf partition type/subtype codes we resolve out of the table.
APP_TYPE = 0x00
DATA_TYPE = 0x01
SUBTYPE_OTA_0 = 0x10   # first OTA app slot: where firmware.bin is flashed
SUBTYPE_OTADATA = 0x00  # data/ota: boot selector (boot_app0.bin)
SUBTYPE_SPIFFS = 0x82   # data/spiffs: the LittleFS image slot

PARTITION_MAGIC = b"\xaa\x50"


def parse_partitions(bin_path: Path) -> dict:
    """Return {(type, subtype): (label, offset, size)} from a partitions.bin."""
    data = bin_path.read_bytes()
    parts = {}
    for i in range(0, len(data), 32):
        entry = data[i:i + 32]
        if len(entry) < 32 or entry[0:2] != PARTITION_MAGIC:
            break  # 0xEBEB MD5 marker or padding: end of the table
        ptype, subtype = entry[2], entry[3]
        offset, size = struct.unpack("<II", entry[4:12])
        label = entry[12:28].split(b"\x00", 1)[0].decode("ascii", "replace")
        parts[(ptype, subtype)] = (label, offset, size)
    return parts


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", required=True,
                    help="PlatformIO build dir, e.g. .pio/build/esp32c6-release")
    ap.add_argument("--chip", default="esp32c6")
    ap.add_argument("--flash-mode", default="qio")
    ap.add_argument("--flash-freq", default="80m")
    ap.add_argument("--flash-size", default="4MB")
    ap.add_argument("--boot-app0", default=None,
                    help="optional boot_app0.bin to seed otadata to boot ota_0")
    ap.add_argument("--out", default=None,
                    help="merged output path (default <build-dir>/firmware-merged.bin)")
    args = ap.parse_args()

    build = Path(args.build_dir)
    bootloader = build / "bootloader.bin"
    partitions = build / "partitions.bin"
    firmware = build / "firmware.bin"
    littlefs = build / "littlefs.bin"

    missing = [p.name for p in (bootloader, partitions, firmware, littlefs)
               if not p.is_file()]
    if missing:
        print(f"error: missing build artifacts in {build}: {', '.join(missing)}",
              file=sys.stderr)
        return 1

    parts = parse_partitions(partitions)

    def offset_of(key, what):
        if key not in parts:
            print(f"error: partition table has no {what} entry", file=sys.stderr)
            sys.exit(1)
        return parts[key][1]

    app_off = offset_of((APP_TYPE, SUBTYPE_OTA_0), "ota_0 app")
    fs_off = offset_of((DATA_TYPE, SUBTYPE_SPIFFS), "spiffs/littlefs")

    out = Path(args.out) if args.out else build / "firmware-merged.bin"

    segments = [
        (BOOTLOADER_OFFSET, bootloader),
        (PARTITION_TABLE_OFFSET, partitions),
        (app_off, firmware),
        (fs_off, littlefs),
    ]
    if args.boot_app0 and Path(args.boot_app0).is_file():
        otadata_off = offset_of((DATA_TYPE, SUBTYPE_OTADATA), "otadata")
        segments.append((otadata_off, Path(args.boot_app0)))

    segments.sort(key=lambda s: s[0])

    cmd = [
        "esptool.py", "--chip", args.chip, "merge_bin", "-o", str(out),
        "--flash_mode", args.flash_mode,
        "--flash_freq", args.flash_freq,
        "--flash_size", args.flash_size,
    ]
    for off, path in segments:
        cmd += [hex(off), str(path)]

    print("merging:", " ".join(cmd))
    subprocess.run(cmd, check=True)

    sums_path = build / "SHA256SUMS.txt"
    listed = [bootloader, partitions, firmware, littlefs, out]
    with sums_path.open("w", encoding="ascii") as f:
        for p in listed:
            f.write(f"{sha256(p)}  {p.name}\n")

    print(f"wrote {sums_path}")
    print(f"flashable image: {out} (write at 0x0)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
