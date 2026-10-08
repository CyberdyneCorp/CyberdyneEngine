## MODIFIED Requirements

### Requirement: Engine-owned text interface
`TextServer` SHALL be the engine-defined interface for fonts and text layout. All engine and
game code SHALL use it; no HarfBuzz, ICU, FreeType or msdfgen type SHALL appear outside the backend.

The interface SHALL cover: font loading and querying, glyph rasterisation and atlas management,
text shaping, line breaking, justification, cursor and hit-testing, and text measurement.

The libraries SHALL sit beneath an engine-owned backend interface that the server is given, so that
the server's own behaviour — the fallback chain, the atlases, the shaping cache and layout — is the
same whichever backend answers, and is testable without the libraries.

A **minimal backend** SHALL be available for size-constrained builds, supporting only simple
left-to-right layout without shaping or ICU.

#### Scenario: Backend is replaceable
- **WHEN** the minimal backend is selected
- **THEN** the API SHALL be unchanged, and complex scripts SHALL degrade to simple glyph mapping
  with a documented capability query reporting the limitation

#### Scenario: Capability query
- **WHEN** code needs to know whether bidirectional layout is available
- **THEN** it SHALL query the capability rather than testing which backend is active

#### Scenario: A library header outside its backend
- **WHEN** a file outside the text backend module includes a FreeType, HarfBuzz, ICU or msdfgen
  header
- **THEN** the layer check SHALL fail the build, naming the backend the library belongs to

### Requirement: Glyph rasterisation and atlas
Glyphs SHALL be rasterised on demand and cached in **glyph atlases**, keyed by font, size,
variation axes, transform, and rendering mode.

Rendering modes SHALL be: **grayscale antialiasing**, **subpixel (LCD)** antialiasing,
**monochrome**, and **signed distance field** (multi-channel, MSDF).

MSDF SHALL be used where text must scale, rotate, or be rendered in 3D without re-rasterisation;
grayscale SHALL be the default for UI at fixed sizes.

Atlases SHALL be packed dynamically, grow up to a device limit, and evict least-recently-used
glyphs under pressure. Coverage, distance-field and colour rasters SHALL be kept in separate atlas
pages, and the page a glyph is filed in SHALL be decided by the raster produced, not by the mode
requested.

**Hinting** and **subpixel positioning** SHALL be configurable, since they trade crispness
against spacing accuracy.

#### Scenario: MSDF text scales cleanly
- **WHEN** MSDF text is scaled up
- **THEN** edges SHALL remain sharp without re-rasterisation, with documented limitations at
  sharp corners

#### Scenario: Atlas pressure
- **WHEN** many fonts and sizes are used
- **THEN** least-recently-used glyphs SHALL be evicted, and thrashing SHALL be reported as a
  diagnostic

#### Scenario: Colour glyphs
- **WHEN** a font provides colour glyphs (COLR/CPAL, CBDT, or SVG-in-OpenType)
- **THEN** they SHALL be rasterised in colour and stored in a colour atlas, whatever rendering mode
  the face was created with

### Requirement: Font import and diagnostics
Fonts SHALL be imported with configurable: rendering mode, pre-rendered glyph ranges (baking
common glyphs at build time to avoid runtime rasterisation hitches), fallback chain, OpenType
feature defaults, and variable-font instances.

A pre-rendered range SHALL include every glyph shaping can substitute for its codepoints under the
font's feature defaults — ligatures, contextual and alternate forms — so that shaping text drawn from
the range rasterises nothing.

The engine SHALL report: atlas occupancy and eviction rates, per-frame glyph rasterisation counts,
shaping cache hit rates, and fonts that trigger fallback frequently.

#### Scenario: Pre-rendered range
- **WHEN** a font is imported with the Latin range pre-rendered
- **THEN** those glyphs SHALL be present in the cooked atlas and require no runtime rasterisation

#### Scenario: Rasterisation hitch
- **WHEN** many new glyphs appear at once (a language switch)
- **THEN** the diagnostic SHALL report the rasterisation spike so the range can be pre-baked
