#!/usr/bin/env python3
import struct
import sys
from pathlib import Path

from PIL import Image


def shift_for_mask(mask: int) -> int:
    shift = 0
    while mask and (mask & 1) == 0:
        shift += 1
        mask >>= 1
    return shift


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: xwd_to_png.py input.xwd output.png")
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    data = src.read_bytes()
    header = struct.unpack(">25I", data[:100])
    (
        header_size,
        _file_version,
        _pixmap_format,
        _pixmap_depth,
        width,
        height,
        _xoffset,
        byte_order,
        _bitmap_unit,
        _bitmap_bit_order,
        _bitmap_pad,
        bits_per_pixel,
        bytes_per_line,
        _visual_class,
        red_mask,
        green_mask,
        blue_mask,
        _bits_per_rgb,
        _colormap_entries,
        ncolors,
        _window_width,
        _window_height,
        _window_x,
        _window_y,
        _window_bdrwidth,
    ) = header
    if bits_per_pixel != 32:
        raise RuntimeError(f"only 32bpp XWD is supported, got {bits_per_pixel}")

    pixel_offset = header_size + ncolors * 12
    endian = "<" if byte_order == 0 else ">"
    rshift = shift_for_mask(red_mask)
    gshift = shift_for_mask(green_mask)
    bshift = shift_for_mask(blue_mask)
    out = bytearray(width * height * 3)
    pos = 0
    for y in range(height):
        row = pixel_offset + y * bytes_per_line
        for x in range(width):
            pixel = struct.unpack_from(endian + "I", data, row + x * 4)[0]
            out[pos] = (pixel & red_mask) >> rshift
            out[pos + 1] = (pixel & green_mask) >> gshift
            out[pos + 2] = (pixel & blue_mask) >> bshift
            pos += 3
    Image.frombytes("RGB", (width, height), bytes(out)).save(dst)
    print(f"saved={dst}")
    print(f"size={width}x{height}")


if __name__ == "__main__":
    main()
