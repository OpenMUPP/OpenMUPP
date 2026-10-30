#!/usr/bin/env python3
"""Combines the bootloader and an application image into one flash image.

The combined image starts at the start of the flash (0x08000000). It has
the bootloader, then 0xFF (erased flash) up to the start of the
application, then the application. A debug probe writes a new board in one
step with it. See README.md, "Flash the firmware".

Usage:
    python3 vsl_combine_images.py \\
        --bootloader openmupp-bootloader-mini.bin \\
        --app versaloon-ng-app-8k.bin --app-offset 0x2000 \\
        --bin combined.bin --hex combined.hex

The inputs are binary files. --app-offset is the start of the application,
from the start of the flash. The tool fails when the bootloader is larger
than this offset, or when the combined image is larger than --flash-size.
The unit tests are in test_vsl_combine_images.py.
"""

import argparse
import sys

FLASH_BASE = 0x08000000
# STM32F103C8
DEFAULT_FLASH_SIZE = 64 * 1024
HEX_RECORD_SIZE = 16


class CombineError(Exception):
    pass


def combine(bootloader, app, app_offset, flash_size=DEFAULT_FLASH_SIZE):
    """The combined image as bytes, from the start of the flash."""
    if len(bootloader) == 0:
        raise CombineError("the bootloader image is empty")
    if len(app) == 0:
        raise CombineError("the application image is empty")
    if len(bootloader) > app_offset:
        raise CombineError(
            f"the bootloader ({len(bootloader)} bytes) is larger than the application offset ({app_offset} bytes)")
    size = app_offset + len(app)
    if size > flash_size:
        raise CombineError(f"the combined image ({size} bytes) is larger than the flash ({flash_size} bytes)")
    return bytes(bootloader) + b"\xff" * (app_offset - len(bootloader)) + bytes(app)


def _record(record_type, address, data):
    fields = bytes([len(data), (address >> 8) & 0xFF, address & 0xFF, record_type]) + data
    checksum = (-sum(fields)) & 0xFF
    return ":" + (fields + bytes([checksum])).hex().upper()


def to_ihex(image, base=FLASH_BASE):
    """The image as Intel HEX text, with extended linear address records."""
    lines = []
    upper = None
    for offset in range(0, len(image), HEX_RECORD_SIZE):
        address = base + offset
        if address >> 16 != upper:
            upper = address >> 16
            lines.append(_record(0x04, 0, upper.to_bytes(2, "big")))
        lines.append(_record(0x00, address & 0xFFFF, image[offset:offset + HEX_RECORD_SIZE]))
    lines.append(_record(0x01, 0, b""))
    return "\n".join(lines) + "\n"


def _int(text):
    return int(text, 0)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--bootloader", required=True, help="binary file of the bootloader")
    parser.add_argument("--app", required=True, help="binary file of the application image")
    parser.add_argument("--app-offset", required=True, type=_int,
                        help="start of the application, from the start of the flash")
    parser.add_argument("--flash-size", type=_int, default=DEFAULT_FLASH_SIZE,
                        help=f"size of the flash in bytes (default {DEFAULT_FLASH_SIZE})")
    parser.add_argument("--bin", help="write the combined image as a binary file")
    parser.add_argument("--hex", help="write the combined image as an Intel HEX file")
    args = parser.parse_args(argv)
    if not args.bin and not args.hex:
        parser.error("give --bin, --hex or both")

    with open(args.bootloader, "rb") as f:
        bootloader = f.read()
    with open(args.app, "rb") as f:
        app = f.read()
    try:
        image = combine(bootloader, app, args.app_offset, args.flash_size)
    except CombineError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if args.bin:
        with open(args.bin, "wb") as f:
            f.write(image)
    if args.hex:
        with open(args.hex, "w", newline="\n") as f:
            f.write(to_ihex(image))
    print(f"combined: {len(bootloader)} bytes bootloader, application at 0x{FLASH_BASE + args.app_offset:08X}, "
          f"{len(image)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
