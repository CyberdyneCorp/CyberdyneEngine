## REMOVED Requirements

### Requirement: Input actions
**Reason**: Superseded by the action model in `input-and-actions`.

An action model needs input users, device ownership, a layered context stack, processors,
modifiers, and triggers. None of those belong in a platform abstraction whose responsibility is
normalising operating-system events, and specifying them here would have required the platform layer
to know about players, gameplay features, and accessibility policy.

Nothing is lost. The successor capability retains named actions bound to one or more inputs,
per-binding dead zone, inversion and scale, composed axes and vectors, the four state queries, a
consistent per-frame snapshot, configuration-loaded and runtime-rebindable binding sets — including
both original scenarios — and adds users, contexts, triggers, accessibility, and the gameplay
boundary.

## MODIFIED Requirements

### Requirement: Input
The input pipeline SHALL be: platform event → `InputServer` normalisation → **timestamped event
stream and per-frame snapshot** → `input-and-actions` → consumers.

This capability's responsibility ends at normalised, timestamped device events. Actions, users,
contexts, bindings, and triggers are defined in `input-and-actions`, and gameplay commands in
`gameplay-framework`.

Event types SHALL cover: key press and release with scancode, keycode, modifiers and repeat flag;
text input as UTF-8 (separate from key events); mouse motion (absolute and relative), buttons,
and wheel; touch begin, move, and end with per-touch ids; pen input with pressure and tilt;
gamepad buttons, axes, and connection changes; and gestures where the platform provides them.

Every event SHALL carry a **high-resolution timestamp** taken as close to the platform's observation
as available, and a stable device identifier — because fixed-tick resolution, replay fidelity, and
latency measurement all depend on when an event happened rather than when it was processed.

Gamepad support SHALL use SDL3 as the backend, giving controller database mappings, rumble, and
hot-plug across platforms.

#### Scenario: Text input is separate from keys
- **WHEN** the user types a character with a compose key or IME
- **THEN** a text-input event with the composed UTF-8 SHALL be delivered, distinct from the raw
  key events

#### Scenario: Relative mouse for camera control
- **WHEN** the mouse is captured
- **THEN** relative motion SHALL be delivered without being clamped by screen edges

#### Scenario: Controller hot-plug
- **WHEN** a gamepad is connected mid-session
- **THEN** a connection event SHALL be delivered with a stable device id and its mapping resolved
  from the controller database

#### Scenario: Events are timestamped at observation
- **WHEN** an event is delivered late because a frame was long
- **THEN** its timestamp SHALL reflect when the platform observed it, so tick resolution remains
  correct

### Requirement: Fixed-step input handling
Input consumed by fixed-step simulation SHALL be accumulated between ticks so that no press is
lost when a frame contains zero or multiple ticks.

Resolution of accumulated events into per-tick action state and command frames is defined in
`input-and-actions`; this requirement states the guarantee the platform layer must make possible by
delivering timestamped events without coalescing away transitions.

#### Scenario: Button pressed and released within one frame
- **WHEN** a button is pressed and released between two simulation ticks
- **THEN** the tick SHALL still observe a just-pressed and just-released event, rather than
  missing the input entirely

#### Scenario: Transitions are not coalesced away
- **WHEN** several transitions of one control occur within a frame
- **THEN** each SHALL be delivered with its timestamp, rather than collapsed into a final state
