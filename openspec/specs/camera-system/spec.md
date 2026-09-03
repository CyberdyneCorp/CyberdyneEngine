# camera-system Specification

## Purpose

Defines **CyberCamera**: composable rigs that produce an evaluated camera, from which a render view,
an audio listener and a streaming source are derived.

Four concepts are kept apart — definition, rig, evaluated camera, render view — because one
evaluated camera may produce several views, and because renderer concerns like temporal jitter and
backend clip conventions belong to the view rather than to anything gameplay touches.

A camera is not a scene object and has no base class to inherit from. A rig is a **graph compiled to
a program**, the fifth time this engine uses that pattern after materials, VFX, animation and
behaviour graphs. Gameplay never writes a camera transform: it emits intent and impulses, every
contribution is a weighted layer, and the debugger can therefore say what moved the camera.

Three separations do most of the work. The **target is not the controlled entity** — a strategy
camera frames an army, a spectator watches someone else. **Collision and occlusion are different
questions** — glass occludes without colliding. And **aim has three meanings**: where the camera
looks, where the player aims (which travels in commands and is what the server validates), and where
the weapon points. The camera is explicitly not authoritative gameplay state.

The camera is also the producer nine other capabilities were waiting for: render views, streaming
sources with velocity and prediction, cut events that invalidate temporal history and shadow caches,
view importance for residency budgets, the listener anchor, and the screen rays selection needs.

## Requirements

### Requirement: Four separated concepts
The camera system SHALL distinguish, and SHALL NOT conflate:

| Concept | Is |
|---|---|
| **Camera definition** | An authored asset describing a rig composition and its parameters |
| **Camera rig** | A runtime instance of a definition, holding state |
| **Evaluated camera** | The pose, lens, and derived data resolved for a frame |
| **Render view** | The renderer's description of a view to produce |

One evaluated camera MAY produce **several render views** — a main view, a weapon view, a portal, a
minimap — so "one camera is one framebuffer" SHALL NOT be assumed.

Renderer concerns — temporal jitter, backend clip conventions, reversed-Z, resource binding — belong
to the render view and SHALL NOT appear in gameplay-facing camera interfaces.

#### Scenario: Renderer details do not leak
- **WHEN** gameplay reads a camera
- **THEN** it SHALL obtain pose, lens, and projection semantics, and no backend or temporal concept

#### Scenario: One camera, several views
- **WHEN** a first-person camera renders the world and a separate weapon view
- **THEN** both SHALL be render views derived from one evaluated camera

### Requirement: A camera is not a scene object
A camera SHALL be addressable as a lightweight handle and data, and SHALL NOT require an ECS entity,
a node, or a base class to exist.

There SHALL NOT be a camera base class intended for subclassing to produce camera behaviours.

Entities MAY reference camera rigs, and the editor MAY present cameras as objects; neither SHALL
imply one entity per evaluated camera.

#### Scenario: A camera without an entity
- **WHEN** a debug camera, a reflection capture, and an editor viewport are active
- **THEN** each SHALL be a camera rig with no requirement for a gameplay entity

#### Scenario: No camera hierarchy
- **WHEN** a third-person camera behaviour is created
- **THEN** it SHALL be a rig composition, not a subclass of a camera type

### Requirement: Rig graphs compile to programs
A camera definition SHALL be a **graph of rig nodes** — target, position, orientation, constraint,
collision, lens, noise, blend, and output — composed rather than inherited.

Graphs SHALL be **compiled** at cook time into a compact rig program, following the same pattern as
material, VFX, animation, and behaviour graphs.

Runtime evaluation SHALL execute the compiled program. Per-node heap-allocated virtual objects SHALL
NOT be used in the evaluation path.

Projects and plugins SHALL be able to register custom node kinds through the extension points in
`project-and-plugins`.

#### Scenario: Composition, not inheritance
- **WHEN** a third-person camera is authored
- **THEN** it SHALL compose follow, orbit, shoulder offset, collision, aim, and noise nodes

#### Scenario: Evaluation is compact
- **WHEN** a rig is evaluated
- **THEN** it SHALL execute a compiled program with no per-node allocation or virtual dispatch

### Requirement: Camera targets
A camera's **target** SHALL be a separate binding with its own policy, and MAY be: an entity, an
entity group, a world position, bounds, a spline, or a project-supplied provider.

