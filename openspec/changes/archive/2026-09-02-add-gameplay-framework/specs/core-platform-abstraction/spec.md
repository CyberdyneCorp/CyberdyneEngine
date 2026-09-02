## MODIFIED Requirements

### Requirement: Input actions
`InputServer` SHALL provide an action mapping layer: named actions bound to one or more inputs,
with per-binding deadzone, inversion, and scale; and named axes and 2D vectors composed from
bindings.

Action state SHALL be queryable as pressed, just-pressed, just-released, and analogue value,
sampled against a per-frame snapshot so all consumers in a frame observe the same state.

Binding sets SHALL be loadable from configuration and rebindable at runtime, and SHALL be organised
into **contexts** that can be activated and deactivated, so that a menu, a vehicle, and on-foot
movement have independent bindings without conflicting.

**Input actions are the boundary with gameplay.** Actions produce **gameplay commands** (see
`gameplay-framework`); raw input events SHALL NOT reach gameplay systems. This is what allows a
human, an AI, a network peer, and a replay to drive the same simulation path, and what makes
rebinding incapable of changing gameplay behaviour.

Input actions MAY be consumed directly by the interface and by editor tooling, which are not
gameplay.

#### Scenario: Same action, keyboard and gamepad
- **WHEN** "move" is bound to WASD and to the left stick
- **THEN** `get_vector("move")` SHALL return a normalised vector from whichever device is active,
  with the stick's deadzone applied

#### Scenario: Consistent state within a frame
- **WHEN** two systems query the same action in one frame
- **THEN** both SHALL observe identical state regardless of when the platform event arrived

#### Scenario: Gameplay never sees a key
- **WHEN** a player presses a key bound to an attack
- **THEN** gameplay SHALL receive an attack command, and no gameplay system SHALL observe the key
  event

#### Scenario: Rebinding cannot break gameplay
- **WHEN** a player rebinds every control
- **THEN** gameplay logic SHALL be unaffected, because it consumes commands rather than inputs
