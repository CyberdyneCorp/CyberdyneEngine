# text-and-fonts Specification

## Purpose

Defines text handling: font loading and rasterisation, shaping for complex scripts,
bidirectional layout, line breaking and justification, and the glyph atlas the renderer consumes.

Text is a domain where correctness for the world's writing systems is only achievable by using
the mature libraries. The engine integrates **HarfBuzz** (shaping), **ICU** (Unicode algorithms),
and **FreeType** (rasterisation) behind an engine-owned `TextServer` interface.

## Requirements

### Requirement: Engine-owned text interface
`TextServer` SHALL be the engine-defined interface for fonts and text layout. All engine and
game code SHALL use it; no HarfBuzz, ICU, or FreeType type SHALL appear outside the backend.

The interface SHALL cover: font loading and querying, glyph rasterisation and atlas management,
text shaping, line breaking, justification, cursor and hit-testing, and text measurement.

A **minimal backend** SHALL be available for size-constrained builds, supporting only simple
left-to-right layout without shaping or ICU.

#### Scenario: Backend is replaceable
- **WHEN** the minimal backend is selected
- **THEN** the API SHALL be unchanged, and complex scripts SHALL degrade to simple glyph mapping
  with a documented capability query reporting the limitation

#### Scenario: Capability query
- **WHEN** code needs to know whether bidirectional layout is available
- **THEN** it SHALL query the capability rather than testing which backend is active

### Requirement: Font loading
The engine SHALL load: TrueType and OpenType (`.ttf`, `.otf`, `.ttc`), WOFF and WOFF2, bitmap
fonts, and image-grid fonts.

Fonts SHALL support: **variable font** axes, OpenType feature selection (ligatures, kerning,
stylistic sets, tabular figures), synthetic bold and italic where a face is unavailable, and a
**fallback chain** so glyphs missing from one font are sought in the next.

**System fonts** SHALL be queryable by family name where the platform allows.

#### Scenario: Missing glyph
- **WHEN** a codepoint is absent from the primary font
- **THEN** the fallback chain SHALL be searched, then the system fallback, and finally a
  visible `.notdef` box SHALL be rendered rather than nothing

#### Scenario: Variable font axis
- **WHEN** a weight axis value is set
- **THEN** glyphs SHALL be rasterised at that instance, and the cache SHALL be keyed by axis
  values

### Requirement: Glyph rasterisation and atlas
Glyphs SHALL be rasterised on demand and cached in **glyph atlases**, keyed by font, size,
variation axes, transform, and rendering mode.

Rendering modes SHALL be: **grayscale antialiasing**, **subpixel (LCD)** antialiasing,
**monochrome**, and **signed distance field** (multi-channel, MSDF).

MSDF SHALL be used where text must scale, rotate, or be rendered in 3D without re-rasterisation;
grayscale SHALL be the default for UI at fixed sizes.

Atlases SHALL be packed dynamically, grow up to a device limit, and evict least-recently-used
glyphs under pressure.

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
- **THEN** they SHALL be rasterised in colour and stored in a colour atlas

### Requirement: Text shaping
Text SHALL be shaped through HarfBuzz, converting a character sequence plus font, size,
language, script, direction, and feature settings into a glyph sequence with positions and
advances.

Shaping SHALL handle: ligatures, contextual forms (Arabic joining), reordering (Indic scripts),
mark positioning, kerning, and vertical layout for East Asian scripts.

Runs SHALL be segmented by script, direction, and font before shaping, and shaped results SHALL
be cached keyed by the run's content and parameters.

#### Scenario: Arabic joining
- **WHEN** Arabic text is shaped
- **THEN** contextual initial, medial, final, and isolated forms SHALL be selected correctly, and
  marks positioned per the font's tables

#### Scenario: Shaping cache
- **WHEN** the same string is laid out repeatedly with unchanged parameters
- **THEN** the shaped result SHALL be reused rather than reshaped

### Requirement: Bidirectional layout
Text SHALL be laid out per the **Unicode Bidirectional Algorithm** via ICU, resolving paragraph
direction (explicit or from first strong character), embedding levels, and visual reordering.

The engine SHALL support: direction overrides, isolates, and **structured text** hints so
technical strings (file paths, URLs, code) are ordered sensibly in RTL contexts.

