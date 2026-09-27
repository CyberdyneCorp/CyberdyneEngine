#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare the beauty shot ungraded and under its two looks. `just capture-beauty-grading` runs it.

THREE THINGS, AND THE FIRST TWO CAN FAIL THE RECIPE:

1. **Ungraded is the frame before grading existed.** The `--none` picture's pixels must be the
   pixels of `--reference` (M11.c's published `m11c-beauty-shot.png`) exactly: without a look the
   frame takes no code path it did not already take.
2. **Each look does what its name says**, measured over the whole frame: the warm look raises the
   red-to-blue balance and the cool look lowers it. A look that baked to the identity, or a lookup
   that read the wrong axis, would fail here.
3. **The three side by side**, each scaled to a third of the width with a box filter, written to
   `--triptych`. The published full-size stills are the evidence; the triptych is for reading.

Pixels are compared as decoded RGBA, so a PNG recompressed by tools/docs/collect_beauty_shot.py
compares equal to the one the device wrote.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

from PIL import Image

# A look has to move the frame's red/blue balance by more than this to count as having acted.
MINIMUM_SHIFT = 0.02


def load(path: pathlib.Path) -> Image.Image:
    with Image.open(path) as opened:
        return opened.convert("RGBA")


def balance(image: Image.Image) -> float:
    red, _, blue, _ = image.split()
    red_total = sum(value * count for value, count in enumerate(red.histogram()))
    blue_total = sum(value * count for value, count in enumerate(blue.histogram()))
    return red_total / max(blue_total, 1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--none", type=pathlib.Path, required=True)
    parser.add_argument("--warm", type=pathlib.Path, required=True)
    parser.add_argument("--cool", type=pathlib.Path, required=True)
    parser.add_argument("--reference", type=pathlib.Path, required=True)
    parser.add_argument("--triptych", type=pathlib.Path, required=True)
    arguments = parser.parse_args()

    none, warm, cool = load(arguments.none), load(arguments.warm), load(arguments.cool)
    reference = load(arguments.reference)
    if none.size != reference.size or none.tobytes() != reference.tobytes():
        print(f"{arguments.none}: NOT the pixels of {arguments.reference}. Without a look the frame "
              "must be byte-identical to the frame before grading existed.", file=sys.stderr)
        return 1
    print(f"ungraded      identical to {arguments.reference.name}, {none.size[0]}x{none.size[1]}")

    neutral = balance(none)
    shifts = {"warm": balance(warm) / neutral - 1.0, "cool": balance(cool) / neutral - 1.0}
    print(f"red/blue      ungraded {neutral:.4f}, warm {shifts['warm']:+.2%}, "
          f"cool {shifts['cool']:+.2%}")
    if shifts["warm"] < MINIMUM_SHIFT or shifts["cool"] > -MINIMUM_SHIFT:
        print("a look did not move the frame's balance the way its name says", file=sys.stderr)
        return 1

    third = (none.size[0] // 3, none.size[1] // 3)
    triptych = Image.new("RGBA", (third[0] * 3, third[1]))
    for index, image in enumerate((none, warm, cool)):
        triptych.paste(image.resize(third, Image.Resampling.BOX), (third[0] * index, 0))
    triptych.convert("RGB").save(arguments.triptych, optimize=True)
    print(f"triptych      {arguments.triptych}  (ungraded, warm, cool)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
