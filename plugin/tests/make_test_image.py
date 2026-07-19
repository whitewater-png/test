#!/usr/bin/env python3
"""make_test_image.py - Generates a small synthetic PNG test image for
exercising upscale_cli, without any dependency beyond the Python standard
library (uses zlib + struct to write a minimal PNG directly, so this
works even before `pip3 install numpy` has been run).

Usage:
    python3 make_test_image.py output.png [width] [height]

Draws a simple pattern (color gradient + a grid of squares) so that
correctness/tiling issues (seams, misaligned tiles) are visually obvious
when viewing the upscaled output.
"""
from __future__ import annotations

import struct
import sys
import zlib


def make_png(path: str, width: int, height: int) -> None:
    rows = []
    for y in range(height):
        row = bytearray([0])  # filter type 0 (none) for this scanline
        for x in range(width):
            r = (x * 255) // max(1, width - 1)
            g = (y * 255) // max(1, height - 1)
            b = 255 if ((x // 8) + (y // 8)) % 2 == 0 else 64
            a = 255
            row += bytes([r, g, b, a])
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    idat = zlib.compress(raw, 9)

    with open(path, "wb") as f:
        f.write(sig)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(f"Usage: {argv[0]} output.png [width=64] [height=64]", file=sys.stderr)
        return 1
    out_path = argv[1]
    width = int(argv[2]) if len(argv) > 2 else 64
    height = int(argv[3]) if len(argv) > 3 else 64
    make_png(out_path, width, height)
    print(f"Wrote {out_path} ({width}x{height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
