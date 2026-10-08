# `src/text/` — layer 2

**The Unicode layer above CyberText**: the bidirectional algorithm, line breaking, Arabic joining,
the shaping cache and localisation.

**Governed by**: `text-and-fonts`. M8.b task 9.4.

## What this module is, now that the libraries are in

`text-and-fonts` names **HarfBuzz, ICU and FreeType** in its normative text — "shaped through
HarfBuzz", "per the Unicode Bidirectional Algorithm **via ICU**", "via ICU, with dictionary-based
breaking", "plural rules and number and date formatting come from ICU". Since issue #86 all three are
in `deps/manifest.toml`, behind `cy::text::TextBackend` in src/backends/text-complete/ (with msdfgen),
gated by `CY_TEXT` and `CY_TEXT_ICU`.

This module stays, for three reasons. It is **the minimal path**: a build with `CY_TEXT` off, or a
complete backend with `CY_TEXT_ICU` off, resolves bidirectional levels and breaks lines with these
algorithms. It is **a test oracle**: `integration.text_complete` compares HarfBuzz's Arabic forms
with `join_arabic` letter by letter and ICU's levels with `resolve_levels` run by run, and shows the
one place they differ (N0, below). And it holds **what ICU's data would hold** and this build does not
ship — line breaking with a `WordList` dictionary, plural rules, the string table — because the ICU
subset compiled in is data-free (src/backends/text-complete/README.md says why).

The algorithms are implemented here over property tables whose coverage is declared and queryable.
What that buys and what it does not, with what the complete backend adds:

| Requirement | Here | Not here |
|---|---|---|
| Bidirectional layout | P2/P3, X1–X8, W1–W7, N1/N2, I1/I2, L1, L2; overrides; isolates; structured-text hints | X10's isolating run sequences (approximated by level runs, and **reported**); N0 paired brackets; mirrored glyphs — all three are ICU's in the complete backend, and HarfBuzz mirrors |
| Line breaking | UAX #14's rules over the covered classes; mandatory breaks; the dictionary deferral and a longest-match `WordList` | a shipped Thai/Khmer/Lao dictionary; the Korean and emoji classes |
| Shaping | Arabic joining: joining types, the four contextual forms, transparent marks, the mandatory lam-alef ligature | GSUB and GPOS — ligatures beyond lam-alef, mark positioning from the font's tables, Indic reordering, kerning — are HarfBuzz's in the complete backend; vertical layout is not built anywhere |
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

What `text-and-fonts` still lacks after issue #86 is written down here rather than in a status file
somebody has to reconcile later: dictionary breaking from shipped dictionaries, number, currency and
date formatting, vertical layout, and split carets at direction boundaries. Each is either declared
false in `TextCapabilities` or reported by the function that cannot do it.

## Why this is a peer of `src/servers/text/` and not part of it

`src/servers/text/` (layer 2) owns the interface, the glyph atlases, the minimal image-grid backend
and the layout objects, and since issue #86 it LINKS this module: its bidirectional lines fall back
to `resolve_levels` and `reorder_visual` when a backend has no algorithm of its own, and its face
runs keep a combining mark with its base through `grapheme_break_of`. The two once each defined
`decode_utf8`; the text server's copy is gone, because one binary linking both had two definitions.

This module is the algorithms — and it depends on **`cy::core` alone**, because they are functions
over text and property tables. A module that needed a font server to answer "which way does this run
go" could not be used by a cook, a tool or a test.

## Testing

`unit.text_unicode` — 39 cases over the bidirectional algorithm (paragraph level, mixed runs, Arabic
numbers, overrides, isolates, L1's trailing whitespace, the file-path hint), line breaking (spaces,
brackets, hyphens, mandatory breaks, Thai with and without a dictionary), justification and overflow,
grapheme clusters, Arabic joining (the four forms, transparent marks, right-joining letters, the
ligature), itemisation, the shaping cache and its eviction, and localisation.