**The target SHALL NOT be implicitly the controlled entity.** Following what the player controls is
one policy among several.

Targets SHALL be referenced by stable handles that survive streaming and structural change (see
`gameplay-framework`), and rig nodes SHALL NOT retain raw pointers to component data across frames.

Multiple targets SHALL be supported with per-target weight, minimum screen size, and priority.

#### Scenario: The camera watches something else
- **WHEN** a player drives a character while the camera follows a drone
- **THEN** the target binding SHALL be independent of control

#### Scenario: An army is the target
- **WHEN** a strategy camera frames a selection
- **THEN** its target SHALL be the group's bounds, not a single entity

### Requirement: Camera intent
Gameplay and players SHALL influence cameras through **intent** — look delta, zoom, aim, recentre,
and project-defined parameters — and through impulses, not by writing camera transforms.

Gameplay systems SHALL NOT set a camera's position or orientation directly. Direct pose control
SHALL be available only to low-level debug and custom node code.

Intent SHALL be produced from input actions, so that the same camera responds to a mouse, a stick,
gyroscopic input, or a scripted source without special cases.

#### Scenario: Nothing writes the transform
- **WHEN** several systems influence the camera
- **THEN** each SHALL contribute intent, impulses, or rig parameters, and the evaluated pose SHALL
  be produced by the rig

#### Scenario: Look works from any source
- **WHEN** look is driven by a mouse, a stick, or gyroscopic input
- **THEN** the camera SHALL receive intent, and the differences SHALL have been handled by input
  processing

### Requirement: Camera stack and blending
Each local player SHALL own a **camera stack** of contributions with priorities and weights: a base
gameplay camera, additive modifiers, effects, volume influence, cinematic override, and debug
override.

Transitions between rigs SHALL be **blends** with a declared duration, curve, and per-channel policy
for position, rotation, and lens.

Rotation blending SHALL interpolate correctly for orientations; lens blending SHALL respect the lens
model in use, so that a physical lens does not interpolate as though it were a field-of-view value.

The stack SHALL be inspectable: each contribution's weight and its effect SHALL be reportable.

#### Scenario: A cinematic takes over and returns
- **WHEN** a cinematic camera takes priority and later releases
- **THEN** the transition in and out SHALL blend by the declared policy without a visible snap

#### Scenario: Contributions are attributable
- **WHEN** the camera is not where a developer expects
- **THEN** the stack SHALL show each contribution and its weight

### Requirement: Lens model
Cameras SHALL support both a **gameplay lens** — vertical field of view, aspect, near and far — and a
**physical lens** — focal length, sensor dimensions, aperture, focus distance, shutter, and
sensitivity — mapping to one lens abstraction.

Projection SHALL support perspective, orthographic, and a custom projection supplied by a project.
Orthographic SHALL be first-class, since strategy, two-dimensional, and editor views require it.

Perspective cameras SHALL support an **infinite far plane** where the renderer path permits, and
culling and streaming SHALL NOT depend on a finite far distance for correctness.

The camera SHALL express projection **semantically**; the renderer SHALL construct backend-specific
matrices.

#### Scenario: Physical and gameplay lenses coexist
- **WHEN** a cinematic uses focal length and aperture while gameplay uses field of view
- **THEN** both SHALL map to the same lens abstraction and blend correctly

#### Scenario: Orthographic is not a special case
- **WHEN** a strategy or editor view uses orthographic projection
- **THEN** it SHALL use the same camera, rig, and view path

### Requirement: Follow, orbit, and constraints
The engine SHALL provide rig nodes for: **follow** with world-space, target-space, socket, or bounds
anchoring; **orbit** with yaw, pitch, distance, and limits; and **constraints** on position,
orientation, distance, region, volume, or spline.

Constraint nodes SHALL support the common camera forms — side-scrolling, rail, arena, and bounded
map — as compositions rather than as separate camera implementations.

Orbit input SHALL update intent; the rig SHALL resolve the resulting orientation, so that limits and
smoothing apply consistently.

#### Scenario: A rail camera
- **WHEN** a camera follows a spline through a level
- **THEN** it SHALL be a follow node constrained to a spline, not a separate camera type

### Requirement: Stable smoothing
Camera smoothing SHALL use **frame-rate independent** formulations expressed in physically
meaningful terms — half-life, frequency and damping ratio, or an equivalent — and SHALL produce the
same visual result at different frame rates.

