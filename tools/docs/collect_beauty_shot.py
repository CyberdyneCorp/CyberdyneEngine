#!/usr/bin/env python3
"""Recompress M11.c's beauty shot and report what it cost. `just capture-beauty-shot` runs it.

WHY THE STILLS ARE RECOMPRESSED, and it is the same argument — and the same two lines of PIL —
`tools/docs/collect_world.py`, `collect_animated_character.py` and `collect_virtual_geometry.py`
already make: the capture writes through the golden-image suite's PNG writer, which STORES rather
than compresses. That is the right trade for a test that compares bytes and the wrong one for a file
that lives in the tree, where a 1920x1080 frame costs about 6 MiB against about 900 KiB.

**THIS ONLY RECOMPRESSES.** The pixels are the ones the device produced, and the check below is not
a comment: it re-reads both files and refuses to write one whose pixels are not identical to what it
read. A "recompression" that resampled, quantised or colour-managed the image would be a published
picture the engine did not draw, which is exactly what `docs/design/beauty-shot.md`'s provenance is
written to make impossible.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

from PIL import Image

IMAGES = pathlib.Path(__file__).resolve().parents[2] / "docs" / "design" / "images"


def recompress(path: pathlib.Path) -> tuple[int, int]:
    """Rewrite one PNG with deflate. Returns (before, after) in bytes."""
    before = path.stat().st_size
    with Image.open(path) as opened:
        original = opened.convert("RGBA")
        pixels = original.tobytes()
        size = original.size
    original.save(path, optimize=True)

    with Image.open(path) as written:
        if written.convert("RGBA").tobytes() != pixels or written.size != size:
            raise SystemExit(f"{path}: recompression changed the pixels, which it must never do")
    return before, path.stat().st_size


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "stills",
        nargs="*",
        type=pathlib.Path,
        default=[
            IMAGES / "m11c-beauty-shot.png",
            IMAGES / "m11c-beauty-shot-linear.png",
        ],
    )
    arguments = parser.parse_args()
    for path in arguments.stills:
        if not path.is_file():
            print(f"{path}: not written by the capture", file=sys.stderr)
            return 1
        before, after = recompress(path)
        print(f"{path.name}  {before} -> {after} bytes, pixels unchanged")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
