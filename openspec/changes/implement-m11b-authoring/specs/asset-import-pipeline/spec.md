## ADDED Requirements

### Requirement: What a cook could not do is part of its derivation key
A cooked artefact's derivation key SHALL include **the capabilities of the cooker that produced it**,
not only the source bytes and the cook parameters. A cache entry produced by a build that lacked a
step SHALL NOT satisfy a request from a build that has it.

The engine already records the fact and does not yet act on it. `select_format` names the format a
texture cook *would* produce and writes `encoded = false` beside an uncompressed payload, because no
BC7 or ASTC encoder is linked into any build in this tree. The dependency manifest states the
intended behaviour in so many words — a build that links an encoder *"produces a different derivation
key and re-cooks rather than serving uncompressed pixels from the cache"* — and nothing enforces it.

This matters beyond textures and beyond this milestone. The same shape holds for every step an
importer declares absent: mesh simplification without a simplifier, glTF without skins, a font
without a shaper, virtual geometry without its cooker. **In each case the failure is silent by
construction** — the cache hits, the build is fast, and the artefact is the one the weaker cooker
produced.

The check SHALL be able to fail: a cache populated by a cooker missing a step, then queried by one
that has it, SHALL miss.

#### Scenario: An encoder lands and the cache does not serve the old artefact
- **WHEN** a texture is cooked by a build with no BC7 encoder, and then requested by a build with one
- **THEN** the derivation key SHALL differ and the texture SHALL be re-cooked, rather than the
  uncompressed payload being served

#### Scenario: The capability set is part of the key, not a note beside it
- **WHEN** an importer declares a step absent
- **THEN** that declaration SHALL participate in the derivation key of everything the importer
  produces, and a test SHALL fail if the key is unchanged by it
