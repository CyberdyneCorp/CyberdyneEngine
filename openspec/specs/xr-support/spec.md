# xr-support Specification

## Purpose

Defines XR (VR, AR, and mixed reality) support. XR is a **planned** capability, not part of the
initial milestone. This specification exists to record the requirements the rest of the engine
must not preclude, so XR can be added without restructuring the renderer, input, or scene systems.

## Requirements

### Requirement: XR is deferred but not precluded
XR SHALL NOT be implemented in the initial engine milestone. The architecture SHALL, however,
satisfy the structural prerequisites listed in this specification, and any change that would
violate them SHALL be treated as a breaking architectural decision.

#### Scenario: A change would preclude XR
- **WHEN** a proposal assumes a single view per camera, or a single fixed frame rate, or that the
  application owns the main loop
- **THEN** it SHALL be flagged against this specification and either revised or accepted with an
  explicit decision to drop XR support

### Requirement: Multi-view rendering prerequisite
The renderer SHALL support rendering **multiple views per camera** in a single pass, with
per-view projection and transform matrices indexed in shaders, and layered render targets.

This is required for stereo rendering and is already specified in
`rendering-forward-clustered`; XR is its primary consumer.

#### Scenario: Two views, one submission
- **WHEN** a camera requests two views
- **THEN** geometry SHALL be submitted once and amplified across both layers, not drawn twice

### Requirement: Runtime-driven frame timing prerequisite
The runtime SHALL support an execution model in which **an external runtime drives frames**: the
engine exposes a `tick()` entry point and does not assume ownership of a `while (running)` loop.

The frame loop SHALL tolerate a frame cadence and predicted display time supplied externally,
rather than deriving all timing from its own clock.

#### Scenario: Runtime calls the engine
- **WHEN** an XR runtime signals that a frame should begin, with a predicted display time
- **THEN** the engine SHALL run one frame against that time, rather than free-running

### Requirement: Late-latching prerequisite
The renderer SHALL allow the **view transform to be updated after culling and command recording
have begun**, so the most recent head pose can be applied as late as possible before submission.

View matrices SHALL therefore be sourced from a buffer updated late in the frame, not baked into
recorded commands.

#### Scenario: Pose updated late
- **WHEN** a more recent head pose becomes available after culling
- **THEN** the view matrices SHALL be updated before submission without re-recording draw commands

### Requirement: Planned XR interface
When implemented, XR SHALL be exposed through an `XRSystem` interface providing: session lifecycle
(create, begin, end, with state transitions), per-view projection and transform queries, swapchain
acquisition and release, input via an action-based abstraction, reference spaces (view, local,
stage, and unbounded), and environment blend modes (opaque, additive, alpha-blend).

**OpenXR** SHALL be the initial and primary backend.

#### Scenario: Action-based input
- **WHEN** XR input is implemented
- **THEN** it SHALL use an action-based model with per-device binding profiles, not per-device
  polling, so new hardware works without game changes

#### Scenario: Session lifecycle
- **WHEN** the runtime moves the session between states (idle, ready, synchronised, visible,
  focused, stopping)
- **THEN** the engine SHALL respond appropriately, including pausing simulation when not visible

### Requirement: Planned XR tracking model
XR tracking SHALL be exposed as **trackers** with named poses: head, left and right hands,
controllers, and optionally eyes, face, and body joints.

Poses SHALL carry position, orientation, linear and angular velocity, and a tracking confidence,
so games can respond to tracking loss rather than receiving stale or zero poses.

A **tracking origin** entity SHALL define the mapping between tracking space and world space,
including a world scale factor.

#### Scenario: Tracking loss
- **WHEN** a controller loses tracking
- **THEN** its pose confidence SHALL drop and the game SHALL be able to fade or freeze the
  associated visual, rather than seeing it snap to the origin

#### Scenario: Recentring
- **WHEN** the player recentres
- **THEN** the tracking origin SHALL be adjusted so the current head pose maps to the intended
  world position

### Requirement: Planned XR rendering features
When implemented, XR rendering SHALL support: **foveated rendering** via variable-rate shading
driven by a runtime-supplied density map, **composition layers** for UI rendered at native
runtime resolution rather than through the scene, and **passthrough** compositing for mixed
reality where the runtime provides it.

#### Scenario: UI on a composition layer
- **WHEN** a UI panel is submitted as a composition layer
- **THEN** it SHALL be composited by the runtime at full resolution without reprojection blur

#### Scenario: Foveation
- **WHEN** the runtime supplies a foveation density map
- **THEN** peripheral regions SHALL be shaded at a reduced rate through the existing VRS path

### Requirement: Comfort and safety requirements
When implemented, XR SHALL provide: a documented frame-rate budget with the consequences of
missing it, guidance and defaults for comfortable locomotion (vignetting during movement, snap
turning), a guardian or boundary awareness API, and warnings when a frame consistently misses the
display cadence.

#### Scenario: Frame budget missed
- **WHEN** frames consistently exceed the display cadence
- **THEN** the engine SHALL report it prominently in development builds, since dropped frames in
  XR cause discomfort rather than mere visual stutter
