## ADDED Requirements

### Requirement: A second implementation is what proves the abstraction
An interface implemented once is a description of its single implementation. `Platform` and
`DisplayServer` have had exactly one desktop implementation since M4 — SDL3, in
`platform/desktop-sdl3/` — so the claim that the porting surface carries no SDL assumption has never
been tested.

The engine SHALL therefore ship a **native** `Platform` and `DisplayServer` for at least one desktop
platform, implemented against the operating system directly and replacing SDL3 on that platform.

Adding it SHALL require **no change in `src/core/`, `src/ecs/`, `src/servers/` or `src/scene/`**, and
this SHALL be checked against the changeset that adds the backend rather than asserted afterwards. A
change in any of those four directories is a defect in the abstraction and SHALL be recorded as a
finding naming the interface that leaked, whether or not the port is completed around it.

The SDL3 backend SHALL remain, built and tested, on the platforms that do not have a native one. The
second implementation is the proof; removing the first would be a larger change making a smaller
claim.

No type belonging to a windowing library or an operating system SHALL appear above `platform/`,
which the layer check already enforces for the backend that exists.

#### Scenario: The port changes nothing above the platform layer
- **WHEN** a native platform backend is added
- **THEN** the changeset SHALL contain no modification under `src/core/`, `src/ecs/`, `src/servers/`
  or `src/scene/`, and a check SHALL fail naming any file that does

#### Scenario: The engine runs on the native backend
- **WHEN** the engine runs with the native backend selected instead of SDL3
- **THEN** the empty sample SHALL start, tick and exit cleanly, and the golden scenes SHALL render
  with the same references as on the SDL3 backend

#### Scenario: A leaked assumption is a finding, not a workaround
- **WHEN** the port cannot proceed without changing an engine-side interface
- **THEN** the required change SHALL be recorded as a defect in the abstraction, naming the
  interface, rather than absorbed into the backend as a special case

### Requirement: The porting surface is proved against a platform that shares no desktop assumption
`Supported platforms` requires the abstraction not to preclude mobile targets — no assumption of a
mouse, of a resizable window, of a filesystem writable outside the user mount, or that the process
controls its own main loop. Those are properties of code that has never been compiled against a
platform lacking them.

The engine SHALL therefore provide a **stub platform backend** that asserts all four absences: it
exposes no pointing device, a window whose size it does not control, a read-only filesystem outside
the user mount, and **no main loop of its own** — driving frames through the runtime's `tick()`
entry point.

The stub SHALL build and run a frame in continuous integration, so that a regression in any of the
four is a build or test failure rather than a discovery made during a port.

The stub SHALL NOT be a second headless backend: the headless implementation the specification
already requires exists to run without a window system, while this one exists to run without desktop
assumptions, and a platform that satisfies one may fail the other.

#### Scenario: No mouse, and nothing requires one
- **WHEN** the engine runs on the stub platform
- **THEN** no subsystem SHALL require a pointing device to start or to advance a frame

#### Scenario: The platform drives the frames
- **WHEN** the stub platform runs
- **THEN** frames SHALL advance through the runtime's `tick()` entry point called by the platform,
  and no engine-side loop SHALL own the process

#### Scenario: A regression in a mobile prerequisite is caught before the port
- **WHEN** a change introduces a dependency on a resizable window or on writing outside the user
  mount
- **THEN** the stub platform's build or its frame test SHALL fail naming it
