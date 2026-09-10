# `src/text/` — layer 2

**The Unicode layer above CyberText**: the bidirectional algorithm, line breaking, Arabic joining,
the shaping cache and localisation.

**Governed by**: `text-and-fonts`. M8.b task 9.4.

## What tier this actually reaches, stated first

`text-and-fonts` names **HarfBuzz, ICU and FreeType** in its normative text — "shaped through
HarfBuzz", "per the Unicode Bidirectional Algorithm **via ICU**", "via ICU, with dictionary-based
breaking", "plural rules and number and date formatting come from ICU". **None of the three is in
`deps/manifest.toml`**, and integrating ICU is a dependency task of its own size, not a corner of a
text module.

So this module is the honest half: **the algorithms, implemented here, over property tables whose
coverage is declared and queryable**. What that buys and what it does not:

| Requirement | Here | Not here |
|---|---|---|
| Bidirectional layout | P2/P3, X1–X8, W1–W7, N1/N2, I1/I2, L1, L2; overrides; isolates; structured-text hints | X10's isolating run sequences (approximated by level runs, and **reported**); N0 paired brackets; mirrored glyphs |
| Line breaking | UAX #14's rules over the covered classes; mandatory breaks; the dictionary deferral and a longest-match `WordList` | a shipped Thai/Khmer/Lao dictionary; the Korean and emoji classes |
| Shaping | Arabic joining: joining types, the four contextual forms, transparent marks, the mandatory lam-alef ligature | GSUB and GPOS — ligatures beyond lam-alef, mark positioning from the font's tables, Indic reordering, kerning, vertical layout |
| Justification | inter-word, inter-character, kashida, and the configurable priority among them | — |
| Overflow | clip, word wrap, character wrap, ellipsis at start/middle/end, shrink-to-fit | — |
| Localisation | locale parsing, direction, CLDR plural categories for the named languages, message formatting with a plural selector, a string table with a fallback chain, pseudo-localisation | number, currency and date formatting — those are locale DATA, and a hand-written table of them would be wrong for most of the world |

**Every gap above is queryable in code**, not only in this table: `ShapingCapabilities` reports what
shaping can do, `coverage_of()` says whether a codepoint's properties are data or a documented
default, `BidiResult::approximated` says when an isolate made the answer approximate,
`BreakReport::used_dictionary` says whether a Thai line was broken by a dictionary or by the
fallback, and `plural_rules_known()` says whether a language's plural rules are implemented. That is
`text-and-fonts`' own rule — "code needs to know whether bidirectional layout is available THEN it
SHALL query the capability rather than testing which backend is active" — applied one level up.

**`text-and-fonts` therefore does not reach Complete in this milestone**, and this README is where
that is written down rather than in a status file somebody has to reconcile later. What it reaches is
Working: an engine that can lay out Hebrew, Arabic and Thai correctly enough to ship an interface,
and that says exactly where it stops.

## Why this is a peer of `src/servers/text/` and not part of it

`src/servers/text/` (layer 2, M5) owns the interface, the glyph atlas, the minimal image-grid backend
and the layout objects, and its `TextCapabilities` declares `complex_shaping`, `bidirectional` and
`dictionary_line_breaking` all false. This module is those three algorithms — and it depends on
**`cy::core` alone**, because they are functions over text and property tables. A module that needed
a font server to answer "which way does this run go" could not be used by a cook, a tool or a test,
and `src/ui/` is the consumer that wires the two together.

## Testing

`unit.text_unicode` — 39 cases over the bidirectional algorithm (paragraph level, mixed runs, Arabic
numbers, overrides, isolates, L1's trailing whitespace, the file-path hint), line breaking (spaces,
brackets, hyphens, mandatory breaks, Thai with and without a dictionary), justification and overflow,
grapheme clusters, Arabic joining (the four forms, transparent marks, right-joining letters, the
ligature), itemisation, the shaping cache and its eviction, and localisation.
