## ADDED Requirements

### Requirement: Aim and foot placement through the ABI
ABI 1.13 SHALL append `animation_aim_at`, which takes a world point into the character's model
space through the entity's placement and writes it to the program's aim target parameters, and
`animation_foot_placement`, which reports what a foot placement stood on in a `CyFootPlacement`.
`animation_aim_at` SHALL be simulation (`[N F]`), SHALL refuse a non-finite point with
`CY_RESULT_INVALID_ARGUMENT` and an unknown target with `CY_RESULT_NOT_FOUND` having written nothing.
`animation_foot_placement` SHALL be presentation (`[N U]`). A backend written against 1.12 SHALL
answer `CY_RESULT_UNAVAILABLE`. CyberdyneKit SHALL expose them as `Animator.aim(_:at:)` and
`Animator.footPlacement`, beside `Animator.locomotion(velocity:facing:)` and `setLocomotion`, which
set a locomotion blend space's speed and direction from a behaviour.

#### Scenario: An aim at a world point
- **WHEN** a game aims a character standing two metres along x and turned a quarter at a world
  point
- **THEN** the program's target parameters SHALL hold that point in the character's model space

#### Scenario: Speed and direction from a behaviour
- **WHEN** a behaviour moving at 3 m/s straight ahead sets its locomotion each fixed update
- **THEN** the speed parameter SHALL read 3 and the direction 0
