# Falsification

Each mutation was applied to the named file by `mutate.py` (an exact, single-occurrence text
replacement), the suite's target rebuilt with `cmake --build build/dev --target <suite>`, the suite
run, and the file restored with `git checkout` and its md5 compared with the one taken before the
edit. Every row below went RED in the cases listed and was restored; the suite passed again after the
final rebuild. `render.ui` rows ran on the development machine's Vulkan device (RTX 5060).

| # | File | Mutation | Suite | Cases that failed |
|---|---|---|---|---|
| 1 | `src/backends/text-complete/src/harfbuzz_shaper.cpp` | HarfBuzz's Y-up offsets passed through unflipped | `integration.text_complete` | shaping: Arabic joins, ligates lam-alef and places its marks, in visual order |
| 2 | `src/backends/text-complete/src/harfbuzz_shaper.cpp` | the HarfBuzz scale set to the size in 26.6 halved | `integration.text_complete` | complete: synthetic bold widens a glyph and synthetic italic leans it; shaping: Latin kerns AV and ligates ffi, as HarfBuzz shapes it; shaping: Arabic joins, ligates lam-alef and places its marks, in visual order; shaping: Devanagari moves its vowel sign before the consonant and forms conjuncts; shaping: Thai decomposes sara am and moves the tone mark clear of it |
| 3 | `src/backends/text-complete/src/freetype_faces.cpp` | the variable-font instance never pinned in FreeType | `integration.text_complete` | complete: a variable face rasterises one glyph at two weights into two cache entries; complete: an axis the font does not have is refused, not ignored |
| 4 | `src/backends/text-complete/src/msdf_generator.cpp` | the distance field's range halved | `integration.text_complete` | complete: one distance-field entry draws a glyph at two scales within the edge bound |
| 5 | `src/backends/text-complete/src/msdf_generator.cpp` | msdfgen's bottom-up rows copied top-down | `integration.text_complete` | complete: one distance-field entry draws a glyph at two scales within the edge bound |
| 6 | `src/backends/text-complete/src/freetype_faces.cpp` | FreeType's BGRA copied with blue in the red channel | `integration.text_complete` | complete: a COLR glyph lands in the colour atlas in its palette's colours |
| 7 | `src/backends/text-complete/src/freetype_faces.cpp` | FT_LOAD_COLOR never set, so COLR layers are not blended | `integration.text_complete` | complete: a COLR glyph lands in the colour atlas in its palette's colours |
| 8 | `src/backends/text-complete/src/freetype_faces.cpp` | synthetic bold not applied to the raster | `integration.text_complete` | complete: synthetic bold widens a glyph and synthetic italic leans it |
| 9 | `src/backends/text-complete/src/freetype_faces.cpp` | WOFF2 not recognised, so FreeType's generic error is returned | `integration.text_complete` | complete: WOFF2 is refused, naming the decompressor this build leaves out |
| 10 | `src/backends/text-complete/src/complete_backend.cpp` | a WOFF face shaped from its compressed bytes | `integration.text_complete` | complete: WOFF and a collection shape exactly as the face inside them |
| 11 | `src/backends/text-complete/src/icu_bidi.cpp` | the empty-text guard removed (the regression) | `integration.text_complete` | complete: an empty line lays out, and an empty paragraph resolves to no runs |
| 12 | `src/backends/text-complete/src/icu_bidi.cpp` | a right-to-left paragraph handed to ICU as left to right | `integration.text_complete` | shaping: ICU pairs brackets in a right-to-left paragraph, which src/text does not |
| 13 | `src/backends/text-complete/src/harfbuzz_shaper.cpp` | the GSUB closure replaced by .notdef alone | `integration.text_complete` | complete: the glyph closure reaches the ligature the character map does not |
| 14 | `src/servers/text/src/server.cpp` | level runs placed in logical rather than visual order | `unit.text` | backend: a number in right-to-left text keeps its digits in order and its runs in place |
| 15 | `src/servers/text/src/server.cpp` | level runs placed in logical rather than visual order | `integration.text_complete` | complete: a number in Hebrew keeps its digits in order and the runs in visual order |
| 16 | `src/servers/text/src/server.cpp` | a right-to-left run's face runs placed left to right | `unit.text` | backend: a right-to-left run lists its face runs right to left |
| 17 | `src/servers/text/src/server.cpp` | every raster filed in the coverage atlas | `unit.text` | backend: each raster lands in the atlas of its format |
| 18 | `src/servers/text/src/server.cpp` | a cooked glyph counted as rasterised | `unit.text` | cooked: a cooked font round-trips, and its pre-rendered glyphs are not rasterised |
| 19 | `src/servers/text/src/server.cpp` | a fallback not counted against its primary | `unit.text` | diagnostics: the fallback report counts against the primary that needed it |
| 20 | `src/servers/text/src/server.cpp` | the frame's starting count never moved | `unit.text` | diagnostics: a frame that rasterises past the threshold is a spike |
| 21 | `src/servers/text/src/server.cpp` | a right-to-left paragraph's Start left at the left | `unit.text` | backend: a right-to-left paragraph starts at the right |
| 22 | `src/servers/text/src/cooked_font.cpp` | a glyph rectangle outside its page accepted | `unit.text` | cooked: every truncation of a cooked font is refused rather than read past its end |
| 23 | `src/servers/text/src/atlas.cpp` | a repack copying a row's width rather than its bytes | `unit.text` | atlas: a four-byte atlas keeps whole pixels through a repack |
| 24 | `tools/import/src/font.cpp` | the cook pre-renders the character map's glyphs without the closure | `integration.import_font` | font: a cooked Latin range lays Latin out with no runtime rasterisation; font: the cook records the mode, the instance, the features and the fallbacks |
| 25 | `tools/import/src/font.cpp` | a feature's value ignored | `integration.import_font` | font: the cook records the mode, the instance, the features and the fallbacks |
| 26 | `tools/import/src/font.cpp` | every glyph's page written as the coverage page | `integration.import_font` | font: the cook records the mode, the instance, the features and the fallbacks |
| 27 | `src/ui/text/src/text_painter.cpp` | the field's range not doubled | `integration.ui_interface_font` | interface font: a size scales the quads and the distance range together |
| 28 | `src/ui/text/src/text_painter.cpp` | no shadow pass | `integration.ui_interface_font` | interface font: an outline and a shadow come from the same entry, in one batch |
| 29 | `src/ui/text/src/text_painter.cpp` | colour spans ignored | `integration.ui_interface_font` | interface font: a gradient runs across the line and a colour span overrides it |
| 30 | `src/ui/text/src/text_painter.cpp` | set_text leaves the new glyphs for painting to rasterise | `integration.ui_interface_font` | interface font: setting text makes its glyphs resident, so painting never grows a page |
| 31 | `src/ui/text/src/text_painter.cpp` | a line scaled by the em rather than by the line height | `integration.ui_interface_font` | interface font: a line is the built-in font's height, so its layouts keep their rows |
| 32 | `src/ui/render/src/encode.cpp` | the outline band dropped on the host | `unit.ui_render` | ui_render: a distance-field glyph is its colour inside, the outline's in the band |
| 33 | `src/ui/render/src/encode.cpp` | the host reference samples the field by point | `render.ui` | (h) a paragraph in the interface font is one draw, shaded as the host shades it |
| 34 | `src/ui/render/src/text_atlas.cpp` | the distance-field page uploaded as one byte a texel | `render.ui` | (h) a paragraph in the interface font is one draw, shaded as the host shades it |
| 35 | `src/ui/text/src/text_painter.cpp` | the outline drawn transparent | `render.ui` | (i) an outlined, shadowed title matches its golden image |

