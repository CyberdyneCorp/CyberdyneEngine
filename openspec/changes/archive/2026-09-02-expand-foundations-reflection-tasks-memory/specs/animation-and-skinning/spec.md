## MODIFIED Requirements

### Requirement: Animation clips
An **animation clip** SHALL be a set of tracks, each targeting a binding with keyframes.

Track kinds: `Translation`, `Rotation`, `Scale`, `BlendShape`, `Property` (any reflected field),
`Event` (named triggers), and `Reference` (asset swaps).

Property track bindings SHALL be stored as **stable identities** — a target selector plus a
`TypeId` and `FieldId` (see `core-type-system`) — not as a name path such as
`Root.LeftArm.Weapon.Damage`. Renaming an entity, a component, or a field SHALL NOT break a clip.

Bindings SHALL be **resolved once** to a direct accessor when a clip is prepared for an instance;
sampling SHALL NOT perform a reflection lookup per frame.

A binding whose target no longer exists SHALL be reported as an unresolved binding, naming the clip
and the target, and SHALL be skipped rather than failing the clip.

Interpolation modes: `Step`, `Linear`, `Cubic` (with in/out tangents), and `Spherical` for
rotations.

Clips SHALL declare: duration, a loop mode (`None`, `Loop`, `PingPong`), and a sample rate hint.

#### Scenario: Rotation interpolation
- **WHEN** a rotation track is sampled between keys
- **THEN** quaternions SHALL be interpolated along the shortest arc, with neighbouring keys
  hemisphere-aligned at import so no long-way rotation occurs

#### Scenario: Property track
- **WHEN** a clip animates a material parameter
- **THEN** the track SHALL target the field by identity, be resolved once to a direct accessor, and
  be written without a per-frame reflection lookup

#### Scenario: Renaming does not break a clip
- **WHEN** an animated field is renamed
- **THEN** the clip SHALL continue to animate it, because the binding keys on identity

#### Scenario: Unresolved binding is reported
- **WHEN** an animated field has been removed from its type
- **THEN** the binding SHALL be reported as unresolved and skipped, and the clip's other tracks
  SHALL still play
