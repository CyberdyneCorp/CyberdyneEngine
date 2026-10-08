# Integrate FreeType, HarfBuzz, msdfgen and ICU behind the text server, import fonts, and draw them

## Why

`text-and-fonts` names HarfBuzz, ICU and FreeType in its normative text, and the engine integrated
none of them: it could lay out Hebrew, Arabic and Thai with in-tree algorithms but load only
image-grid fonts, and nothing drew a glyph from a real font. CyberUI drew its interface in a 6x13
bitmap font #102 compiled in as a stand-in. Four requirement rows were exemptions — font loading,
rasterisation and atlas, text rendering, and font import and diagnostics — and
`m11e:dependency-set-integrated` counted the four libraries as the shortfall. Issue #86.

## What changes

- `deps/manifest.toml`: HarfBuzz 14.6.0, FreeType 2.14.3, msdfgen 1.13 and ICU 78.3, pinned to
  commits with their licences; `cmake/dependencies.cmake` configures each to what the engine uses
  (FreeType without bzip2, libpng, Brotli or its HarfBuzz hook; HarfBuzz with no subsetter or
  platform shaper; msdfgen's core; ICU as the sixteen data-free `libicuuc` files of its
  bidirectional algorithm). Two feature options gate them: `CY_TEXT` and `CY_TEXT_ICU`, both on.
  `THIRD_PARTY.md` regenerated; `tools/layercheck/layercheck.py` confines the four libraries' headers
  to the backend.
- `src/servers/text/`: `TextBackend`, the seam the libraries sit beneath; `start_with`; outline faces
  from a `FontSource` or a `CookedFont`; one atlas per `PixelFormat` (coverage, distance field,
  colour); face runs; bidirectional lines in visual order, with src/text/'s algorithm as the
  fallback; the cooked-font format; per-frame rasterisation counts, the spike report and the
  fallback report. `FontDesc` gains hinting, feature overrides and a distance range.
- `src/backends/text-complete/`: `CompleteTextBackend` — FreeType faces, rasters, COLR colour and
  outlines; HarfBuzz shaping and the GSUB closure; msdfgen distance fields; ICU's bidi.
- `tools/import/`: `FontImporter` — rendering mode, pre-rendered ranges (with every glyph the GSUB
  closure reaches) baked into atlas pages, fallback chain, feature defaults and variable instance.
- CyberUI: `BuiltinMaterial::GlyphField` in the interface shader; `TextPainter` on an outline face
  with outline, shadow, gradient and per-character colour; the interface font (Noto Sans, Latin,
  compiled in with `CY_TEXT`) replacing the built-in bitmap font, which stays as the fallback face and
  as the whole of the text with `CY_TEXT` off; `upload_text_atlases`; the strategy sample's HUD in
  the interface font.
- `deps/fonts/`: Noto subsets (OFL 1.1) and a COLR test face, produced by
  `tools/content/make_fonts.py` from pinned upstream digests.
- Tests: `unit.text` (over a fake backend), `integration.text_complete`, `integration.import_font`,
  `integration.ui_interface_font`, `unit.ui_render`'s distance-field case, `render.ui` (h) and (i).

## Scope

Not built, each declared false or refused with its reason: WOFF2, CBDT/sbix/SVG colour glyphs, LCD
subpixel rendering, subpixel positioning, vertical layout, system-font queries, ICU's data (so no
dictionary breaking from shipped dictionaries and no locale formatting), font-table kashida, shadow
blur, and text in 2D world space and 3D.

## Impact

- Specs: `text-and-fonts` (the backend seam, colour rasters, the closure in pre-rendered ranges, the
  per-frame report), `build-system-and-platforms` (the two options).
- Every build with `CY_TEXT` fetches FreeType, HarfBuzz and msdfgen; with `CY_TEXT_ICU` ICU's 390 MB
  tree for a sixteen-file build. `-D CY_TEXT=OFF -D CY_TEXT_ICU=OFF` fetches none and the text
  suites that do not need them still run.
