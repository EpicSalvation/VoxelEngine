#!/usr/bin/env python3
# Regenerates the M15 textured-blocks sample asset: six 16x16 PNG tiles and the
# textured_blocks.bbmodel that embeds them as base64 data URIs (exactly as
# Blockbench exports a self-contained .bbmodel). Run from anywhere:
#
#     python assets/blockbench/generate_sample.py
#
# The committed .bbmodel + PNGs are the canonical artifact the 15-textured-blocks
# demo imports; this script only documents how they were authored and lets them be
# regenerated. It has no third-party dependencies (hand-rolled PNG writer).

import base64
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TEX_DIR = os.path.join(HERE, "textures")

N = 16  # tile edge in pixels


def png_bytes(pixels):
    """Encode a 16x16 list-of-rows of (r,g,b,a) tuples into PNG bytes (RGBA8)."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter type 0 (None) per scanline
        for (r, g, b, a) in row:
            raw += bytes((r, g, b, a))

    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return out + struct.pack(">I", crc)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", N, N, 8, 6, 0, 0, 0)  # 8-bit, color type 6 (RGBA)
    idat = zlib.compress(bytes(raw), 9)
    return sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


def fill(fn):
    return [[fn(x, y) for x in range(N)] for y in range(N)]


def noise(x, y, base, amp):
    # Cheap deterministic per-pixel jitter so flat colors read as a material.
    h = (x * 73856093) ^ (y * 19349663)
    d = (h % (2 * amp + 1)) - amp
    return max(0, min(255, base + d))


# ── Six material tiles (top / bottom / side per block) ───────────────────────
def grass_top(x, y):
    return (noise(x, y, 70, 18), noise(x, y, 150, 22), noise(x, y, 60, 16), 255)


def dirt(x, y):
    return (noise(x, y, 120, 16), noise(x, y, 85, 14), noise(x, y, 55, 12), 255)


def grass_side(x, y):
    # Dirt body with a green crown on the top few rows (y small == top).
    if y < 4:
        return grass_top(x, y)
    if y == 4:
        return (noise(x, y, 95, 20), noise(x, y, 130, 24), noise(x, y, 55, 16), 255)
    return dirt(x, y)


def brick_top(x, y):
    # Mossy cap: gray stone speckled with green.
    if (x * 5 + y * 3) % 7 == 0:
        return (noise(x, y, 80, 14), noise(x, y, 120, 18), noise(x, y, 70, 14), 255)
    return (noise(x, y, 140, 12), noise(x, y, 140, 12), noise(x, y, 140, 12), 255)


def stone(x, y):
    g = noise(x, y, 128, 14)
    return (g, g, g, 255)


def brick_side(x, y):
    # Running-bond brick: mortar lines every 8px high, offset every other course.
    course = y // 8
    bx = (x + (4 if course % 2 else 0)) % 16
    mortar = (y % 8 == 0) or (bx == 0)
    if mortar:
        return (200, 200, 195, 255)  # light mortar
    return (noise(x, y, 150, 14), noise(x, y, 70, 10), noise(x, y, 55, 10), 255)  # red brick


TILES = [
    ("grass_top", grass_top),   # 0
    ("dirt", dirt),             # 1
    ("grass_side", grass_side), # 2
    ("brick_top", brick_top),   # 3
    ("stone", stone),           # 4
    ("brick_side", brick_side), # 5
]


def main():
    os.makedirs(TEX_DIR, exist_ok=True)
    sources = []
    for name, fn in TILES:
        png = png_bytes(fill(fn))
        with open(os.path.join(TEX_DIR, name + ".png"), "wb") as f:
            f.write(png)
        b64 = base64.b64encode(png).decode("ascii")
        sources.append((name, "data:image/png;base64," + b64))

    def tex(i):
        name, src = sources[i]
        return (
            '    {"name": "%s", "id": "%d", "width": 16, "height": 16, '
            '"uv_width": 16, "uv_height": 16, "source": "%s"}' % (name, i, src)
        )

    textures_json = ",\n".join(tex(i) for i in range(len(sources)))

    # Two elements: a grass cube and a brick cube, side by side with a gap, both
    # inside the 16-unit authoring cube (the importer maps [0,16] -> the voxel grid).
    bbmodel = """{
  "meta": {"format_version": "4.5", "model_format": "free", "box_uv": false},
  "name": "textured_blocks",
  "resolution": {"width": 16, "height": 16},
  "elements": [
    {
      "name": "grass_block",
      "from": [0, 0, 0],
      "to":   [6, 6, 6],
      "faces": {
        "up":    {"uv": [0, 0, 16, 16], "texture": 0},
        "down":  {"uv": [0, 0, 16, 16], "texture": 1},
        "north": {"uv": [0, 0, 16, 16], "texture": 2},
        "south": {"uv": [0, 0, 16, 16], "texture": 2},
        "east":  {"uv": [0, 0, 16, 16], "texture": 2},
        "west":  {"uv": [0, 0, 16, 16], "texture": 2}
      }
    },
    {
      "name": "brick_block",
      "from": [10, 0, 0],
      "to":   [16, 6, 6],
      "faces": {
        "up":    {"uv": [0, 0, 16, 16], "texture": 3},
        "down":  {"uv": [0, 0, 16, 16], "texture": 4},
        "north": {"uv": [0, 0, 16, 16], "texture": 5},
        "south": {"uv": [0, 0, 16, 16], "texture": 5},
        "east":  {"uv": [0, 0, 16, 16], "texture": 5},
        "west":  {"uv": [0, 0, 16, 16], "texture": 5}
      }
    }
  ],
  "textures": [
%s
  ]
}
""" % textures_json

    with open(os.path.join(HERE, "textured_blocks.bbmodel"), "w", newline="\n") as f:
        f.write(bbmodel)

    print("Wrote %d textures to %s" % (len(sources), TEX_DIR))
    print("Wrote textured_blocks.bbmodel (%d bytes)" % len(bbmodel))


if __name__ == "__main__":
    main()
