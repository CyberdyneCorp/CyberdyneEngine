#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare the beauty shot with soft and contact shadows off and on. `just capture-soft-shadows` runs it.

THREE THINGS, AND THE FIRST ONE CAN FAIL THE RECIPE:

1. **Off is the frame before the change.** The off picture's pixels must be the pixels of
   `--reference` (M11.c's published `m11c-beauty-shot.png`) exactly: the setting off draws with the
   entry point the frame always drew with.
2. **What on changed**, as numbers, in both directions. Unlike ambient occlusion, a soft filter
   moves light BOTH ways — a penumbra is lighter inside the old hard edge and darker outside it — so
   brighter pixels are counted and reported rather than refused.
3. **Where the two differ most, off beside on**, with the amplified difference as a third panel —
   compare_ambient_occlusion.py's window, written to `--detail`.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

from PIL import Image, ImageChops

from compare_ambient_occlusion import AMPLIFY, WINDOW, detail_origin, load, luma


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
              "the frame byte-identical to the frame before soft and contact shadows existed.",
              file=sys.stderr)
        return 1
    print(f"off           identical to {arguments.reference.name}, pixel for pixel")

    width, height = off.size
    difference = [0.0] * (width * height)
    changed = darker = brighter = 0
    largest = 0.0
    for index, (before, after) in enumerate(zip(off.getdata(), on.getdata())):
        delta = luma(before) - luma(after)
        changed += 1 if before != after else 0
        darker += 1 if delta > 1.0 else 0
        brighter += 1 if delta < -1.0 else 0
        difference[index] = abs(delta)
        largest = max(largest, abs(delta))
    pixels = width * height
    print(f"on            {changed} of {pixels} pixels changed ({100.0 * changed / pixels:.1f} %); "
          f"{darker} more than one step darker, {brighter} more than one step brighter; largest "
          f"{largest:.1f} of 255")

    x, y = detail_origin(difference, width, height)
    box = (x, y, min(width, x + WINDOW[0]), min(height, y + WINDOW[1]))
    crop_off, crop_on = off.crop(box), on.crop(box)
    scale = 2
    tile = (crop_off.width * scale, crop_off.height * scale)
    difference_panel = ImageChops.difference(crop_off.convert("RGB"), crop_on.convert("RGB"))
    difference_panel = difference_panel.point(lambda value: min(255, value * AMPLIFY)).convert("RGBA")
    detail = Image.new("RGBA", (tile[0] * 3 + 16, tile[1]), (0, 0, 0, 255))
    detail.paste(crop_off.resize(tile, Image.NEAREST), (0, 0))
    detail.paste(crop_on.resize(tile, Image.NEAREST), (tile[0] + 8, 0))
    detail.paste(difference_panel.resize(tile, Image.NEAREST), (2 * tile[0] + 16, 0))
    detail.save(arguments.detail)
    print(f"detail        {arguments.detail} — window {box}: off, on, and |off - on| x {AMPLIFY}; "
          f"{scale}x nearest")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
