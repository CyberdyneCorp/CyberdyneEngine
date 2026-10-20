#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Lay one scene's GI comparison out side by side. `just capture-gi-interior` runs it.

Four pictures of ONE view, each from `cy_sample_beauty`:

1. **GI off**: the frame's flat sky term, which lights every surface as if nothing stood over it.
2. **CyberGI**: the engine's default — the screen probes and their probe-space denoiser.
3. **Path tracer**: the same frame shaded through the offline path tracer's indirect diffuse
   (`gi::PathTracer`, the lightmap bake's) in place of the GI's; everything else — the sun, its
   shadow, the GI's specular, the sky, the post chain — is the frame's own.
4. **Error**: where the GI's indirect diffuse and the path tracer's differ, relative to the path
   tracer's, from black through red to pale yellow at 50 % and above. Part of it is the reference's
   own noise, which the run measures from two independent halves of each pixel's paths and prints
   beside the error.

The labels carry the figures the run printed, read from its log so a caption cannot quote a number
the run did not measure: under the frame without the GI, its flat sky term's relative error over
the traced pixels; under CyberGI, its GPU milliseconds (its `gi fastest` line, the fastest of the
frames the run times after the still) and its relative error; under the path tracer, its paths,
bounces and processor time, and its own noise. The script fails when the log does not hold them.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

from PIL import Image, ImageDraw, ImageFont

ERROR = re.compile(r"gi picture\s+(\d+) pixels path traced every (\d+) px \((\d+) paths, (\d+) "
                   r"bounces\) in ([0-9.]+) s.*?([0-9.]+) relative error, the reference's own noise "
                   r"([0-9.]+)")
FLAT = re.compile(r"gi flat\s+.*?its flat sky term ([0-9.]+) relative error")
GPU = re.compile(r"gi fastest\s+([0-9.]+) ms")
TIER = re.compile(r"traced through the (.+)$", re.MULTILINE)


def load(path: pathlib.Path) -> Image.Image:
    with Image.open(path) as opened:
        return opened.convert("RGB")


def font(size: int) -> ImageFont.ImageFont:
    for name in ("DejaVuSans-Bold.ttf", "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def label(image: Image.Image, text: str) -> Image.Image:
    labelled = image.copy()
    draw = ImageDraw.Draw(labelled)
    face = font(max(14, image.height // 26))
    left, top, right, bottom = draw.textbbox((0, 0), text, font=face)
    pad = max(6, image.height // 90)
    draw.rectangle((0, 0, right - left + (2 * pad), bottom - top + (2 * pad)), fill=(0, 0, 0))
    draw.text((pad - left, pad - top), text, font=face, fill=(255, 255, 255))
    return labelled


def figures(log: str) -> dict[str, str]:
    error = ERROR.search(log)
    gpu = GPU.search(log)
    flat = FLAT.search(log)
    if error is None or gpu is None or flat is None:
        sys.exit("compose_gi_comparison: the log holds no `gi picture`, `gi flat` or `gi fastest` "
                 "line")
    tier = TIER.search(log)
    return {
        "pixels": error.group(1),
        "step": error.group(2),
        "paths": error.group(3),
        "bounces": error.group(4),
        "seconds": error.group(5),
        "error": error.group(6),
        "noise": error.group(7),
        "flat": flat.group(1),
        "ms": gpu.group(1),
        "tier": tier.group(1) if tier else "the world tier",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("off", "on", "reference", "error", "log", "out"):
        parser.add_argument(f"--{name}", required=True, type=pathlib.Path)
    args = parser.parse_args()
    numbers = figures(args.log.read_text(encoding="utf-8"))
    off, on, reference, error = (load(path) for path in (args.off, args.on, args.reference,
                                                         args.error))
    size = (off.width // 2, off.height // 2)
    panels = [
        label(off.resize(size, Image.LANCZOS),
              f"GI off, flat sky term: 0 ms, error {float(numbers['flat']):.3f}"),
        label(on.resize(size, Image.LANCZOS),
              f"CyberGI: {float(numbers['ms']):.2f} ms on the GPU, error "
              f"{float(numbers['error']):.3f}"),
        label(reference.resize(size, Image.LANCZOS),
              f"Path tracer: {numbers['paths']} paths, {numbers['bounces']} bounces, "
              f"{float(numbers['seconds']):.0f} s of CPU, noise {float(numbers['noise']):.3f}"),
        label(error.resize(size, Image.LANCZOS),
              f"Error map: {float(numbers['error']):.3f} relative, the reference's noise "
              f"{float(numbers['noise']):.3f} (black 0, yellow 50 %+)"),
    ]
    sheet = Image.new("RGB", (size[0] * 2, size[1] * 2))
    for index, panel in enumerate(panels):
        sheet.paste(panel, ((index % 2) * size[0], (index // 2) * size[1]))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(args.out, optimize=True)
    print(f"{args.out}: error {numbers['error']} (the reference's own noise {numbers['noise']}, "
          f"the flat sky term's {numbers['flat']}) over {numbers['pixels']} pixels traced every "
          f"{numbers['step']} px, GI {numbers['ms']} ms through the {numbers['tier']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
