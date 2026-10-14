#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare the beauty shot with its flat sky term and with the engine's global illumination.
`just capture-beauty-gi` runs it.

THREE THINGS, AND THE FIRST TWO CAN FAIL THE RECIPE:

1. **Without the GI, the frame is the frame before it existed.** The `--off` picture's pixels must
   be the pixels of `--reference` (M11.c's published `m11c-beauty-shot.png`) exactly: the GI's
   entry points, its bindings and its prepass are added beside the published ones and change none
   of them.
2. **With it, the surfaces change and the sky does not.** The GI replaces the flat sky term on
   every surface the scene pass draws, so a large share of the frame's pixels must move; the dome is
   drawn by the published sky pass and seen through no surface, so a box of open sky between the
   columns must be the same pixels in both. A GI that tinted the whole frame, or touched nothing,
   fails here.
3. **The pictures for reading**: the two frames side by side at half size (`--pair`), and crops at
   full size of a column's shadowed face, the copper sphere and a block's shadowed front, without
   and with, beside the difference amplified (`--detail`).

Pixels are compared as decoded RGBA, so a PNG recompressed by tools/docs/collect_beauty_shot.py
compares equal to the one the device wrote.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

from PIL import Image, ImageChops

# Open sky between the near column on the left and the monolith, above the far columns' tops: as
# fractions of the frame, (left, top, right, bottom).
SKY = (0.27, 0.02, 0.36, 0.20)
# The crops for reading: a column's shadowed face on the left, the copper sphere, and the shadowed
# front of the block on the right.
CROPS = {
    "column": (0.11, 0.40, 0.21, 0.80),
    "sphere": (0.32, 0.68, 0.53, 1.00),
    "block": (0.63, 0.72, 0.94, 1.00),
}
# At least this share of the frame's pixels must change with the GI on: the surfaces are most of the
# frame and every one of them has a sky term the GI replaces.
MIN_CHANGED = 0.30
# The difference picture's gain, so a change of a few 8-bit steps is visible.
DIFFERENCE_GAIN = 6


def load(path: pathlib.Path) -> Image.Image:
    with Image.open(path) as opened:
        return opened.convert("RGBA")


def fraction_box(image: Image.Image, box: tuple[float, float, float, float]) -> tuple[int, ...]:
    width, height = image.size
    left, top, right, bottom = box
    return (int(left * width), int(top * height), int(right * width), int(bottom * height))


def changed_share(a: Image.Image, b: Image.Image) -> float:
    difference = ImageChops.difference(a.convert("RGB"), b.convert("RGB")).convert("L")
    histogram = difference.histogram()
    width, height = a.size
    return 1.0 - (histogram[0] / float(width * height))


def write_pair(off: Image.Image, on: Image.Image, path: pathlib.Path) -> None:
    width, height = off.size
    half = (width // 2, height // 2)
    pair = Image.new("RGB", (half[0] * 2, half[1]))
    pair.paste(off.convert("RGB").resize(half, Image.LANCZOS), (0, 0))
    pair.paste(on.convert("RGB").resize(half, Image.LANCZOS), (half[0], 0))
    pair.save(path, optimize=True)


def write_detail(off: Image.Image, on: Image.Image, path: pathlib.Path) -> None:
    rows = []
    for box in CROPS.values():
        area = fraction_box(off, box)
        a = off.crop(area).convert("RGB")
        b = on.crop(area).convert("RGB")
        gain = ImageChops.difference(a, b).point(lambda value: min(255, value * DIFFERENCE_GAIN))
        rows.append((a, b, gain))
    width = max(sum(image.size[0] for image in row) for row in rows)
    height = sum(row[0].size[1] for row in rows)
    sheet = Image.new("RGB", (width, height))
    y = 0
    for row in rows:
        x = 0
        for image in row:
            sheet.paste(image, (x, y))
            x += image.size[0]
        y += row[0].size[1]
    sheet.save(path, optimize=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--off", type=pathlib.Path, required=True)
    parser.add_argument("--on", type=pathlib.Path, required=True)
    parser.add_argument("--reference", type=pathlib.Path, required=True)
    parser.add_argument("--pair", type=pathlib.Path, required=True)
    parser.add_argument("--detail", type=pathlib.Path, required=True)
    args = parser.parse_args()

    off = load(args.off)
    on = load(args.on)
    reference = load(args.reference)
    failures = []

    if off.size != reference.size or off.tobytes() != reference.tobytes():
        failures.append(f"{args.off} is not {args.reference} pixel for pixel")
    else:
        print(f"off           the published still, pixel for pixel ({off.size[0]}x{off.size[1]})")

    if on.size != off.size:
        failures.append("the frames with and without the GI are different sizes")
    else:
        share = changed_share(off, on)
        print(f"on            {share:.1%} of the pixels changed")
        if share < MIN_CHANGED:
            failures.append(f"only {share:.1%} of the pixels changed; the GI should replace the "
                            f"sky term on every surface (at least {MIN_CHANGED:.0%})")
        sky = fraction_box(off, SKY)
        if off.crop(sky).tobytes() != on.crop(sky).tobytes():
            failures.append("the open sky between the columns changed: the GI touched the dome")
        else:
            print("sky           the open sky is the same pixels with and without")
        write_pair(off, on, args.pair)
        write_detail(off, on, args.detail)
        print(f"wrote         {args.pair}\n              {args.detail}")

    for failure in failures:
        print(f"compare_gi: {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
