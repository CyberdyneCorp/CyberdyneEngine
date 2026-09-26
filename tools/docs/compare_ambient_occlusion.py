#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare the beauty shot with ambient occlusion off and on. `just capture-ambient-occlusion` runs it.

THREE THINGS, AND THE FIRST ONE CAN FAIL THE RECIPE:

1. **Off is the frame before the stage existed.** The off picture's pixels must be the pixels of
   `--reference` (M11.c's published `m11c-beauty-shot.png`) exactly. The setting off takes no code
   path the frame did not already take, and this is where that is measured on the artefact rather
   than argued in a comment.
2. **What on changed**, as numbers: how many pixels moved, and by how much in Rec. 709 luma, with the
   direction of every change counted — ambient occlusion only ever REMOVES sky light, so a pixel that
   got brighter is a defect and is reported as one.
3. **One contact, off beside on.** The 384x216 window where the two differ most, side by side and
   doubled with nearest-neighbour sampling so no pixel is invented, and a third panel that is NOT a
   rendered image: the per-channel difference off minus on, multiplied by eight, so the reader can
   see where the term acted. Written to `--detail`.

Pixels are compared as decoded RGBA, so a PNG recompressed by tools/docs/collect_beauty_shot.py
compares equal to the one the device wrote.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

from PIL import Image, ImageChops

WINDOW = (384, 216)
AMPLIFY = 8


def load(path: pathlib.Path) -> Image.Image:
    with Image.open(path) as opened:
        return opened.convert("RGBA")


def luma(pixel: tuple[int, int, int, int]) -> float:
    return 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2]


def detail_origin(difference: list[float], width: int, height: int) -> tuple[int, int]:
    """The top-left of the window with the largest summed difference, on a coarse grid."""
    step = 16
    columns = (width + step - 1) // step
    rows = (height + step - 1) // step
    cells = [0.0] * (columns * rows)
    for y in range(height):
        base = y * width
        row = (y // step) * columns
        for x in range(width):
            cells[row + x // step] += difference[base + x]
    span_x, span_y = WINDOW[0] // step, WINDOW[1] // step
    best, origin = -1.0, (0, 0)
    for cy in range(0, max(1, rows - span_y + 1)):
        for cx in range(0, max(1, columns - span_x + 1)):
            total = sum(cells[(cy + dy) * columns + cx + dx] for dy in range(span_y) for dx in range(span_x))
            if total > best:
                best, origin = total, (cx * step, cy * step)
    return origin


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--off", type=pathlib.Path, required=True)
    parser.add_argument("--on", type=pathlib.Path, required=True)
    parser.add_argument("--reference", type=pathlib.Path, required=True)
    parser.add_argument("--detail", type=pathlib.Path, required=True)
    arguments = parser.parse_args()

    off, on, reference = load(arguments.off), load(arguments.on), load(arguments.reference)
    if off.size != on.size:
        print("the two captures are different sizes", file=sys.stderr)
        return 1
    if off.size != reference.size or off.tobytes() != reference.tobytes():
        print(f"{arguments.off}: NOT the pixels of {arguments.reference}. The setting off must leave "
              "the frame byte-identical to the frame before the ambient occlusion stage existed.",
              file=sys.stderr)
        return 1
    print(f"off           identical to {arguments.reference.name}, pixel for pixel")

    width, height = off.size
    off_pixels, on_pixels = list(off.getdata()), list(on.getdata())
    difference = [0.0] * (width * height)
    changed = brighter = 0
    total = 0.0
    largest = 0.0
    for index, (before, after) in enumerate(zip(off_pixels, on_pixels)):
        delta = luma(before) - luma(after)
        if before != after:
            changed += 1
        if delta < -1.0:
            brighter += 1
        difference[index] = max(delta, 0.0)
        total += delta
        largest = max(largest, delta)
    pixels = width * height
    print(f"on            {changed} of {pixels} pixels changed ({100.0 * changed / pixels:.1f} %), "
          f"mean luma {total / pixels:.2f} darker, largest {largest:.1f} of 255")
    print(f"on            {brighter} pixel(s) more than one step BRIGHTER")

    x, y = detail_origin(difference, width, height)
    box = (x, y, min(width, x + WINDOW[0]), min(height, y + WINDOW[1]))
    crop_off, crop_on = off.crop(box), on.crop(box)
    scale = 2
    tile = (crop_off.width * scale, crop_off.height * scale)
    difference_panel = ImageChops.subtract(crop_off.convert("RGB"), crop_on.convert("RGB"))
    difference_panel = difference_panel.point(lambda value: min(255, value * AMPLIFY)).convert("RGBA")
    detail = Image.new("RGBA", (tile[0] * 3 + 16, tile[1]), (0, 0, 0, 255))
    detail.paste(crop_off.resize(tile, Image.NEAREST), (0, 0))
    detail.paste(crop_on.resize(tile, Image.NEAREST), (tile[0] + 8, 0))
    detail.paste(difference_panel.resize(tile, Image.NEAREST), (2 * tile[0] + 16, 0))
    detail.save(arguments.detail)
    print(f"detail        {arguments.detail} — window {box}: off, on, and (off - on) x {AMPLIFY}; "
          f"{scale}x nearest")
    return 1 if brighter > 0 else 0


if __name__ == "__main__":
    raise SystemExit(main())
