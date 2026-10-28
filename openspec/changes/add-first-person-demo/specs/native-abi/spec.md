## ADDED Requirements

### Requirement: Effects and decals a game spawns through the ABI
ABI 1.15 SHALL append `effect_find` and `effect_play` over the effects a host registered by name in
its VFX simulation world, and `decal_find_material`, `decal_spawn` and `decal_status` over the
decal materials a host registered and its decal budget. Playing an effect and spawning a decal are
presentation: both SHALL be refused with `CY_RESULT_PERMISSION_DENIED` during a fixed update, while
resolving a name SHALL be allowed in every phase. An effect SHALL play fire-and-forget and release
itself when its last particle dies. A decal the full budget evicts on arrival SHALL answer
`CY_RESULT_OK` with `CY_DECAL_NULL` and be counted. A host that binds neither backend SHALL answer
`CY_RESULT_UNAVAILABLE`. The Swift overlay SHALL expose the entries as CyberdyneKit's `Effect`,
`DecalMaterial` and `Decal`.

#### Scenario: A shot's sparks and mark from a frame update
- **WHEN** a game plays a registered effect and spawns a decal from a frame update
- **THEN** the effect SHALL be playing in the host's simulation world at the position given, and the decal SHALL be in the budget with its projection axis on the pose's +Z

#### Scenario: Refused in a fixed update
- **WHEN** a game plays an effect or spawns a decal from a fixed update
- **THEN** the call SHALL return `CY_RESULT_PERMISSION_DENIED` and play or spawn nothing
