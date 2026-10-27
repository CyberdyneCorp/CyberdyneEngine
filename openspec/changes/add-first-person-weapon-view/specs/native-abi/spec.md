## ADDED Requirements

### Requirement: The first-person camera through the ABI
ABI 1.13 SHALL append `first_person_configure`, `first_person_input`, `first_person_kick`,
`first_person_reset`, `first_person_state`, `first_person_release`, `viewmodel_configure` and
`viewmodel_get` over the engine's first-person behaviours and weapon view, addressed by the
`CyCamera` of the camera's rig. The camera is presentation: every call SHALL be refused with
`CY_RESULT_PERMISSION_DENIED` during a fixed update. A camera without behaviours SHALL answer
`CY_RESULT_NOT_FOUND`. The Swift overlay SHALL expose the entries as CyberdyneKit's
`FirstPersonCamera` and `WeaponView`.

#### Scenario: Driven from a frame update
- **WHEN** a game feeds a first-person camera mouse counts from a frame update
- **THEN** the camera SHALL turn by the sensitivity once when the host advances it

#### Scenario: Refused in a fixed update
- **WHEN** a game kicks a first-person camera from a fixed update
- **THEN** the call SHALL return `CY_RESULT_PERMISSION_DENIED` and change nothing
