#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Lay frames of the live GI out in a labelled grid: the guide's editor and RTS pictures.

Each argument is `PATH=LABEL`, in reading order; `--columns` says how many a row holds. The frames
are the suites' own stills, written by:

* the editor's viewport — `CY_GI_VIEWPORT_SHOT=<dir> build/dev/cy_test_smoke_editor_authored_frame_vulkan
  -tc="*device GI*"` writes `editor-gi-live-*.png` (`docs/design/images/editor-gi-live.png`);
* the RTS — `cy_sample_rts-api --render <dir>` and `--render <dir> --gi off` write `rts-gi-*.png`
  and `rts-nogi-*.png` (`docs/design/images/rts-api-gi.png`).

    python3 tools/docs/compose_gi_live.py --columns 3 --out docs/design/images/editor-gi-live.png \\
        shots/editor-gi-live-plain.png="Lit, no GI" shots/editor-gi-live-Lit.png="Lit, device GI" ...
"""

from __future__ import annotations

import argparse
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont


def font(size: int) -> ImageFont.ImageFont:
    for name in ("DejaVuSans.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def labelled(path: pathlib.Path, text: str, band: int) -> Image.Image:
    with Image.open(path) as opened:
        frame = opened.convert("RGB")
    out = Image.new("RGB", (frame.width, frame.height + band), (24, 24, 28))
    out.paste(frame, (0, band))
    draw = ImageDraw.Draw(out)
    face = font(max(12, band - 10))
    draw.text((6, (band - face.size) // 2 if hasattr(face, "size") else 4), text, font=face,
              fill=(235, 235, 235))
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--columns", type=int, default=3)
    parser.add_argument("--band", type=int, default=24, help="the label band's height in pixels")
    parser.add_argument("frames", nargs="+", help="PATH=LABEL")
    args = parser.parse_args()
    tiles = []
    for entry in args.frames:
        path, separator, text = entry.partition("=")
        if not separator:
            print(f"compose_gi_live: '{entry}' is not PATH=LABEL", file=sys.stderr)
            return 2
        tiles.append(labelled(pathlib.Path(path), text, args.band))
    width = max(tile.width for tile in tiles)
    height = max(tile.height for tile in tiles)
    rows = (len(tiles) + args.columns - 1) // args.columns
    grid = Image.new("RGB", (width * args.columns, height * rows), (0, 0, 0))
    for index, tile in enumerate(tiles):
        grid.paste(tile, ((index % args.columns) * width, (index // args.columns) * height))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    grid.save(args.out, optimize=True)
    print(f"wrote {args.out} ({grid.width}x{grid.height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
