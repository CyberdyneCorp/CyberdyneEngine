# swift-scripting Spec Delta

## ADDED Requirements

### Requirement: Engine-scheduled Swift systems
A `@System` function a game module lists SHALL be registered with the engine with the access its
`Query` type declares and the stage its attribute names, and SHALL be run by the engine's scheduler
in that stage over the engine's chunk storage, with the chunks of every archetype holding every
component it reads or writes and none it excludes. A query naming a resource or a component the
world has not registered SHALL be refused at registration.

#### Scenario: A Swift system the scheduler runs
- **WHEN** a game module lists a `@System(stage: .simulation)` writing a component and the host runs
  its fixed stages
- **THEN** the system SHALL run once per fixed step, and SHALL not run when the host leaves script
  systems out of its schedule

### Requirement: Tree callbacks and node references are driven
A Swift behaviour attached to a scene node SHALL receive `onEnterTree`, `onReady`, `onEnable`,
`onDisable` and `onExitTree` from the engine, each only if the class overrides it, and every
`@Node(path)` property SHALL be resolved against the behaviour's node before `onReady` runs, and be
nil when the path does not resolve.

#### Scenario: Node reference resolved before onReady
- **WHEN** a behaviour on `/Level/Commander` declares `@Node("../Barracks")` and the level has a
  `Barracks` node
- **THEN** the property SHALL hold that node when `onReady` runs

### Requirement: Bodies and characters from Swift
`CyberdyneKit` SHALL provide `RigidBody` (apply a force, an impulse at a point or the centre of
mass, a torque; set and read velocity) and `CharacterController` (create on an entity, move one fixed
step with an optional jump, read the state, destroy) over the ABI's entries, throwing
`CyberdyneError` for every refusal.

#### Scenario: A character walked from a fixed update
- **WHEN** a behaviour moves its character at 2 m/s along +X for 120 fixed steps
- **THEN** the character SHALL be about 4 m further along X and report that it is grounded
