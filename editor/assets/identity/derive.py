#!/usr/bin/env python3
"""Derive the editor's identity assets from the normative logo sheet.

`docs/design/images/cyberengine-logo.png` is the normative identity: one sheet carrying the hero
mark, the wordmark, and four lockups — horizontal, vertical, monochrome and app icon. It is art,
rendered on a near-black backdrop, and the editor needs the same marks as *transparent* images so
that a lockup sits on charcoal chrome rather than on a plate of its own. A plate would read as a
badge, and `editor-visual-language` is explicit that the chrome around the mark stays charcoal and
flat.

So this script does one operation, and it is worth stating precisely because it is the only place
the artwork is altered:

    the backdrop is keyed out by luminance headroom above the sheet's own corner colour,
    with a knee that removes the flat vignette and keeps the mark's emissive bloom.

Nothing is recoloured. The metallic gradient and the blue emissive core survive untouched — they
belong to the mark, and this script's whole purpose is to let them stay there while the chrome
around them does not pick them up.

Run from the repository root:  python3 editor/assets/identity/derive.py
"""

from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[3]
SHEET = ROOT / "docs/design/images/cyberengine-logo.png"
OUT = ROOT / "editor/assets/identity"

# Boxes into the sheet, in its own pixels. The sheet is 1254 x 1254.
HERO_MARK = (375, 85, 885, 655)
HORIZONTAL = (25, 992, 447, 1112)
MONOCHROME = (784, 986, 1016, 1140)

# Below this much luminance above the sheet's corner colour, a pixel is backdrop. High enough to
# remove the vignette, low enough to keep the bloom around the mark, which is part of the mark.
KNEE = 26.0


def keyed(box: tuple[int, int, int, int]) -> Image.Image:
    """Crop the sheet and return it with its backdrop keyed to transparency."""
    art = np.asarray(Image.open(SHEET).crop(box).convert("RGB")).astype(np.float32)
    edges = np.concatenate([art[0:2].reshape(-1, 3), art[-2:].reshape(-1, 3)])
    lifted = np.clip(art - np.median(edges, axis=0), 0.0, 255.0)
    headroom = lifted.max(axis=2)
    alpha = np.clip((headroom - KNEE) / (255.0 - KNEE), 0.0, 1.0) ** 0.85 * 255.0
    colour = np.where(
        headroom[..., None] > 1.0,
        np.clip(lifted * 255.0 / np.maximum(headroom[..., None], 1.0), 0.0, 255.0),
        0.0,
    )
    return Image.fromarray(np.dstack([colour, alpha]).astype(np.uint8), "RGBA")


def as_mask(image: Image.Image) -> Image.Image:
    """Force an image to pure white, keeping its alpha.

    A monochrome lockup is one shape in one colour, so shipping it with a colour baked in would be
    two assets that have to be kept in step — and the one baked in here would be white, which is
    invisible on the light theme. Shipped as a mask, the interface tints it with the theme's primary
    text colour and the same file serves both themes.
    """
    art = np.asarray(image).astype(np.uint8)
    art[..., 0:3] = 255
    return Image.fromarray(art, "RGBA")


def square(image: Image.Image, size: int) -> Image.Image:
    """Centre an image on a transparent square, so an icon is not distorted by its aspect."""
    side = max(image.size)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.alpha_composite(image, ((side - image.width) // 2, (side - image.height) // 2))
    return canvas.resize((size, size), Image.LANCZOS)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    keyed(HORIZONTAL).save(OUT / "cyberengine-horizontal.png")
    as_mask(keyed(MONOCHROME)).save(OUT / "cyberengine-monochrome.png")
    mark = keyed(HERO_MARK)
    square(mark, 256).save(OUT / "cyberengine-mark-256.png")
    square(mark, 64).save(OUT / "cyberengine-mark-64.png")
    for name in sorted(OUT.glob("*.png")):
        print(f"{name.relative_to(ROOT)}  {Image.open(name).size}")


if __name__ == "__main__":
    main()
