#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Does the beauty shot read its cooked mip chain? M11.c task 3.8, measured on THE SHOT.

`just measure-beauty-mip-chain` photographs `samples/12-beauty` three times from the same committed
graphs, the same textures and the same camera: twice with every cooked mip chain uploaded, once with
level 0 of each ALBEDO map and nothing beneath it (`cy_sample_beauty --albedo-levels 1`). This
compares the three pictures and says what it found.

THE QUESTION IS THE ONE THE TASK BEGAN FROM, asked of the artefact rather than of a test scene. The
spike's measurement was that a frame with eight of nine levels never uploaded was BYTE-IDENTICAL,
because `cy_material_sample` — which the generated prelude writes and every albedo and data map in
the shot goes through — took `cyMaterialSampleTextureLevel(..., 0.0)`.

WHY THE ALBEDO MAP ALONE. It is the one texture only the MATERIAL samples. `beauty.slang` samples the
normal map and the data map's occlusion channel itself, with the implicit form, so cutting every
chain moves the picture whatever the material does — measured before the fix at 960x540: 15.17% of
texels. Cutting the albedo chains alone is the control whose answer depends on `cy_material_sample`
and on nothing else: before the fix it was byte-identical.

THREE REQUIREMENTS, each with the reason it is not weaker:

  1. The two full-chain pictures are IDENTICAL. The still is deterministic — its air is the settled
     field and nothing steps between runs — so any difference in (2) is the chain's and not noise.
  2. The full chain and level 0 alone DIFFER, by at least `--min-percent` of the texels.
  3. Level 0 alone is the MORE ALIASED of the two: its neighbour-to-neighbour energy exceeds the
     chain's. A difference in the other direction would be a picture that changed for some reason
     other than filtering.

Exits 0 when all three hold, 1 naming the first that did not.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

from PIL import Image, ImageChops, ImageStat


def load(path: pathlib.Path) -> Image.Image:
    return Image.open(path).convert("RGB")


def difference(a: Image.Image, b: Image.Image) -> tuple[int, int, float]:
    """(texels that differ, texels in all, mean |delta| over every channel of every texel)."""
    delta = ImageChops.difference(a, b)
    # A texel differs if ANY channel moved, which is what "byte-identical" means. A greyscale
    # conversion would weight the channels and round a one-step change in blue alone to zero.
    any_channel = ImageChops.lighter(
        ImageChops.lighter(delta.getchannel(0), delta.getchannel(1)), delta.getchannel(2)
    )
    differing = sum(any_channel.point(lambda value: 1 if value else 0).histogram()[1:])
    mean = sum(ImageStat.Stat(delta).mean) / 3.0
    return differing, a.width * a.height, mean


def neighbour_energy(image: Image.Image) -> float:
    """Mean |delta| between horizontally and vertically adjacent texels, in 8-bit steps.

    Aliasing is high-frequency energy the filter should have removed, so the picture sampled at
    level 0 alone carries more of it wherever a surface is minified.
    """
    width, height = image.size
    right = ImageChops.difference(
        image.crop((1, 0, width, height)), image.crop((0, 0, width - 1, height))
    )
    down = ImageChops.difference(
        image.crop((0, 1, width, height)), image.crop((0, 0, width, height - 1))
    )
    horizontal = sum(ImageStat.Stat(right).mean) / 3.0
    vertical = sum(ImageStat.Stat(down).mean) / 3.0
    return (horizontal + vertical) / 2.0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("chain", type=pathlib.Path, help="the shot with every mip chain uploaded")
    parser.add_argument("repeat", type=pathlib.Path, help="the same shot rendered a second time")
    parser.add_argument(
        "level_zero", type=pathlib.Path, help="the shot with level 0 alone of every albedo map"
    )
    parser.add_argument(
        "--min-percent",
        type=float,
        default=1.0,
        help="the fewest texels, in percent, the chain must move",
    )
    options = parser.parse_args()

    chain = load(options.chain)
    repeat = load(options.repeat)
    level_zero = load(options.level_zero)
    if chain.size != repeat.size or chain.size != level_zero.size:
        print(
            f"the three pictures are not one size: {chain.size}, {repeat.size}, "
            f"{level_zero.size}"
        )
        return 1

    same, total, same_mean = difference(chain, repeat)
    moved, _, moved_mean = difference(chain, level_zero)
    chain_energy = neighbour_energy(chain)
    zero_energy = neighbour_energy(level_zero)
    percent = 100.0 * moved / total

    print(f"beauty mip chain: {chain.width}x{chain.height}, {total} texels")
    print(
        f"beauty mip chain: two renders of the full chain differ in {same} texels at mean "
        f"|delta| {same_mean:.3f}/255"
    )
    print(
        f"beauty mip chain: full chain against albedo level 0 alone {moved} texels ({percent:.2f}%) "
        f"at mean |delta| {moved_mean:.3f}/255"
    )
    print(
        f"beauty mip chain: neighbour-to-neighbour |delta| {chain_energy:.3f}/255 through the "
        f"chain against {zero_energy:.3f}/255 at level 0 alone"
    )

    if same != 0:
        print(
            "REFUSED: the full chain rendered twice is not byte-identical, so a difference "
            "against level 0 could be noise rather than the chain"
        )
        return 1
    if percent < options.min_percent:
        print(
            f"REFUSED: the shot moved {percent:.2f}% of texels when each albedo map was level 0 alone, "
            f"under the {options.min_percent:.2f}% floor — the material programs are not reading the "
            "cooked chain"
        )
        return 1
    if zero_energy <= chain_energy:
        print(
            "REFUSED: level 0 alone is not the more aliased picture, so what moved was not "
            "filtering"
        )
        return 1
    print("beauty mip chain: the shot reads its cooked mip chain")
    return 0


if __name__ == "__main__":
    sys.exit(main())