The layer rule: deleting `"unicode": ICU` from `THIRD_PARTY_DIRECTORIES` in
`tools/layercheck/layercheck.py` turns `tools/layercheck/selftest.py`'s `thirdparty-above-backends`
case red ("diagnostic does not mention: unicode/ubidi.h"); restored, 11 of 11 pass.

## Mutations that first stayed green, and what changed

- **Run order.** Iterating a line's level runs in logical rather than visual order passed every case
  at first: in "ab שלום cd" the one right-to-left run is alone at its level, so L2's order of RUNS is
  the logical one and only the glyphs inside the run reverse. A number inside Hebrew ("של 12 אב") has
  three level runs whose visual order differs from their logical one; that case was added to
  `unit.text` and `integration.text_complete`, and both go red under the mutation (rows above).
- **ICU's paragraph level.** Handing a right-to-left paragraph to ICU as "auto" changed nothing,
  because the case's text begins with a Hebrew letter and auto resolves it right to left. The mutation
  recorded hands it in as left to right instead.
- **Three mutations that did not compile** (an unused helper or variable under `-Werror`) were
  rewritten to keep the helper referenced and the effect the same.

## The bug the suites found

`render`ing the strategy sample's HUD in the interface font failed with "the interface was not built:
U_ILLEGAL_ARGUMENT_ERROR". ICU's `ubidi_setPara` refuses a null text pointer even at length zero, and
an empty array's data is null, so any empty label stopped the interface being built. The regression
case "complete: an empty line lays out, and an empty paragraph resolves to no runs" was written first
and failed (`REQUIRE(laid.has_value())`, `REQUIRE(resolved.has_value())`, the paragraph level 0
against 1); `icu_resolve_bidi` now answers an empty paragraph with no runs and the level asked for,
and the case passes. Row "the empty-text guard removed" is the same case going red again.

## A test font that was wrong, not the backend

The COLR case first failed with the inner blue square drawn at the left edge. FreeType places a
TrueType outline by its `hmtx` left side bearing; `make_fonts.py` had given every glyph a bearing of
100 units while the inner layer's outline starts at 300, so the layer was shifted onto the base. The
generator now sets each glyph's bearing to its outline's left edge; the backend was not changed.
