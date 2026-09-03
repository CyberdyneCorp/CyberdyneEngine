## MODIFIED Requirements

### Requirement: Procedural placement
Foliage SHALL be placeable by **rules** evaluated against environment fields and terrain: biome,
slope, altitude, moisture, temperature, soil, sun exposure, water distance, noise, roads, and
exclusion zones.

Placement rules SHALL be **procedural programs** (see `procedural-content-generation`): authored as
graphs, compiled, executed by region, cached by derivation key, and invalidated through that
capability's dependency and radius declarations. Foliage SHALL NOT maintain a separate procedural
execution or invalidation model, and generation SHALL produce **foliage populations** through the
output adapter rather than entities.

Rules SHALL produce species selection, density, scale, orientation, variation, and age.

Rule evaluation SHALL be **deterministic**, derived from the world seed, a region identifier, and the
rule graph version, so that a region regenerates identically rather than being serialised instance by
instance.

Where a region's macro ecosystem state has evolved while unloaded, materialisation SHALL be
consistent with that state rather than regenerating as though nothing had happened.

Manual painting SHALL compose with rules: an author MAY place, move, or remove instances, and MAY
suppress rules within a region.

#### Scenario: A forest is generated, not placed
- **WHEN** a forest covers ten square kilometres
- **THEN** its instances SHALL be derived from rules and seed rather than stored individually

#### Scenario: Regeneration is identical
- **WHEN** a region is regenerated on another machine or in another session
- **THEN** it SHALL produce identical instances for the same seed and rule version

#### Scenario: A grown forest appears grown
- **WHEN** a region whose vegetation state increased while unloaded is materialised
- **THEN** the generated foliage SHALL reflect that state
