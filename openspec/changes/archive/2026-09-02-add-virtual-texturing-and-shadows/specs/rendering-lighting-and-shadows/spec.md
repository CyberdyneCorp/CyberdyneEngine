## MODIFIED Requirements

### Requirement: Shadow atlas allocation
The **conventional** shadow path — used by profiles that do not enable virtual shadows, and as the
fallback where capabilities are absent — SHALL allocate point and spot light shadows from a **shadow
atlas** partitioned into tiles of several sizes.

This path SHALL remain fully supported. It is the correct answer for constrained hardware and the
required fallback; it is not deprecated by `virtual-shadows`.

Tile size SHALL be selected per light per frame from its projected screen coverage, with
hysteresis so a light does not oscillate between sizes.

Allocation SHALL prefer a free tile, then the least recently used tile whose owner has not been
allocated within a minimum retention period, so competing lights do not thrash.

Shadow maps SHALL be **cached across frames** for lights and casters that have not changed,
tracked by a version per light and per caster set.

Where a light's shadow mode is `Virtual`, its shadowing SHALL be governed by `virtual-shadows`
instead, and it SHALL consume no atlas tile.

#### Scenario: Nearby light gets a larger tile
- **WHEN** a light covers a large fraction of the screen
- **THEN** it SHALL receive a larger atlas tile, subject to availability

#### Scenario: Static shadow is reused
- **WHEN** a static light with only static casters has already been rendered
- **THEN** its shadow map SHALL be reused without re-rendering

#### Scenario: Atlas is oversubscribed
- **WHEN** more lights request shadows than the atlas can hold
- **THEN** lights SHALL be prioritised by screen coverage and importance, and those that miss out
  SHALL render unshadowed with the shortfall reported

#### Scenario: Modes coexist
- **WHEN** a scene mixes conventional and virtual shadowed lights
- **THEN** each SHALL use its declared path, and the atlas SHALL be sized only for the conventional
  ones
