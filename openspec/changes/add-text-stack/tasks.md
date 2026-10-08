# Tasks

- [x] Pin HarfBuzz, FreeType, msdfgen and ICU in `deps/manifest.toml`, configure them in `cmake/dependencies.cmake`, add `CY_TEXT` and `CY_TEXT_ICU`, and regenerate `THIRD_PARTY.md`.
- [x] Confine the four libraries' headers to `src/backends/text-complete/` in `tools/layercheck/layercheck.py`, with selftest fixtures.
- [x] Add `TextBackend`, `start_with`, outline faces, the per-format atlases, face runs, bidirectional lines, the cooked-font format and the per-frame and fallback reports to `src/servers/text/`.
- [x] Add `CompleteTextBackend`: FreeType faces, rasters and outlines, COLR colour, HarfBuzz shaping and the closure, msdfgen fields, ICU bidi.
- [x] Add `FontImporter` to `tools/import/` and register it.
- [x] Add `BuiltinMaterial::GlyphField` to the interface shader and its host reference; regenerate the committed SPIR-V and MSL.
- [x] Start `TextPainter` on an outline face with outline, shadow, gradient and per-character colour; compile in the interface font; keep the built-in font as the fallback; add `upload_text_atlases`; move the strategy sample's HUD to the interface font.
- [x] Produce `deps/fonts/` with `tools/content/make_fonts.py` and record their provenance.
- [x] Add `unit.text` backend cases, `integration.text_complete`, `integration.import_font`, `integration.ui_interface_font`, the `unit.ui_render` distance-field case and `render.ui` (h) and (i); prove each red by a mutation.
- [x] Build and run the text and UI suites with `CY_TEXT` and `CY_TEXT_ICU` off.
- [x] Update the READMEs, `docs/guides/text.md` and `docs/guides/ui.md`, `deps/`, the requirements map, and validate this change.
