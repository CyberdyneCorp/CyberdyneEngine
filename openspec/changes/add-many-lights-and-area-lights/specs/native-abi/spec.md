## ADDED Requirements

### Requirement: Lights a game spawns through the ABI
ABI 1.11 SHALL append `light_spawn`, `light_update`, `light_destroy`, `light_alive` and
`light_status` over the engine's pool of lights a game spawns: a `CyLightDesc` (kind, pose, colour,
intensity, range, cone, size, lifetime, fade, two-sided flag, layer mask) and a `CyLight` handle. A
light is presentation: every call SHALL be refused with `CY_RESULT_PERMISSION_DENIED` during a
fixed update. A full pool SHALL refuse with `CY_RESULT_OUT_OF_RANGE`, a light that ended with
`CY_RESULT_NOT_FOUND`. The Swift overlay SHALL expose the entries as CyberdyneKit's `Light`.

#### Scenario: A muzzle flash from a frame update
- **WHEN** a game spawns a point light with a lifetime from a frame update
- **THEN** the light SHALL be in the next frame's lights, and after its lifetime `light_alive`
  SHALL answer false

#### Scenario: Refused in a fixed update
- **WHEN** a game calls `light_spawn` from a fixed update
- **THEN** the call SHALL return `CY_RESULT_PERMISSION_DENIED` and spawn nothing
