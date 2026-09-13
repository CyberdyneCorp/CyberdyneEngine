## MODIFIED Requirements

### Requirement: Regional environmental state
Foliage SHALL carry **regional state** derived from environment fields — normal, wet, dry, burning,
burned, snow-covered — modulating appearance and behaviour, with per-instance exceptions where
gameplay has affected an individual plant.

Regional state SHALL be read from fields rather than stored per instance, so that a burned forest
costs a field region rather than a million instance updates.

State changes SHALL be visible to materials, VFX, audio, and gameplay through the same fields.

**Foliage declares the current state it realises and consumes the potential it recovers toward.**
`vegetation` — how much plant life a position actually carries — is foliage's, declared and produced
by it. `vegetation-potential` — what a position would carry given time, derived from the slow inputs
`weather-and-wind`'s *Ecosystem state* names — belongs to the ecosystem, and foliage SHALL sample it
rather than declare it. The two are separate fields because `environment-fields`' *Potential and
current state* requires them to be, and declaring both from one module is how they came to be
declared twice with two encodings — UNorm8/Static from foliage and UNorm16/SlowlyVarying from
weather's ecosystem half, which `FieldRegistry::declare()` refuses in either order, so a project
registering both producers fails at startup.

Where no ecosystem producer is registered, foliage SHALL sample the potential's declared default and
SHALL NOT substitute a declaration of its own: a missing producer is a field that reads its default,
which the substrate already defines.

#### Scenario: A forest burns
- **WHEN** fire spreads through a region
- **THEN** the burn-state field SHALL change and the foliage in that region SHALL render burned,
  without per-instance writes

#### Scenario: One tree is different
- **WHEN** a single plant is destroyed within an unburned region
- **THEN** it SHALL be recorded as an instance exception rather than changing the region's state

#### Scenario: Both producers register in one project
- **WHEN** a project registers the foliage producer and the ecosystem producer, in either order
- **THEN** both SHALL register, because exactly one of them declares `vegetation-potential`

#### Scenario: Recovery reads the ecosystem's potential
- **WHEN** foliage recovers realised vegetation toward what the region could support
- **THEN** it SHALL sample `vegetation-potential` from the substrate rather than deriving a potential
  of its own
