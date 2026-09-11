#!/usr/bin/env python3
"""Place `just capture-virtual-geometry`'s frames into docs/design/images/.

The capture tool writes through the golden-image suite's PNG writer, which stores rather than
compresses — the right trade for a test comparing bytes, and the wrong one for a file that lives in
the tree, where it costs about 2.7 MiB per image against about 60 KiB. This only recompresses; the
pixels are the ones the device produced.
"""

from __future__ import annotations

import pathlib
import sys

from PIL import Image

#: Which captured frame becomes which committed image. The three cluster views are one view of one
#: scene at three geometric-error thresholds, which is the only way a still shows the hierarchy
#: choosing.
PAIRS = [
    ("t1-shaded.png", "virtual-geometry-shaded.png"),
    ("t1-triangles.png", "virtual-geometry-triangles.png"),
    ("t1-clusters.png", "virtual-geometry-clusters-1px.png"),
    ("t4-clusters.png", "virtual-geometry-clusters-4px.png"),
    ("t16-clusters.png", "virtual-geometry-clusters-16px.png"),
]

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
OUTPUT = REPO_ROOT / "docs" / "design" / "images"


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: collect_virtual_geometry.py <capture-directory>", file=sys.stderr)
        return 2
    work = pathlib.Path(sys.argv[1])
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for source, target in PAIRS:
        frame = work / source
        if not frame.is_file():
            print(f"collect: {frame} is missing; the capture did not write it", file=sys.stderr)
            return 1
        destination = OUTPUT / target
        Image.open(frame).save(destination, optimize=True)
        print(f"    {target}  {destination.stat().st_size // 1024} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
