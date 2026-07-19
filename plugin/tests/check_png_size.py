#!/usr/bin/env python3
"""check_png_size.py - Reads a PNG's IHDR chunk and prints/verifies its
dimensions, with no dependency beyond the standard library. Used to
verify upscale_cli's output resolution during manual/CI testing.

Usage:
    python3 check_png_size.py file.png                  # prints "WxH"
    python3 check_png_size.py file.png --expect 256 256  # exits 1 on mismatch
"""
from __future__ import annotations

import struct
import sys


def read_png_size(path: str) -> tuple[int, int]:
    with open(path, "rb") as f:
        sig = f.read(8)
        if sig != b"\x89PNG\r\n\x1a\n":
            raise ValueError(f"{path} is not a PNG file")
        f.read(4)  # IHDR length
        tag = f.read(4)
        if tag != b"IHDR":
            raise ValueError(f"{path}: expected IHDR chunk first, got {tag!r}")
        width, height = struct.unpack(">II", f.read(8))
        return width, height


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(f"Usage: {argv[0]} file.png [--expect W H]", file=sys.stderr)
        return 1

    path = argv[1]
    width, height = read_png_size(path)

    if len(argv) >= 5 and argv[2] == "--expect":
        expected_w, expected_h = int(argv[3]), int(argv[4])
        if (width, height) != (expected_w, expected_h):
            print(f"MISMATCH: {path} is {width}x{height}, expected {expected_w}x{expected_h}")
            return 1
        print(f"OK: {path} is {width}x{height} as expected")
        return 0

    print(f"{width}x{height}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