Naive per-frame interpolation toward a target SHALL NOT be used, because its behaviour depends on
frame rate.

Smoothing state SHALL be part of the rig instance, and SHALL be resettable on cuts and
teleports.

#### Scenario: The same feel at any frame rate
- **WHEN** the same scene is played at 60 and at 144 frames per second
- **THEN** camera smoothing SHALL settle over the same wall-clock time

#### Scenario: A cut resets smoothing
- **WHEN** the camera cuts
- **THEN** smoothing state SHALL be reset rather than easing from the previous pose

### Requirement: Collision and occlusion are separate
The camera SHALL perform **collision** queries — keeping the camera out of geometry — and
**occlusion** queries — keeping the target visible — as distinct operations with distinct responses.

Responses SHALL be declared: pull in, slide, swap shoulder, fade the obstacle, or ignore by tag.

Queries SHALL be **batched** through the physics interface, and SHALL NOT be issued as scattered
synchronous casts from individual rig nodes.

Target visibility MAY sample several points on the target so that a partly occluded target is
distinguishable from a fully hidden one.

#### Scenario: Glass occludes without colliding
- **WHEN** a transparent surface stands between camera and target
- **THEN** the occlusion response SHALL apply, and the camera SHALL NOT be pushed as though it had
  collided

#### Scenario: Queries are batched
- **WHEN** several cameras evaluate collision and occlusion in a frame
- **THEN** their queries SHALL be batched rather than issued individually per node

### Requirement: Framing and composition
The engine SHALL provide a **framing** node computing camera distance, orientation, and lens to keep
one or more targets within the view, given their bounds, weights, and minimum screen sizes.

Framing SHALL support **screen-space composition constraints**: placing a target at a screen
fraction, maintaining headroom, keeping the horizon level, and dead zones within which target
movement produces no camera movement.

Framing SHALL be usable for strategy selections, boss encounters, dialogue, two-player fighting
cameras, and editor previews, from one implementation.

#### Scenario: Two combatants stay in frame
- **WHEN** two weighted targets separate
- **THEN** framing SHALL adjust distance and position to keep both visible at their minimum screen
  sizes

#### Scenario: A dead zone avoids jitter
- **WHEN** a target moves slightly within the dead zone
- **THEN** the camera SHALL not move

### Requirement: Aim is three things
The system SHALL distinguish:

| Concept | Meaning |
|---|---|
| **View aim** | Where the camera is looking |
| **Control aim** | Where the player is aiming — the value carried in commands and validated by the authority |
| **Weapon aim** | Where the weapon actually points, after offsets, recoil, and animation |

Aiming SHALL be a **rig layer** — offset, lens change, and reticle alignment — over the base camera,
not a separate camera implementation.

**The camera SHALL NOT be authoritative gameplay state.** A server SHALL validate against control
aim carried in a command and the player's simulated state, never against a client's camera
transform.

#### Scenario: Shots follow the weapon, not the camera
- **WHEN** a weapon is offset from the view
- **THEN** the fired direction SHALL derive from weapon aim, while the reticle reflects control aim

#### Scenario: The camera is not authority
- **WHEN** a hit is validated
- **THEN** the server SHALL use the command's control aim and the authoritative state, not a camera
  transform

### Requirement: Aim assistance
Aim assistance — magnetism, slowdown near targets, and snapping — SHALL be implemented as
**modifiers between input and camera intent**, informed by gameplay spatial queries.

Assistance SHALL be configurable and shall be part of the accessibility surface, not a hidden
constant.

Assistance SHALL NOT be implemented in device drivers or platform input handling, and SHALL be
observable in diagnostics so its effect can be measured.

#### Scenario: Assistance is inspectable
- **WHEN** aim assistance is active
- **THEN** diagnostics SHALL show the raw look, the assisted look, and the candidate that influenced
  it

### Requirement: Shake, recoil, and the impulse bus
Camera disturbance SHALL be **additive** over the evaluated base, and gameplay SHALL emit
**impulses** — a source, a tag, a strength, and a world position where applicable — rather than
modifying the camera.

Shake sources SHALL declare amplitude, frequency, duration, falloff, and spatial attenuation, and
SHALL be attenuated by distance and occlusion from their world position.

