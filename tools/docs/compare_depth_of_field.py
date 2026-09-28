#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare the beauty shot without depth of field and focused on its two targets.
`just capture-beauty-depth-of-field` runs it.

THREE THINGS, AND THE FIRST TWO CAN FAIL THE RECIPE:

1. **Without the stage, the frame is the frame before it existed.** The `--off` picture's pixels
   must be the pixels of `--reference` (M11.c's published `m11c-beauty-shot.png`) exactly.
2. **Each focus is sharp where it is focused and soft where it is not**, measured as the mean
   squared Laplacian of luminance — the energy of the finest detail — in a box around each target:
   the sphere 8 m out at the bottom of the frame, and the colonnade's last pillar on the right, 33 m
   out. Focused on the sphere, the sphere keeps most of the detail the frame without the stage has
   and the column loses most of its own; focused on the column, the other way round. A stage that
   blurred everything, or nothing, or the wrong field, fails here.
3. **The pictures for reading**: the two focused frames side by side at half size (`--pair`), and a
   crop of each target in the three frames at full size (`--detail`). The published stills are the
   evidence.

Pixels are compared as decoded RGBA, so a PNG recompressed by tools/docs/collect_beauty_shot.py
compares equal to the one the device wrote.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

from PIL import Image, ImageFilter

# Each target's box, as fractions of the frame: (left, top, right, bottom). The sphere's centre
# projects to about (0.43, 0.88) with a radius of a fifth of the height; the column to about
# (0.57, 0.51), 60 pixels wide and 330 tall at 1920 x 1080.
BOXES = {
    "sphere": (0.365, 0.74, 0.485, 0.98),
    "column": (0.550, 0.36, 0.586, 0.66),
}
# Focused, a target keeps at least this share of the detail it has with no stage; defocused, it
# keeps at most the second. The first is below one because the in-focus band is a pixel wide and a
# surface is not a plane; the second is what a blur of several pixels leaves of a stone texture.
KEPT_WHEN_FOCUSED = 0.75
KEPT_WHEN_DEFOCUSED = 0.6


def load(path: pathlib.Path) -> Image.Image:
    with Image.open(path) as opened:
        return opened.convert("RGBA")


def box(image: Image.Image, name: str) -> tuple[int, int, int, int]:
    left, top, right, bottom = BOXES[name]
    width, height = image.size
    return (int(left * width), int(top * height), int(right * width), int(bottom * height))


def detail(image: Image.Image, name: str) -> float:
    """The mean squared Laplacian of luminance inside the target's box.

    Filtered over the whole frame and THEN cropped: PIL's kernel filter copies an image's edge
    pixels through unfiltered, so filtering the crop would count each border pixel's raw luminance
    as detail, and on a dark or bright target that border is most of the measure.
    """
    grey = image.convert("L")
    kernel = ImageFilter.Kernel((3, 3), (0, 1, 0, 1, -4, 1, 0, 1, 0), 1, 128)
    laplacian = grey.filter(kernel).crop(box(image, name))
    values = list(laplacian.getdata())
    return sum((value - 128) ** 2 for value in values) / max(len(values), 1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("off", "sphere", "column", "reference", "pair", "detail"):
        parser.add_argument(f"--{name}", type=pathlib.Path, required=True)
    arguments = parser.parse_args()

    off, sphere, column = load(arguments.off), load(arguments.sphere), load(arguments.column)
    reference = load(arguments.reference)
    if off.size != reference.size or off.tobytes() != reference.tobytes():
        print(f"{arguments.off}: NOT the pixels of {arguments.reference}. Without depth of field "
              "the frame must be byte-identical to the frame before the stage existed.",
              file=sys.stderr)
        return 1
    print(f"off           identical to {arguments.reference.name}, {off.size[0]}x{off.size[1]}")

    failed = False
    for focused, image in (("sphere", sphere), ("column", column)):
        other = "column" if focused == "sphere" else "sphere"
        kept = detail(image, focused) / max(detail(off, focused), 1e-9)
        lost = detail(image, other) / max(detail(off, other), 1e-9)
        print(f"focus {focused:7} keeps {kept:6.1%} of the {focused}'s detail and "
              f"{lost:6.1%} of the {other}'s")
        if kept < KEPT_WHEN_FOCUSED or lost > KEPT_WHEN_DEFOCUSED:
            print(f"focused on the {focused}, the frame is not sharp there and soft elsewhere",
                  file=sys.stderr)
            failed = True
    if failed:
        return 1

    half = (off.size[0] // 2, off.size[1] // 2)
    pair = Image.new("RGBA", (half[0] * 2, half[1]))
    for index, image in enumerate((sphere, column)):
        pair.paste(image.resize(half, Image.Resampling.BOX), (half[0] * index, 0))
    pair.convert("RGB").save(arguments.pair, optimize=True)
    print(f"pair          {arguments.pair}  (focused on the sphere, on the column)")

    crops = [[image.crop(box(image, name)) for image in (off, sphere, column)]
             for name in ("sphere", "column")]
    width = sum(crop.size[0] for crop in crops[0]) + sum(crop.size[0] for crop in crops[1])
    height = max(crop.size[1] for row in crops for crop in row)
    sheet = Image.new("RGBA", (width, height), (0, 0, 0, 255))
    x = 0
    for row in crops:
        for crop in row:
            sheet.paste(crop, (x, 0))
            x += crop.size[0]
    sheet.convert("RGB").save(arguments.detail, optimize=True)
    print(f"detail        {arguments.detail}  (sphere off, sphere-focused, column-focused; "
          "then the column the same)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
