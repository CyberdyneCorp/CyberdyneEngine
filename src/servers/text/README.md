# `src/servers/text/` — layer 2

Layer 2, target `cy::servers-text`, headers `<cy/servers/text/*.h>`, namespace `cy::text`. M5 task
5.3 (the interface, the atlas, the minimal backend) and issue #86 (the backend seam, outline faces,
cooked fonts, bidirectional lines), governed by `text-and-fonts`.

`TextServer` is the engine's one interface to fonts and text: font loading and querying, glyph
rasterisation and atlas management, shaping, line breaking, justification, cursor and hit-testing, and
measurement. Every one of those is on the interface. What differs between builds is what the BACKEND
behind it can do, and a caller finds that out from `capabilities()` rather than from which functions
exist.

## What is here

| Header | What it owns |
|---|---|
| `text.h` | The vocabulary: direction, render mode, `PixelFormat`, overflow, alignment, `TextCapabilities`, `FontMetrics`, `TextDiagnostics` (with the per-frame report), `FallbackReport` |
| `font.h` | `FontHandle`, `FontDesc` (size, mode, axes, hinting, feature overrides, synthetic styles, distance range), `FontSource`, the fallback chain, and `ImageGridFont` |
| `backend.h` | `TextBackend`: the seam the outline-font libraries sit beneath — faces, rasters, shaping, the glyph closure and the bidirectional algorithm, in engine types |
| `atlas.h` | The glyph atlas: dynamic packing over `cy::geom::AtlasPacker`, growth to a device limit, least-recently-used eviction, a thrash counter, and one or four bytes a pixel |
| `cooked_font.h` | The cooked font the importer writes and `create_face(const CookedFont&)` reads: the face, the font file, the pre-rendered glyphs and their pages, the fallback chain |
| `layout.h` | `ShapedGlyph`, `ShapedRun`, `TextLine`, `TextParagraph`, carets, hit-testing, inline objects and break opportunities |
| `server.h` | The server: faces, three atlases, the shaping cache, face runs, bidirectional lines and the paragraph |

## Two backends

**The minimal backend** — `start(config)` — is the one `text-and-fonts` carves out for
size-constrained builds: image-grid faces, one glyph per codepoint, left to right. Every field of its
`TextCapabilities` is `false`, and a right-to-left or vertical request is **refused** with
`Unsupported`, not approximated. An outline face on it is refused naming `CY_TEXT`.

**The complete backend** — `start_with(config, backend)` with a `CompleteTextBackend` from
src/backends/text-complete/ — is FreeType, HarfBuzz, msdfgen and ICU. Layer 2 cannot construct a
layer-3 object, so the caller hands it in; `start` with `BackendKind::Complete` names the libraries and
the call rather than silently downgrading. With it the server:

- **creates outline faces** from a `FontSource` (TrueType, OpenType, collection, WOFF) or a
  `CookedFont`, beside image-grid faces — a grid font can still be the last face of a chain;
- **itemises by face**: consecutive codepoints the chain answers with the same face are one run, so
  HarfBuzz sees whole words and joins, ligates and kerns across them; a combining mark or joiner stays
  in its base character's face;
- **lays a line out bidirectionally**: levels from the backend (ICU), or from src/text/'s algorithm
  when the backend has none, each level run shaped in its own direction through the shaping cache and
  placed in visual order; a right-to-left paragraph aligns `Start` to the right;
- **files each raster by what it is**: coverage, distance field or colour, each in its own atlas
  (`atlas(PixelFormat)`), started the first time a glyph of that format is placed.

## Three atlases, and the key that keeps them honest

A glyph's slot is keyed by face, glyph and subpixel bucket; the face folds in size, axes, hinting and
render mode, because a face is created per instance. So a variable font at two weights is two faces
and two sets of entries, and a glyph at 12 pixels never draws the raster made for 11. The page a slot
is on is its `PixelFormat` — and a glyph asked for in grayscale that has colour layers lands in the
colour page, because the format is decided by what was rasterised, not by what was asked.

## A font is data, and the server does not open files

A font arrives as bytes the caller supplies — an `ImageGridFont`, a `FontSource`, or a cooked font —
and the bytes must outlive every face made from them, which an asset held by the asset system
satisfies. This is layer 2 with no filesystem, and a cooked font is produced by the font importer
(tools/import/, `font.h`) with its pre-rendered ranges, fallback chain, feature defaults and instance
already decided.

`create_face(const CookedFont&)` copies the cooked pages' glyphs into the live atlases and counts them
as `glyphs_preloaded`, so laying out a cooked range rasterises nothing — which
`integration.import_font` asserts from this counter.

## Diagnostics

`TextDiagnostics` carries atlas occupancy, evictions, growths, thrashes, shaping-cache hits and
misses, fallbacks, `.notdef` boxes served, rasterisations and preloads — and, closed by `end_frame()`,
the per-frame report `text-and-fonts` asks for: the last frame's rasterisations, the peak, and the
number of frames past `TextServerConfig::rasterisation_spike` (the spike report). `fallback_report()`
lists each primary face with how many codepoints it shaped and how many a later face answered, the
report of "fonts that trigger fallback frequently".

## Nothing here is behind a `CY_*` option

The interface, the atlases, the cooked-font format and the minimal backend compile in every build,
so their suites run in every configuration. `unit.text` tests the server's backend logic against a
fake backend (tests/fake_backend.h) for the same reason: face runs, bidirectional lines, the three
atlases, cooked fonts and the per-frame report are covered with `CY_TEXT` off.