**Recoil** SHALL be modelled as an impulse with a spring response and recovery, integrated with the
aim layer rather than expressed as random noise.

A player setting SHALL scale or disable shake, as an accessibility requirement, without gameplay
changes.

#### Scenario: An explosion shakes nearby cameras
- **WHEN** an explosion emits an impulse
- **THEN** each local camera SHALL respond according to its own distance and occlusion

#### Scenario: Shake can be reduced
- **WHEN** a player reduces camera shake
- **THEN** every shake source SHALL scale, with no per-effect handling

### Requirement: Camera volumes
World volumes SHALL be able to influence cameras within them: field of view, smoothing, collision
policy, lens, composition, and post-process contribution.

Volumes SHALL have priority and blend distance, and overlapping volumes SHALL be resolved by
declared rules, as post-process volumes are.

Volumes SHALL be found through the world's spatial index rather than by testing every volume each
frame.

#### Scenario: An interior changes the camera
- **WHEN** the camera enters a tunnel volume
- **THEN** its distance and collision policy SHALL blend to the volume's settings over the blend
  distance

### Requirement: Strategy camera
The engine SHALL provide a **strategy camera** as a first-class composition: pan, orbit, zoom, edge
scrolling, terrain following, map bounds, and collision.

Zoom SHALL drive a **single normalised parameter** mapped through curves to height, distance, tilt,
and lens, so that zooming out raises and tilts the camera coherently rather than through independent
controls.

Terrain following SHALL query terrain height directly (see `terrain`) rather than casting physics
rays every frame.

Edge scrolling SHALL be derived from a pointer position exposed as an action, and SHALL NOT be
implemented in platform mouse handling.

Map bounds SHALL constrain the camera anchor to a rectangle, polygon, or world region.

#### Scenario: Zoom is one parameter
- **WHEN** the player zooms out
- **THEN** height, tilt, and distance SHALL follow the zoom curve together

#### Scenario: Terrain following is cheap
- **WHEN** the camera pans across terrain
- **THEN** its anchor height SHALL come from terrain height queries, not from per-frame physics
  casts

### Requirement: Screen and world projection
The camera SHALL expose projection utilities usable by gameplay, interface, and tools: screen point
to world ray, world position to screen point with depth and a behind-camera indication, and a
screen rectangle to a frustum for box selection.

These SHALL be derived from the evaluated camera and view, and SHALL NOT require renderer internals.

Results SHALL account for the viewport rectangle, so split-screen and editor viewports are correct
without special cases.

#### Scenario: Box selection in a strategy game
- **WHEN** a player drags a selection rectangle
- **THEN** the camera SHALL produce a frustum for the rectangle, and selection SHALL query it

#### Scenario: Placement follows the cursor
- **WHEN** a structure is placed
- **THEN** a screen ray SHALL be intersected with terrain, and the result SHALL be correct in a
  split-screen viewport

### Requirement: Large-world camera positions
Camera position SHALL use the world's **cell-relative representation** (see
`world-partition-and-streaming` and `core-math`), so cameras remain exact at planetary distances.

The camera SHALL publish the **rendering origin** used for camera-relative rendering, so the
renderer can rebase world positions without gameplay involvement.

Camera arithmetic in rig evaluation SHALL avoid accumulating error over long sessions and long
distances.

#### Scenario: A camera a thousand kilometres out
- **WHEN** the camera is far from the world origin
- **THEN** its pose SHALL be exact and its rendered result free of jitter

### Requirement: Render view production
Each evaluated camera SHALL produce one or more **render views** carrying: pose, projection
semantics, viewport rectangle, target, layer mask, quality settings, **importance**, and a
**history identity**.

Views SHALL be organisable into families that share work (see `rendering-architecture`), and views
SHALL declare importance so that residency and budget systems can prioritise them — a main view
above a reflection capture above an editor thumbnail.

Temporal jitter SHALL be applied to the view by the temporal framework, and the camera SHALL provide
an unjittered projection.

A view's **history identity** SHALL be stable across target changes and shall be invalidated on
cuts, so the temporal framework can associate accumulated history with the right view.

#### Scenario: Secondary views cost less
- **WHEN** a minimap and a main view are both active
- **THEN** their declared importance SHALL cause the minimap to receive lower detail and residency
  priority

