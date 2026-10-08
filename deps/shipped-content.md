## Shipped content

**M11.c task 6.2.** `thirdparty-dependencies`' governance applies to content the project ships as
much as to code it links, and M11.c is the rung that first made the project ship any.

Everything under `content/` is **the project's own**, licensed as the rest of this repository is
([`LICENSE`](LICENSE)). Nothing in it is a third-party asset and nothing in it is derived from one,
which is why no entry appears above for any of it.

| what | where | provenance |
|---|---|---|
| Nine PNG textures | `content/beauty/textures/` | generated from seed `0x5EEDB107` by `tools/content/make_beauty_textures.py` — [the record](content/beauty/textures/PROVENANCE.md) |
| Three material graphs | `content/beauty/materials/` | authored on the editor's own node-graph canvas by `cy-author-material`; the `.cygraph` is written by the engine |
| Six primitive sources | `content/beauty/meshes/` | six-line `.cyprim` files; the geometry is `cy::import::build_primitive_mesh`'s |
| One scene | `content/beauty/shot.cyshot` | the camera, the sun, the grade and thirty-one placements |

**The rule this establishes, for whoever adds the next asset**: a file under `content/` carries a
`PROVENANCE.md` beside it naming its licence and either the generator and seed that produce it or the
source it came from and the terms it came under. A file that carries neither does not get committed.

### Data compiled into the engine

One third-party work is compiled into the engine as data rather than linked as code:

| what | where | provenance |
|---|---|---|
| The built-in interface font: printable ASCII from the X Window System's `misc-fixed` 6x13 bitmap font | `src/ui/text/src/builtin_font_data.h` | **public domain** — the font's own `COPYRIGHT` property reads "Public domain font. Share and enjoy." Generated from `/usr/share/fonts/X11/misc/6x13.pcf.gz` (the `xfonts-base` package) by `src/ui/text/tools/make_builtin_font.py` |

It was a stand-in until the engine could draw real fonts (`text-and-fonts`, issue #86). It stays as
the interface's fallback face, and as the whole of its text in a build with `CY_TEXT` off.

### Fonts

Issue #86. Fonts the project carries in [`deps/fonts/`](fonts/PROVENANCE.md), every one produced by
`tools/content/make_fonts.py` from a pinned upstream file:

| what | where | provenance |
|---|---|---|
| CyberUI's interface font: Noto Sans, Latin and Latin-1, its weight axis kept, compiled into `cy::ui-text` with `CY_TEXT` | `deps/fonts/NotoSans-Latin-VF.ttf` | **SIL Open Font License 1.1** ([`deps/fonts/OFL.txt`](fonts/OFL.txt)), no Reserved Font Name. Copyright 2022 The Noto Project Authors. Subset from google/fonts at `5e8a3ba8` |
| Test faces: Noto Sans Arabic, Devanagari, Thai and Hebrew, subset and instanced at Regular; the Hebrew subset as WOFF; the Thai and Hebrew subsets as a collection | `deps/fonts/NotoSans*-Subset.*`, `deps/fonts/NotoSansThaiHebrew.ttc` | **SIL Open Font License 1.1**, as above. Read by the tests; not compiled into anything |
| `CyberColourTest.ttf`: one COLRv0 glyph | `deps/fonts/` | the project's own, licensed as this repository is; built from nothing by the same script |
