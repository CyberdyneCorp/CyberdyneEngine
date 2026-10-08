# `src/backends/text-complete/` — layer 3

Target `cy::text-complete`, header `<cy/backends/text/complete_backend.h>`, namespace `cy::text`.
Issue #86, governed by `text-and-fonts` and `thirdparty-dependencies`. Behind `CY_TEXT` (ICU behind
`CY_TEXT_ICU`), both on by default.

`CompleteTextBackend` implements `cy::text::TextBackend` (src/servers/text/) over the four libraries
`text-and-fonts` names. The text server keeps the fallback chain, the atlases, the shaping cache and
layout; this module answers the four questions the server cannot: what is in a font file, what a
glyph looks like, how a run of text shapes, and what the Unicode Bidirectional Algorithm says.

## One library, one file

| File | Library | What it does |
|---|---|---|
| `src/freetype_faces.cpp` | FreeType 2.14.3 | Opens TrueType, OpenType (glyf and CFF), collections and WOFF from memory; pins the variable-font instance; sizes the face; hinting (none, light, full); synthetic bold (`FT_GlyphSlot_Embolden`) and italic (`FT_GlyphSlot_Oblique`); grayscale and monochrome rasters; COLR layers blended into a colour raster; outlines for the distance field; the decompressed sfnt of a WOFF file |
| `src/harfbuzz_shaper.cpp` | HarfBuzz 14.6.0 | Shapes one run in one face and one direction through `hb_ot_font`: GSUB and GPOS, the Arabic, Indic and Thai shapers, mark attachment, kerning, the face's feature defaults and the request's overrides; the GSUB glyph closure a font importer pre-renders |
| `src/msdf_generator.cpp` | msdfgen 1.13 (core) | Multi-channel signed distance fields with the true distance in alpha, from the outline FreeType decomposed |
| `src/icu_bidi.cpp` | ICU 78.3 (`libicuuc`, sixteen files) | The Bidirectional Algorithm in full: isolating run sequences, N0 bracket pairs; a stub with `CY_TEXT_ICU` off |
| `src/complete_backend.cpp` | none | The class: faces opened in both FreeType and HarfBuzz over the same bytes, size and instance, and the routing between them |

The four meet in `src/internal.h`, in engine types: an outline crosses from FreeType to msdfgen as a
`GlyphOutline` of verbs and points, never as an `FT_Outline`. `tools/layercheck/layercheck.py`
fails the build if any of the four libraries' headers is included outside this directory, and each
is linked `PRIVATE`, so a consumer cannot even resolve one.

## Conventions every caller relies on

- **Pixels, Y down.** Advances, offsets and bearings are in the face's pixels with Y growing down
  from the baseline, as everything in `cy::text` is. HarfBuzz's 26.6 fixed point with Y up and
  FreeType's pitched bitmaps are converted here.
- **Visual order.** `shape` returns a right-to-left run already reversed — left to right on the page —
  as HarfBuzz does; clusters are UTF-8 byte offsets into the run.
- **The same instance in both libraries.** Size, `wght`-style axes and synthetic bold are applied to
  the FreeType face and the HarfBuzz font alike (a synthetic bold widens the advance by the em over
  twenty-four in both), so a shaped advance and a rasterised glyph agree.
- **A colour glyph is colour.** A face asked for grayscale whose glyph has COLR layers rasterises it
  as `PixelFormat::Colour`, and the server files it in the colour atlas.
- **Distance fields are unhinted and padded by the range.** A distance-field raster is the outline's
  bounds on whole pixels grown by `FontDesc::distance_range` on every side; 0.5 is the edge and the
  range either side spans [0, 1]. A field that reads inside at its padded corner is inverted, which is
  how a CFF outline's opposite winding is handled.

## What is not built, and why

| Not built | Why | What happens instead |
|---|---|---|
| WOFF2 | Needs Brotli, which `cmake/dependencies.cmake` leaves out of FreeType | `open_face` refuses with `Unsupported`, naming Brotli |
| CBDT and sbix colour bitmaps | Stored as PNG; libpng is left out (`FT_DISABLE_PNG`) | COLR (v0 layers) is the colour path |
| LCD subpixel rendering | Needs a three-channel coverage atlas and a known panel order | `open_face` refuses `RenderMode::SubpixelLcd` |
| Subpixel positioning | Multiplies the atlas by four for what a distance field already gives | `subpixel_positioning` is false |
| Vertical layout | Not built | `shape` refuses `Direction::TopToBottom`; `vertical_layout` is false |
| Dictionary line breaking | Needs ICU's data (below) | `dictionary_line_breaking` is false; src/text/'s `WordList` deferral is the path |
| Number, currency and date formatting | Needs ICU's data and `libicui18n` | src/text/'s localisation covers plurals and messages |
| FreeType's and HarfBuzz's allocations through the engine allocator | Their hooks free without a size, the engine's allocator needs one | they use the C heap; the backend's own state uses the allocator it is given |

### ICU's data subset: none, and why

`text-and-fonts` asks for "the ICU data subset, chosen and documented, with a build option to drop
it". The subset this build ships is **empty**. Everything compiled here is data-free: the
bidirectional algorithm reads character properties `libicuuc` compiles in as tables
(`ubidi_props_data.h`, `uchar_props_data.h`, `ucase_props_data.h`), and nothing in the sixteen files
opens an ICU data file.

The data-dependent services — the line-break rules, the Thai, Lao, Khmer and Burmese dictionaries,
locale formatting — would need `icudt78l.dat` or a subset of it, and ICU's git tree carries that
data only as sources that ICU's own tools (`genbrk`, `gendict`, `genrb`, `pkgdata`) compile. Building
that toolchain inside this build is a project of its own, and it is recorded as the next step rather
than approximated. `CY_TEXT_ICU=OFF` drops ICU entirely — no fetch of its 390 MB tree — and the
server then resolves levels with src/text/'s algorithm, which reports through
`BidiResult::approximated` where an isolate made its answer approximate.

## Testing

`integration.text_complete` (tests/), over the fonts in `deps/fonts/`:

- shaping goldens for Latin (kerning, the `ffi` ligature), Arabic (joining, lam-alef, marks, right to
  left), Devanagari (the pre-base vowel sign, conjuncts) and Thai (sara am, the shifted tone mark) —
  produced by uharfbuzz 14.6.0, HarfBuzz's own binding, not by this backend;
- Arabic contextual forms compared letter by letter with src/text/'s joining, and bidirectional
  levels compared run by run with src/text/'s algorithm; plus the N0 bracket case where ICU is right
  and src/text/ is not;
- a variable face rasterised at two weights into two cache entries; an unknown axis refused;
- one distance-field entry drawn at two scales within an edge-error bound of FreeType's own raster;
- a COLR glyph in the colour atlas in its palette's colours;
- a mixed Latin and Hebrew line in visual order, with the fallback reported against the primary;
- WOFF and a collection shaping exactly as the face inside them; WOFF2 refused;
- synthetic bold and italic; the GSUB closure; LCD refused; an empty line and an empty paragraph.

The server's own logic over a backend is `unit.text`, against a fake backend, so it runs with
`CY_TEXT` off.