#### Scenario: Jitter belongs to the renderer
- **WHEN** temporal anti-aliasing is enabled
- **THEN** the camera SHALL provide an unjittered projection and the temporal framework SHALL apply
  jitter

### Requirement: Camera cuts
A **camera cut** SHALL be a typed event carrying its cause — possession change, vehicle entry,
cinematic start, death, photo mode, teleport, or scripted — and SHALL be published to the systems
whose assumptions it breaks: temporal history, illumination convergence, shadow caching, texture and
geometry residency, and world streaming.

A cut SHALL invalidate the view's temporal history; changing a camera's target SHALL NOT
automatically be a cut.

**Anticipated cuts** — a cinematic's cut list, a scripted teleport — SHALL be announcable in advance
and SHALL become deadlines through the residency layer, so the destination is prepared rather than
discovered.

`sequencing-and-cinematics` is the principal producer of anticipated cuts: a compiled sequence knows
its camera track and its cut list, and SHALL publish upcoming cuts and future camera bounds ahead of
reaching them. A sequence SHALL drive cameras through the **camera stack** — rig selection, blends,
lens, and priority — and SHALL NOT write camera transforms.

#### Scenario: A cut does not smear
- **WHEN** a cinematic cuts between viewpoints
- **THEN** temporal history SHALL be invalidated and no accumulation SHALL blend across the cut

#### Scenario: A known cut is prepared
- **WHEN** a cinematic declares an upcoming cut
- **THEN** content at the destination SHALL be requested against that deadline

#### Scenario: A cinematic camera is still a camera
- **WHEN** a sequence takes control of the view
- **THEN** it SHALL contribute to the camera stack at a declared priority, and the camera system
  SHALL evaluate the result

### Requirement: Listener and streaming source
Each active player camera SHALL publish an **audio listener anchor** with a declared policy: at the
camera, at the controlled entity, or interpolated between them — because a distant third-person
camera and a character's ears are not the same place.

Each active camera SHALL publish a **streaming source** (see `world-partition-and-streaming`)
carrying position, velocity, orientation, frustum, prediction horizon, and importance.

The listener and the streaming source SHALL be **derived from the evaluated camera**, not configured
independently, so they cannot disagree about where the player is.

Predicted camera motion — velocity, angular velocity, an approaching target transition, or a
scheduled cut — SHALL be published so that streaming can prefetch.

#### Scenario: Audio is not fifty metres behind
- **WHEN** a third-person camera trails the character
- **THEN** the listener policy SHALL place the listener sensibly, rather than defaulting to the
  camera without consideration

#### Scenario: The camera drives prefetch
- **WHEN** the camera accelerates toward a region
- **THEN** its streaming source SHALL publish predicted motion and content SHALL be requested ahead

### Requirement: Evaluation timing
Camera evaluation SHALL declare a mode: **simulation** — evaluated on the fixed tick and
reproducible; **render** — evaluated per frame for visual smoothness; or **hybrid** — target state
from simulation, smoothing at render rate.

**Hybrid SHALL be the recommended default** for ordinary gameplay cameras, so a 60 hertz simulation
still produces smooth camera motion at high refresh rates.

Camera evaluation SHALL run after movement, physics, and animation pose, and before render
extraction, listener update, and streaming source publication.

Cameras evaluated in simulation mode SHALL NOT depend on wall-clock time, so replay reproduces them
exactly.

#### Scenario: Smooth at high refresh
- **WHEN** simulation runs at 60 hertz and display at 144
- **THEN** a hybrid camera SHALL interpolate its target and smooth per frame

#### Scenario: Reproducible when it matters
- **WHEN** a camera is evaluated in simulation mode
- **THEN** the same tick and inputs SHALL produce the same pose

### Requirement: Networking and replay
Camera state SHALL NOT be replicated per frame. Only **semantic** camera state that other peers
require — a spectator's target, a cinematic cue, a camera mode — SHALL be replicated.

Replays SHALL reconstruct cameras from recorded gameplay state, input, and camera mode events, and
SHALL additionally support recording an authored camera track for directed replay presentation.

Because the camera is not authoritative, small floating-point differences in camera evaluation
between peers SHALL NOT affect gameplay outcomes.

#### Scenario: No camera transform on the wire
- **WHEN** a networked session runs
- **THEN** camera transforms SHALL NOT be replicated each frame

