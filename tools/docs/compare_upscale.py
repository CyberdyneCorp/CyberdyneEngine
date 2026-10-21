#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measure temporal upscaling's captures and compose the published comparison. `just capture-upscale`.

The capture leaves, per scene, output height and mode, `<scene>-<height>-<mode>.png` and the program's
log beside it in one work directory. This reads the whole frame's device time out of each log, the
error of each converged still against the native-antialiased one, prints the table the READMEs
carry, and writes a labelled crop strip per scene and height: the same region in every mode,
magnified with nearest-neighbour so a reader sees pixels rather than a resampling of them.

THE PIXELS IN A CROP ARE THE DEVICE'S. The strip is cut and magnified by an integer factor; nothing
is filtered, so every texel in it is one the frame wrote.
"""

from __future__ import annotations

import argparse
import math
import pathlib
import re
import sys

from PIL import Image, ImageChops, ImageDraw

MODES = ("off", "native", "quality", "balanced", "performance")
LABELS = {
    "off": "no upscaler, no AA",
    "native": "native AA",
    "quality": "Quality 67%",
    "balanced": "Balanced 58%",
    "performance": "Performance 50%",
}
# The beauty program prints "frame timing  60 frames: min a, median b, mean c ms"; the world prints
# "device time: median b ms, ...".
TIMING = (
    re.compile(r"frame timing\s+\d+ frames: min [0-9.]+, median ([0-9.]+)"),
    re.compile(r"device time: median ([0-9.]+) ms"),
)
VALIDATION = re.compile(r"validation(?: errors:|\s+)\s*(\d+)")


def median_ms(log: str) -> float | None:
    """The whole frame's median device time a log reports, or None when it reports none."""
    for pattern in TIMING:
        found = pattern.search(log)
        if found:
            return float(found.group(1))
    return None


def validation_errors(log: str) -> int:
    """The largest validation error count a log reports; zero when it reports none."""
    return max((int(found) for found in VALIDATION.findall(log)), default=0)


def error(image: Image.Image, reference: Image.Image) -> tuple[float, float]:
    """Mean absolute error per 8-bit channel, and PSNR in dB, of `image` against `reference`."""
    if image.size != reference.size:
        raise ValueError("the two pictures are not the same size")
    difference = ImageChops.difference(image.convert("RGB"), reference.convert("RGB"))
    histogram = difference.histogram()
    count = 0
    absolute = 0
    squared = 0
    for channel in range(3):
        for level, texels in enumerate(histogram[channel * 256:(channel + 1) * 256]):
            count += texels
            absolute += level * texels
            squared += level * level * texels
    mae = absolute / count
    mse = squared / count
    psnr = math.inf if mse == 0 else 10.0 * math.log10(255.0 * 255.0 / mse)
    return mae, psnr


def crop_strip(pictures: dict[str, Image.Image], box: tuple[int, int, int, int],
               zoom: int) -> Image.Image:
    """The same `box` from every mode side by side, magnified `zoom` times, each labelled."""
    left, top, width, height = box
    pad, bar = 4, 22
    present = [mode for mode in MODES if mode in pictures]
    strip = Image.new("RGB", (len(present) * (width * zoom + pad) - pad, height * zoom + bar),
                      (24, 24, 24))
    draw = ImageDraw.Draw(strip)
    for index, mode in enumerate(present):
        tile = pictures[mode].convert("RGB").crop((left, top, left + width, top + height))
        tile = tile.resize((width * zoom, height * zoom), Image.NEAREST)
        x = index * (width * zoom + pad)
        strip.paste(tile, (x, bar))
        draw.text((x + 6, 5), LABELS[mode], fill=(235, 235, 235))
    return strip


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=pathlib.Path, required=True)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--heights", nargs="+", type=int, required=True)
    parser.add_argument("--crop", nargs=4, type=float, metavar=("X", "Y", "W", "H"), required=True,
                        help="the crop as fractions of the frame")
    parser.add_argument("--zoom", type=int, default=2)
    parser.add_argument("--out", type=pathlib.Path, required=True,
                        help="directory the crop strips are written to")
    arguments = parser.parse_args()

    failed = False
    print(f"| {arguments.scene} | " + " | ".join(LABELS[mode] for mode in MODES) + " |")
    print("|---" * (len(MODES) + 1) + "|")
    for height in arguments.heights:
        pictures: dict[str, Image.Image] = {}
        cells = []
        for mode in MODES:
            stem = arguments.work / f"{arguments.scene}-{height}-{mode}"
            png, log = stem.with_suffix(".png"), stem.with_suffix(".log")
            if not png.is_file() or not log.is_file():
                print(f"{stem}: the capture wrote no picture or no log", file=sys.stderr)
                return 1
            text = log.read_text(errors="replace")
            if validation_errors(text) != 0:
                print(f"{log}: validation errors", file=sys.stderr)
                failed = True
            pictures[mode] = Image.open(png)
            ms = median_ms(text)
            cells.append("-" if ms is None else f"{ms:.2f} ms")
        reference = pictures["native"]
        for index, mode in enumerate(MODES):
            if mode == "native":
                continue
            mae, psnr = error(pictures[mode], reference)
            cells[index] += f", {psnr:.1f} dB (MAE {mae:.2f})"
        width, frame_height = reference.size
        print(f"| {width}x{frame_height} | " + " | ".join(cells) + " |")
        x, y, w, h = arguments.crop
        box = (int(x * width), int(y * frame_height), int(w * width), int(h * frame_height))
        strip = crop_strip(pictures, box, arguments.zoom)
        target = arguments.out / f"upscale-{arguments.scene}-crops-{height}.png"
        strip.save(target, optimize=True)
        print(f"wrote {target}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
