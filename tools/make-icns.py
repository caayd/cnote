#!/usr/bin/env python3
"""Convert cnote's Windows .ico artwork into a macOS .icns bundle icon.

The repository keeps a single icon source (assets/cnote.ico) for the Windows
.rc resource and the Qt resource.  macOS bundles need an .icns, so this script
re-encodes the ICO's 32-bit DIB frames as PNG and packs them into an ICNS
container with the standard PNG-based chunk types:

    icp4  16x16, icp5 32x32, icp6 64x64, ic07 128x128,
    ic08 256x256, ic09 512x512, ic10 1024x1024

Only the standard library is used (struct + zlib); the output is deterministic.

Usage: make-icns.py <input.ico> <output.icns>

NOTE: the macOS package is not verified in this repository's build
environment (no macOS machine).  This script exists so the bundle carries the
project artwork instead of no icon; it is exercised on any host.
"""

import struct
import sys
import zlib

# icns chunk type by icon pixel size.
ICNS_TYPE_FOR_SIZE = {
    16: b"icp4",
    32: b"icp5",
    64: b"icp6",
    128: b"ic07",
    256: b"ic08",
    512: b"ic09",
    1024: b"ic10",
}


def read_ico_frames(path):
    """Return {size: rgba_bytes} for every 32-bit frame in the ICO."""
    data = open(path, "rb").read()
    reserved, image_type, count = struct.unpack_from("<HHH", data, 0)
    if reserved != 0 or image_type != 1:
        raise ValueError("not an ICO file: %s" % path)

    frames = {}
    for index in range(count):
        width, height, _colors, _reserved, _planes, bit_count, size, offset = (
            struct.unpack_from("<BBBBHHII", data, 6 + 16 * index)
        )
        width = width or 256
        height = height or 256
        if bit_count != 32:
            continue
        header_size = struct.unpack_from("<I", data, offset)[0]
        if header_size < 40:
            continue
        dib_height = struct.unpack_from("<i", data, offset + 8)[0]
        # ICO DIB heights count the XOR image plus the AND mask.
        pixel_height = abs(dib_height) // 2
        if pixel_height != height:
            continue
        rgba = decode_bgra_frame(data, offset, width, height)
        frames.setdefault(width, rgba)
    return frames


def decode_bgra_frame(data, offset, width, height):
    """Decode a bottom-up 32-bit BGRA DIB into top-down RGBA bytes."""
    header_size = struct.unpack_from("<I", data, offset)[0]
    top_down = struct.unpack_from("<i", data, offset + 8)[0] < 0
    pixels = offset + header_size
    stride = width * 4
    mask_stride = ((width + 31) // 32) * 4
    mask = pixels + stride * height

    out = bytearray(width * height * 4)
    alpha_all_zero = True
    for row in range(height):
        source_row = row if top_down else (height - 1 - row)
        base = pixels + source_row * stride
        destination = row * width * 4
        for column in range(width):
            b, g, r, a = data[base + column * 4 : base + column * 4 + 4]
            if a:
                alpha_all_zero = False
            out[destination + column * 4 : destination + column * 4 + 4] = bytes(
                (r, g, b, a)
            )

    if alpha_all_zero:
        # Some ICO writers leave the alpha channel empty and rely on the AND
        # mask; derive alpha from the mask (set bit = transparent).
        for row in range(height):
            source_row = row if top_down else (height - 1 - row)
            mask_row = mask + source_row * mask_stride
            for column in range(width):
                bit = (data[mask_row + column // 8] >> (7 - (column % 8))) & 1
                out[(row * width + column) * 4 + 3] = 0 if bit else 255
    return bytes(out)


def encode_png(width, height, rgba):
    def chunk(tag, payload):
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    raw = bytearray()
    for row in range(height):
        raw.append(0)  # filter type 0 (None)
        raw += rgba[row * width * 4 : (row + 1) * width * 4]
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def build_icns(frames):
    chunks = bytearray()
    for size in sorted(frames):
        icns_type = ICNS_TYPE_FOR_SIZE.get(size)
        if icns_type is None:
            continue
        payload = encode_png(size, size, frames[size])
        chunks += icns_type + struct.pack(">I", len(payload) + 8) + payload
    if not chunks:
        raise ValueError("no usable 32-bit ICO frames for an ICNS container")
    return b"icns" + struct.pack(">I", len(chunks) + 8) + bytes(chunks)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: make-icns.py <input.ico> <output.icns>\n")
        return 2
    frames = read_ico_frames(argv[1])
    icns = build_icns(frames)
    with open(argv[2], "wb") as stream:
        stream.write(icns)
    print(
        "wrote %s (%d bytes) from %s frames %s"
        % (argv[2], len(icns), argv[1], sorted(frames))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