#### Scenario: Replay reconstructs the view
- **WHEN** a replay is played
- **THEN** cameras SHALL be reconstructed from recorded state and mode events, with an authored
  track available for directed playback

### Requirement: Spectator, photo, and director cameras
The engine SHALL provide, as rig compositions rather than separate implementations: **spectator**
cameras (free fly, follow, orbit), a **photo mode** camera with detached control and lens controls,
and an optional **director** camera that scores candidate viewpoints and selects or blends among
them.

Photo mode SHALL be able to detach camera control from gameplay while the simulation is paused or
slowed, through the camera stack and a gameplay feature.

Director scoring SHALL consider visibility of the action, target importance, activity, and shot
repetition, and SHALL be replaceable per project.

#### Scenario: Photo mode is a stack entry
- **WHEN** photo mode is entered
- **THEN** a high-priority camera contribution SHALL take over, and gameplay camera state SHALL be
  preserved for the return

#### Scenario: A director follows the action
- **WHEN** a spectator uses the director camera
- **THEN** candidate viewpoints SHALL be scored and the selection SHALL blend rather than cut
  unnecessarily

### Requirement: Camera settings and accessibility
Player settings SHALL be first-class inputs to cameras and to input processing: look sensitivity,
inversion, field of view, shake scale, aim sensitivity, gyroscopic sensitivity, and edge scrolling.

Accessibility settings SHALL include at minimum: reduced or disabled camera shake, reduced head
motion, motion reduction, a field-of-view override, automatic centring behaviour, and adjustable
camera collision strength.

Settings SHALL be applied in **one place** — as processors, modifiers, or rig parameters — and SHALL
NOT be duplicated independently by input and camera implementations.

#### Scenario: One sensitivity setting
- **WHEN** a player changes look sensitivity
- **THEN** it SHALL apply once in the pipeline, and no camera code SHALL apply it a second time

#### Scenario: Motion reduction is global
- **WHEN** motion reduction is enabled
- **THEN** shake, head motion, and camera acceleration SHALL all respond

### Requirement: Camera diagnostics
The engine SHALL provide a camera inspector showing: the active stack with each contribution's
weight, the rig graph with each node's input and output, collision and occlusion query results with
the desired and corrected pose, the active volumes and their weights, the evaluated lens, and the
published listener and streaming source.

It SHALL report **why the camera changed**, naming the transition reason, and **why a rig node moved
the camera**, so that tuning is directed rather than exploratory.

Camera contributions to streaming SHALL be visible in the streaming diagnostics, connecting a camera
movement to the content it caused to load.

#### Scenario: Why is the camera here
- **WHEN** the camera is not where expected
- **THEN** the inspector SHALL show which node or contribution moved it and by how much

#### Scenario: Camera to content
- **WHEN** a camera movement causes a region to load
- **THEN** the streaming diagnostics SHALL attribute the request to that camera's source

### Requirement: Camera performance
Camera evaluation SHALL support several player cameras and dozens of auxiliary views without
becoming a measurable cost: a main camera rig SHALL evaluate well within a tenth of a millisecond on
a desktop-class processor under normal gameplay.

Evaluation SHALL allocate nothing per frame, and collision and occlusion queries SHALL be batched
rather than issued per node.

Auxiliary views — minimaps, reflection captures, thumbnails — SHALL be budgeted through their
declared importance rather than costing the same as a main view.

#### Scenario: Many views are affordable
- **WHEN** four player cameras and dozens of auxiliary views are active
- **THEN** camera evaluation SHALL remain negligible relative to rendering

### Requirement: Forbidden camera patterns
The following SHALL NOT appear, and each SHALL be checkable in review:

- A monolithic camera base class that camera behaviours inherit from
- Gameplay systems writing final camera transforms
- Per-node heap-allocated virtual graphs in the camera evaluation path
- Renderer jitter, backend projection conventions, or resource concepts in gameplay camera
  interfaces
- The camera target implicitly identical to the controlled entity
- Camera transforms treated as authoritative gameplay state
- Minimap, reflection, split-screen, and cinematic views forced through one global camera singleton
- Frame-rate-dependent smoothing formulations
- Per-frame physics ray casts where a direct terrain or spatial query exists

#### Scenario: A proposal is checked
- **WHEN** a change would let a gameplay system set the camera transform
- **THEN** it SHALL be flagged against this requirement
