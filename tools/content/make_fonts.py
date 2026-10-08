#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Produce the fonts the engine carries in deps/fonts/ from their pinned upstream files.

--- WHAT THIS IS FOR -----------------------------------------------------------------------------

`text-and-fonts` needs real OpenType faces to be tested against: a variable Latin face with a weight
axis, and faces for the scripts whose shaping HarfBuzz exists to get right — Arabic (joining and
marks), Devanagari (reordering and conjuncts), Thai (marks and the dictionary-broken script) and
Hebrew (the right-to-left half of a bidirectional paragraph). CyberUI's default interface font is
the Latin face.

The upstream files are the Noto families as google/fonts publishes them, under the SIL Open Font
License 1.1 with no Reserved Font Name. Each is a megabyte or more, almost all of it scripts and
weights no test touches, so this script cuts each one down to the ranges named below:

  * subset with fontTools, keeping every OpenType layout feature — a subset that dropped GSUB or
    GPOS would test nothing HarfBuzz does;
  * the Latin face keeps its `wght` axis and is pinned at `wdth` 100, so it stays a variable font;
  * the other four are instanced at their default (Regular, wdth 100), because their tests are about
    shaping rather than variation and a static face is a fifth of the size.

The colour face is the project's own, built from nothing by `colour_font()` below: one COLRv0 glyph
of two layers and a CPAL palette, which is the smallest face that exercises the colour atlas.

--- REPRODUCING IT --------------------------------------------------------------------------------

    curl the five files named in UPSTREAM at the commit named there into DIR
    python3 tools/content/make_fonts.py DIR

