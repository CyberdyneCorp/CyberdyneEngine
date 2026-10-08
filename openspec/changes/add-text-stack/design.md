# Design: the text stack

## A seam at layer 2, the libraries at layer 3

`TextServer` is layer 2 and cannot name a layer-3 type, so the libraries sit beneath an interface the
server owns, `TextBackend`, and the caller hands an implementation to `start_with` — the arrangement
`AudioServer::initialize_with` and miniaudio have. Everything crossing the seam is an engine type:
glyph indices, `GlyphRaster` in one of three pixel formats, `BackendGlyph` positions in pixels with Y
down, `BidiResult` runs in byte offsets. A second backend is a second implementation of one header.

The server keeps everything a caller observes — the fallback chain, the atlases, the shaping cache,
line breaking, the paragraph — so one set of tests covers both backends, and `unit.text` covers the
server's backend logic against a fake one in builds with `CY_TEXT` off.

## One library, one file

FreeType, HarfBuzz, msdfgen and ICU are each included by exactly one translation unit of
`src/backends/text-complete/`, and linked `PRIVATE`. An outline travels from FreeType to msdfgen as a
`GlyphOutline` of verbs and points, a WOFF face's decompressed sfnt from FreeType to HarfBuzz as
bytes. `layercheck.py`'s `thirdparty` rule names their headers.

## Runs of one face, lines of one level

HarfBuzz must see whole words to join, ligate and kern, so the server itemises by face rather than by
character, and a combining mark or joiner stays in its base's face. A line is split into embedding
level runs (ICU's, or src/text/'s with `CY_TEXT_ICU` off), each shaped in its own direction through
the shaping cache, and the runs are placed by L2's visual order — so a Hebrew word in an English line
is cached on its own and survives edits around it.

## Three atlases, decided per glyph

A glyph's page is what the rasteriser produced, not what was asked: coverage (one byte), a
multi-channel distance field with the true distance in alpha (four), or premultiplied colour from
COLR layers (four). A grayscale face whose glyph has colour layers files it in colour. Each page is
started the first time a glyph of its format arrives, so a server that never draws an emoji never
allocates a colour page.

## Distance fields, and why the interface uses one

The specification asks for grayscale as the default for UI at fixed sizes. CyberUI's documents are
in reference units scaled to the display (`ui::resolve_scale`), so its text is not at a fixed size,
and the interface font is a distance field: one entry draws at every scale, and an outline is a
threshold of the same field (`GlyphField`'s band), which is the specification's "via MSDF distance
thresholding, not by drawing the text eight times". A painter on a grayscale face draws
`BuiltinMaterial::Glyph` as before.

A field is generated unhinted at the face's size, padded by the range, with 0.5 at the edge. msdfgen's
sign follows the winding, which TrueType and CFF disagree on; the padded corner is outside by
construction, so a field that reads inside there is inverted.

## Cooked fonts and the closure

The cooked font carries the face description, the font file (shaping a string the cook did not
anticipate needs the tables), the pre-rendered glyphs and their pages, and the fallback chain. A cook
pre-renders the character map's glyphs for the ranges and every glyph HarfBuzz's GSUB closure reaches
from them, under the face's feature defaults, so shaping cooked text — ligatures and oldstyle figures
included — rasterises nothing. The runtime copies those glyphs into the live atlases as preloaded.
The format lives at layer 2 because the runtime reads it with no importer present.

## ICU with no data

The bidirectional algorithm reads properties `libicuuc` compiles in, so sixteen files and no data
file give the full UAX #9 (isolating run sequences, N0). Line breaking dictionaries and locale
formatting need ICU's data, which ICU's git tree carries only as sources for ICU's own tools; that
toolchain is not built here, and the decision is recorded in src/backends/text-complete/README.md
with `CY_TEXT_ICU` as the option that drops ICU entirely.

## The UI

`TextPainter` gains an outline start with the built-in grid font pushed behind the outline face. A
style with no size draws a line exactly the built-in font's height, so the HUD and the console keep
their rows. Glyphs are made resident when text is set, outside a frame, so painting only finds and a
frame's uvs never move. A line in one face is one batch: shadows first, then glyphs, one material,
one page.

## Tests and the oracle

The shaping goldens are uharfbuzz's (HarfBuzz's own binding at the pinned version), so they test the
engine's path to HarfBuzz rather than echo it. src/text/ is the oracle for Arabic joining and bidi
levels, and the one case where they differ (N0) is asserted both ways. The distance field is compared
with FreeType's own raster at two scales. `render.ui` (h) holds the device to the host shader within
one step and (i) pins an outlined title to a golden. Every case is proven red by a mutation:
`evidence/falsification.md`.