#### Scenario: Mixed-direction paragraph
- **WHEN** a paragraph mixes Hebrew and English
- **THEN** runs SHALL be reordered visually with correct per-run direction, and the caret SHALL
  move logically rather than visually by default

#### Scenario: File path in an RTL UI
- **WHEN** a path is displayed with the file structured-text hint in an RTL locale
- **THEN** its separators and components SHALL be ordered so the path remains readable

### Requirement: Line breaking and justification
Line breaking SHALL follow the **Unicode line breaking algorithm** via ICU, with
dictionary-based breaking for scripts without spaces (Thai, Japanese, Chinese, Khmer).

Justification SHALL support: inter-word spacing, inter-character spacing where appropriate to the
script, **kashida** elongation for Arabic, and a configurable priority among them.

Overflow behaviour SHALL support: clipping, ellipsis (start, middle, or end), word wrap, character
wrap, and shrink-to-fit.

#### Scenario: Thai line breaking
- **WHEN** Thai text with no spaces is wrapped
- **THEN** dictionary-based breaking SHALL find valid break points

#### Scenario: Arabic justification
- **WHEN** Arabic text is justified with kashida enabled
- **THEN** elongation SHALL be inserted at positions the font marks valid, rather than only
  stretching spaces

#### Scenario: Ellipsis
- **WHEN** text exceeds its width with end-ellipsis overflow
- **THEN** trailing glyphs SHALL be replaced by an ellipsis that fits within the width

### Requirement: Text layout objects
The engine SHALL provide:

- **`TextLine`** — a single shaped line with measurement, hit-testing, and caret positioning
- **`TextParagraph`** — a wrapped, multi-line block with alignment, direction, line spacing,
  overflow behaviour, and **inline objects** (images or UI elements participating in layout)

Both SHALL expose: total size, per-line metrics (ascent, descent, baseline), glyph-to-character
mapping, hit-test from a point to a character index, and caret rectangles including split carets
at direction boundaries.

#### Scenario: Inline image
- **WHEN** an image is embedded in a paragraph with a baseline alignment
- **THEN** it SHALL occupy its advance in layout and be positioned per the alignment

#### Scenario: Hit-testing in bidirectional text
- **WHEN** a user clicks in mixed-direction text
- **THEN** the returned character index SHALL correspond to the visual position clicked, with
  correct handling at direction boundaries

### Requirement: Text rendering
Shaped text SHALL be rendered by emitting glyph quads referencing atlas regions, batched by
atlas texture.

Rendering SHALL support: fill colour and per-character colour, outline (stroke) with width and
colour, drop shadow with offset and blur, and gradient fills.

Text SHALL be renderable in 2D UI space, in 2D world space, and in 3D as a mesh or billboard.

#### Scenario: Batched text
- **WHEN** a paragraph's glyphs come from one atlas
- **THEN** they SHALL be submitted as a single batched draw

#### Scenario: Outlined text
- **WHEN** an outline is configured
- **THEN** it SHALL be rendered from a separate outline atlas entry or via MSDF distance
  thresholding, not by drawing the text eight times

### Requirement: Localisation integration
`TextServer` SHALL integrate with the localisation system so that: the active locale informs
default language and script for shaping, plural rules and number and date formatting come from
ICU, and layout direction follows the locale unless overridden.

#### Scenario: Locale change
- **WHEN** the locale changes at runtime
- **THEN** cached shaped runs SHALL be invalidated where language-dependent, and UI SHALL
  re-layout

### Requirement: Font import and diagnostics
Fonts SHALL be imported with configurable: rendering mode, pre-rendered glyph ranges (baking
common glyphs at build time to avoid runtime rasterisation hitches), fallback chain, OpenType
feature defaults, and variable-font instances.

The engine SHALL report: atlas occupancy and eviction rates, per-frame glyph rasterisation counts,
shaping cache hit rates, and fonts that trigger fallback frequently.

#### Scenario: Pre-rendered range
- **WHEN** a font is imported with the Latin range pre-rendered
- **THEN** those glyphs SHALL be present in the cooked atlas and require no runtime rasterisation

#### Scenario: Rasterisation hitch
- **WHEN** many new glyphs appear at once (a language switch)
- **THEN** the diagnostic SHALL report the rasterisation spike so the range can be pre-baked