The upstream digests are checked before anything is written, and deps/fonts/PROVENANCE.md records
the digest of every file this writes. fontTools 4.55 produced the committed files.
"""

from __future__ import annotations

import hashlib
import pathlib
import sys

from fontTools import subset
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTCollection, TTFont
from fontTools.varLib import instancer

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "deps" / "fonts"

GOOGLE_FONTS_COMMIT = "5e8a3ba899557829a76cfdac30fa512bda91d7ca"

# upstream file -> (sha256, output name, unicode ranges, keep the wght axis)
UPSTREAM = {
    "NotoSans[wdth,wght].ttf": (
        "bfb7bb691513f12e734dc346c03a03f784912432d7e3fa8e56efcf906fe86b3d", "NotoSans-Latin-VF.ttf",
        "U+0020-007E,U+00A0-00FF,U+2013-2014,U+2018-201D,U+2026,U+25CC", True),
    "NotoSansArabic[wdth,wght].ttf": (
        "63111b5b2e074dd48cc67692e0a2726d86ee94c1c37fe8598257b7b4e87e869e", "NotoSansArabic-Subset.ttf", "U+0020,U+0600-06FF,U+200C-200F,U+25CC", False),
    "NotoSansDevanagari[wdth,wght].ttf": (
        "14ec4af41f27482216d1c2229f417ff9b1425e1babb014e57d1d40d03229853e", "NotoSansDevanagari-Subset.ttf", "U+0020,U+0900-097F,U+200C-200D,U+25CC", False),
    "NotoSansThai[wdth,wght].ttf": (
        "5a1c559bb539583c8a1fd99d1c5b9491e5e14478c9cd2bd0970d5c3096cc9ef8", "NotoSansThai-Subset.ttf", "U+0020,U+0E00-0E7F,U+25CC", False),
    "NotoSansHebrew[wdth,wght].ttf": (
        "7ef36a2c3593758cdb622e1bdef4f84523e92fbc3ccc667438dd80ff54c2de88", "NotoSansHebrew-Subset.ttf", "U+0020,U+0590-05FF,U+25CC", False),
}


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cut(source: pathlib.Path, target: pathlib.Path, ranges: str, keep_weight: bool) -> None:
    # Subset first and instance second: fontTools' instancer leaves `gvar` lazily loaded, and the
    # subsetter then asks it for glyphs it never decoded.
    font = TTFont(source)
    options = subset.Options()
    options.layout_features = ["*"]
    options.name_IDs = ["*"]
    options.notdef_outline = True
    options.hinting = False
    options.recalc_timestamp = False
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=subset.parse_unicodes(ranges))
    subsetter.subset(font)
    limits = {"wdth": 100.0} if keep_weight else {"wdth": 100.0, "wght": 400.0}
    font = instancer.instantiateVariableFont(font, limits)
    font.recalcTimestamp = False
    font.save(target)


def box(pen: TTGlyphPen, x0: int, y0: int, x1: int, y1: int) -> None:
    pen.moveTo((x0, y0))
    pen.lineTo((x0, y1))
    pen.lineTo((x1, y1))
    pen.lineTo((x1, y0))
    pen.closePath()


def colour_font(target: pathlib.Path) -> None:
    """One COLRv0 glyph for U+25A0 (BLACK SQUARE): a red square with a blue square inside it."""
    names = [".notdef", "square", "layer.outer", "layer.inner"]
    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(names)
    builder.setupCharacterMap({0x25A0: "square"})
    glyphs = {}
    rects = {".notdef": (100, 0, 500, 700), "square": (100, 0, 900, 800),
             "layer.outer": (100, 0, 900, 800), "layer.inner": (300, 200, 700, 600)}
    for name, rect in rects.items():
        pen = TTGlyphPen(None)
        box(pen, *rect)
        glyphs[name] = pen.glyph()
    builder.setupGlyf(glyphs)
    # The left side bearing is each glyph's own left edge. TrueType rasterisers place an outline by
    # it, so a layer whose bearing disagreed with its outline would be drawn shifted onto the base.
    builder.setupHorizontalMetrics({name: (1000, rects[name][0]) for name in names})
    builder.setupHorizontalHeader(ascent=800, descent=-200)
    builder.setupNameTable({"familyName": "Cyber Colour Test", "styleName": "Regular"})
    builder.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    builder.setupPost()
    builder.setupCOLR({"square": [("layer.outer", 0), ("layer.inner", 1)]}, version=0)
    builder.setupCPAL([[(1.0, 0.0, 0.0, 1.0), (0.0, 0.0, 1.0, 1.0)]])
    builder.font.recalcTimestamp = False
    builder.font["head"].created = builder.font["head"].modified = 0
    builder.save(target)


def containers(out: pathlib.Path) -> None:
    """The two container formats FreeType reads and a plain sfnt does not exercise: WOFF (version 1,
    zlib-compressed tables) and a TrueType collection. Both are repackagings of faces cut above, so
    a test can require that the container shapes exactly as the face inside it does."""
    woff = TTFont(out / "NotoSansHebrew-Subset.ttf")
    woff.flavor = "woff"
    woff.recalcTimestamp = False
    woff.save(out / "NotoSansHebrew-Subset.woff")
    collection = TTCollection()
    collection.fonts = [TTFont(out / "NotoSansThai-Subset.ttf"),
                        TTFont(out / "NotoSansHebrew-Subset.ttf")]
    for font in collection.fonts:
        font.recalcTimestamp = False
    collection.save(out / "NotoSansThaiHebrew.ttc")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__)
        return 2
    source_dir = pathlib.Path(argv[1])
    OUT.mkdir(parents=True, exist_ok=True)
    for name, (expected, output, ranges, keep_weight) in UPSTREAM.items():
        source = source_dir / name
        found = digest(source)
        if expected and found != expected:
            print(f"{name}: sha256 {found}, expected {expected}", file=sys.stderr)
            return 1
        cut(source, OUT / output, ranges, keep_weight)
        print(f"{output}  {digest(OUT / output)}  from {name} {found}")
    colour_font(OUT / "CyberColourTest.ttf")
    print(f"CyberColourTest.ttf  {digest(OUT / 'CyberColourTest.ttf')}")
    containers(OUT)
    for name in ("NotoSansHebrew-Subset.woff", "NotoSansThaiHebrew.ttc"):
        print(f"{name}  {digest(OUT / name)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
