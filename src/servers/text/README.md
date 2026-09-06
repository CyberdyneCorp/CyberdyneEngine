# `src/servers/text/` — layer 2

Layer 2, target `cy::servers-text`, headers `<cy/servers/text/*.h>`, namespace `cy::text`. M5 task
5.3, governed by `text-and-fonts`, which reaches **Seed** here.

`TextServer` is the engine's one interface to fonts and text: font loading and querying, glyph
rasterisation and atlas management, shaping, line breaking, justification, cursor and hit-testing, and
measurement. Every one of those is on the interface. What differs between M5 and M8 is what the
BACKEND behind it can do, and a caller finds that out from `capabilities()` rather than from which
functions exist.

## What is here

| Header | What it owns |
|---|---|
| `text.h` | The vocabulary: direction, render mode, overflow, alignment, `TextCapabilities`, `FontMetrics`, `TextDiagnostics` |
| `font.h` | `FontHandle`, `FontDesc`, variable-font axes, the fallback chain, and `ImageGridFont` — the format a caller supplies |
| `atlas.h` | The glyph atlas: dynamic packing over `cy::geom::AtlasPacker`, growth to a device limit, least-recently-used eviction, and a thrash counter |
| `layout.h` | `ShapedGlyph`, `ShapedRun`, `TextLine`, `TextParagraph`, carets, hit-testing, inline objects, UTF-8 decoding and break opportunities |
| `server.h` | The server: faces, the atlas, the shaping cache, and the layout entry points |

## The minimal backend, and what it says about itself

`text-and-fonts` carves out exactly one: "A **minimal backend** SHALL be available for
size-constrained builds, supporting only simple left-to-right layout without shaping or ICU." That is
what is behind the interface at M5, and every field of its `TextCapabilities` is `false`:

* `complex_shaping` — one glyph per codepoint, no contextual forms, no ligatures, no reordering.
* `bidirectional` — a right-to-left or vertical request is **refused** with `Unsupported`, not
  approximated. A backend that produced left-to-right glyphs for a right-to-left request would be
  lying in a way the caller cannot detect.
* `dictionary_line_breaking` — breaks after spaces, tabs and hyphens, required after a newline, and
  between two characters of a script without spaces. A useful approximation of UAX-14 for Latin; not
  the algorithm.
* `colour_glyphs`, `variable_fonts`, `subpixel_positioning`, `signed_distance_fields`,
  `vertical_layout`, `kashida_justification` — all absent, all declared.

HarfBuzz, ICU and FreeType are not integrated at M5 (`deps/manifest.toml`'s header says why). When
they are, they live in `src/backends/text-*/` at layer 3, which is where SDL, Jolt and miniaudio live
and for the same reason: no third-party type may appear above the backend, and the layer checker is
what enforces it.

## A font is data, and there is no built-in one

A font arrives as bytes the caller supplies. This is layer 2 — it has no filesystem — and a cooked
font is an asset, produced by the font importer `asset-import-pipeline` names, with its pre-rendered
ranges and its fallback chain already decided. A text server that loaded `.ttf` files would be a
second import path.

So a caller that wants overlay text supplies a font, and `ImageGridFont` is deliberately the cheapest
possible thing to supply: a grid of cells over a codepoint range, which is a screenshot of a terminal
font and four numbers. A codepoint no face in the chain has renders as the visible `.notdef` box the
specification requires, and the count is on the diagnostics.

## Nothing here is behind a `CY_*` option

The interface and the minimal backend compile in every build, so their suites run in every
configuration rather than in the ones somebody remembered to turn an option on for. That is the shape
`src/servers/physics/` chose, and its CMakeLists argues it at greater length.
