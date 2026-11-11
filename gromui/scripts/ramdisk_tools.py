#!/usr/bin/env python3
"""Small image helpers for the Android SDK emulator test harness."""

import argparse
from pathlib import Path
import struct


def extract_vendor_partition(source: Path, destination: Path) -> None:
    with source.open("rb") as image:
        image.seek(512)
        header = image.read(92)
        if header[:8] != b"EFI PART":
            raise ValueError("Expected a GPT vendor image")

        entries_lba = struct.unpack_from("<Q", header, 72)[0]
        count, entry_size = struct.unpack_from("<II", header, 80)
        if not 128 <= entry_size <= 4096 or count > 1024:
            raise ValueError("Invalid GPT partition table")

        partition = None
        for index in range(count):
            image.seek(entries_lba * 512 + index * entry_size)
            entry = image.read(entry_size)
            if entry[:16] == bytes(16):
                continue
            name = entry[56:entry_size].decode("utf-16le").rstrip("\0")
            if name == "vendor":
                start, end = struct.unpack_from("<QQ", entry, 32)
                partition = (start * 512, (end - start + 1) * 512)
                break
        if partition is None:
            raise ValueError("No vendor partition in GPT image")

        image.seek(partition[0])
        with destination.open("wb") as output:
            remaining = partition[1]
            while remaining:
                block = image.read(min(1024 * 1024, remaining))
                if not block:
                    raise ValueError("Vendor partition ends before GPT says it should")
                output.write(block)
                remaining -= len(block)


def split_first_archive(source: Path, first: Path, remainder: Path) -> None:
    """Keep later concatenated newc archives byte for byte."""
    data = source.read_bytes()
    position = 0
    while True:
        if data[position : position + 6] not in (b"070701", b"070702"):
            raise ValueError(f"Invalid newc header at offset {position}")
        fields = [
            int(data[position + 6 + i * 8 : position + 14 + i * 8], 16)
            for i in range(13)
        ]
        size, name_size = fields[6], fields[11]
        name_start = position + 110
        name = data[name_start : name_start + name_size - 1]
        file_start = (name_start + name_size + 3) & ~3
        position = (file_start + size + 3) & ~3
        if name == b"TRAILER!!!":
            break

    next_archive = position
    while next_archive < len(data) and data[next_archive] == 0:
        next_archive += 1
    if next_archive < len(data) and data[next_archive : next_archive + 6] not in (
        b"070701",
        b"070702",
    ):
        raise ValueError(f"Unexpected data after first cpio archive at {next_archive}")

    first.write_bytes(data[:position])
    remainder.write_bytes(data[next_archive:])


def main() -> None:
    parser = argparse.ArgumentParser()
    subcommands = parser.add_subparsers(dest="command", required=True)
    vendor = subcommands.add_parser("vendor")
    vendor.add_argument("source", type=Path)
    vendor.add_argument("destination", type=Path)
    ramdisk = subcommands.add_parser("split")
    ramdisk.add_argument("source", type=Path)
    ramdisk.add_argument("first", type=Path)
    ramdisk.add_argument("remainder", type=Path)
    args = parser.parse_args()

    if args.command == "vendor":
        extract_vendor_partition(args.source, args.destination)
    else:
        split_first_archive(args.source, args.first, args.remainder)


if __name__ == "__main__":
    main()
