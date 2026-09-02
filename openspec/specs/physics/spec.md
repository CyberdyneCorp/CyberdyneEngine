# physics Specification

## Purpose

Defines the physics layer: the engine-owned `PhysicsServer` interface, the **Jolt Physics**
backend that implements 3D simulation, the 2D solution, how physics integrates with ECS and the
fixed-step loop, queries, character movement, and determinism.

Jolt is integrated rather than reimplemented. Writing a competitive rigid-body solver is a
multi-year effort with no differentiating value for this engine; Jolt is mature, multithreaded,
deterministic, and permissively licensed.

## Requirements

### Requirement: Engine-owned physics interface
`PhysicsServer` SHALL be an engine-defined, handle-based interface that backends implement. The
rest of the engine SHALL depend only on this interface, never on backend types.

The interface SHALL cover: worlds, bodies (static, kinematic, dynamic), colliders and shapes,
constraints, character controllers, queries, and simulation stepping.

#### Scenario: Backend is replaceable
- **WHEN** a project selects a different physics backend
- **THEN** no gameplay code, component definition, or scene asset SHALL require changes

#### Scenario: Backend types do not leak
- **WHEN** engine or game code is compiled
- **THEN** no Jolt type SHALL appear in any header outside the backend module

### Requirement: Jolt as the 3D backend
The default 3D backend SHALL be **Jolt Physics**, wrapped so that:

- Engine handles map to Jolt body ids
- Engine shapes map to Jolt shapes, with a shape cache so identical shapes are shared
- Collision layers and masks map to Jolt's broad-phase layers and object layer filters
- Jolt's job system is bridged to the engine's job system rather than running its own threads
- Jolt's temp allocator is bridged to an engine arena

Backend-specific tuning (solver iterations, penetration slop, baumgarte factor, speculative
contact distance, sleep thresholds) SHALL be exposed as configuration with documented defaults.

#### Scenario: One job system
- **WHEN** physics steps
- **THEN** its internal parallelism SHALL run on engine job workers, so physics and other work
  share one thread pool and one scheduler

#### Scenario: Shape sharing
- **WHEN** 1 000 entities use an identical box collider
- **THEN** one Jolt shape SHALL be created and referenced by all of them

### Requirement: 2D physics
2D physics SHALL be provided by running the 3D backend **constrained to a plane**: bodies are
created with locked Z translation and locked X/Y rotation, and 2D shapes are extruded to thin 3D
shapes.

This SHALL be an implementation strategy, not a leaked detail: the 2D API, components, and
queries SHALL be genuinely 2D (`Vec2`, angles, 2D shapes).

An independent 2D solver is an explicitly **deferred** decision, to be revisited if profiling
shows the constrained-3D approach is inadequate for 2D-heavy projects.

#### Scenario: 2D API is 2D
- **WHEN** gameplay code queries a 2D raycast
- **THEN** it SHALL pass a `Vec2` origin and direction and receive a `Vec2` normal, with no
  awareness of the third axis

#### Scenario: Constraint is enforced
- **WHEN** a 2D body is subjected to a force with a Z component
- **THEN** the locked degrees of freedom SHALL prevent any out-of-plane motion or drift

### Requirement: Physics components
Physics SHALL be expressed as ECS components:

| Component | Meaning |
|---|---|
| `RigidBody` | Dynamic body: mass, centre of mass, inertia, damping, gravity scale, sleep settings |
| `StaticBody` | Non-moving collision geometry |
| `KinematicBody` | Moved by code or animation; pushes dynamic bodies but is not pushed |
| `Collider` | A shape with a local transform, material, and layer/mask; several may attach to one body |
| `CharacterController` | Capsule-based movement controller (see below) |
| `Constraint` | A joint between two bodies |
| `Trigger` | A non-solid volume reporting overlap events |
| `PhysicsMaterial` | Friction, restitution, and their combine modes |

Shapes SHALL include: sphere, box, capsule, cylinder, convex hull, triangle mesh (static only),
height field, and compound.

#### Scenario: Multiple colliders per body
- **WHEN** a body has several colliders
- **THEN** they SHALL form a compound shape with a combined mass distribution computed from their
  volumes and the body's density or explicit mass

#### Scenario: Triangle mesh on a dynamic body
- **WHEN** a triangle mesh collider is placed on a dynamic body
- **THEN** it SHALL be rejected with a diagnostic recommending convex decomposition, since
  concave dynamic bodies are not supported

### Requirement: Fixed-step integration
Physics SHALL step exactly once per simulation tick at the fixed timestep, within the `Physics`
stage, and SHALL never step during a variable-rate frame.

Transforms written by physics SHALL be published to `LocalTransform`/`WorldTransform` after the
step, and rendering SHALL interpolate between the previous and current physics transforms using
the frame's interpolation alpha.

#### Scenario: Rendering interpolates physics
- **WHEN** the frame rate exceeds the physics rate
- **THEN** rendered positions SHALL interpolate between physics ticks rather than stepping

#### Scenario: Teleport suppresses interpolation
- **WHEN** a body is teleported
- **THEN** the teleport flag SHALL suppress interpolation for that frame, avoiding a smear across
  the world

### Requirement: Collision events
Collision and trigger events SHALL be delivered as ECS event channels: `CollisionEnter`,
`CollisionStay`, `CollisionExit`, `TriggerEnter`, `TriggerStay`, `TriggerExit`, each carrying the
entity pair, contact points, normals, and impulses where applicable.

Events SHALL be written during the physics step and readable in the same tick's `Simulation`
stage.

Behaviours SHALL additionally receive callback forms (`onCollisionEnter`, `onTriggerEnter`) for
ergonomic scripting, dispatched from the same event data.

#### Scenario: Systems and behaviours see the same events
- **WHEN** a collision occurs
- **THEN** both a system reading the channel and a behaviour receiving the callback SHALL observe
  identical data in the same tick

#### Scenario: Contact filtering
- **WHEN** a body enables contact reporting only above an impulse threshold
- **THEN** events below the threshold SHALL not be generated, avoiding event floods on resting
  contacts

### Requirement: Collision filtering
Collision SHALL be filtered by a layer and mask system: each collider has a layer and a mask, and
two colliders interact only if each one's layer is in the other's mask.

A per-pair **collision matrix** SHALL be configurable at the project level, and per-body ignore
lists SHALL be supported for exceptions.

#### Scenario: One-way filtering is symmetric
- **WHEN** A's mask includes B's layer but not vice versa
- **THEN** the pair SHALL NOT collide, because the filter requires mutual acceptance

### Requirement: Queries
`PhysicsServer` SHALL provide: raycast (first hit and all hits), shape cast (sweep), overlap
test (shape and point), and closest-point queries.

Every query SHALL accept: layer and mask filters, an ignore list, a maximum distance or hit
count, a face-culling option, and a flag for whether triggers are included.

Queries SHALL be safe to run from parallel systems during the `Simulation` stage, reading a
consistent post-step world state.

#### Scenario: Raycast excluding self
- **WHEN** a character raycasts downward
- **THEN** it SHALL exclude its own collider and report the ground hit with position, normal,
  distance, entity, and physics material

#### Scenario: Parallel queries
- **WHEN** many entities raycast concurrently from a parallel system
- **THEN** the queries SHALL be thread-safe and SHALL NOT mutate simulation state

#### Scenario: Query during the step
- **WHEN** a query is attempted while the physics step is in progress
- **THEN** it SHALL be rejected in development builds with a diagnostic, since the world is
  mid-solve

### Requirement: Character controller
The engine SHALL provide a capsule-based `CharacterController` implementing collide-and-slide
movement with: ground detection and a maximum slope angle, step offset for stairs, ceiling
detection, a skin width, a maximum iteration count, pushing of dynamic bodies with configurable
force, and **moving platform** support that inherits platform motion.

Two modes SHALL be supported: **grounded** (gravity, slopes, stairs) and **floating** (six
degrees of freedom, for swimming and flying).

#### Scenario: Walking up stairs
- **WHEN** a character moves into a step below the step offset
- **THEN** it SHALL be lifted onto the step without a jump

#### Scenario: Steep slope
- **WHEN** a slope exceeds the maximum angle
- **THEN** the character SHALL be reported as touching a wall rather than grounded, and SHALL
  slide down

#### Scenario: Moving platform
- **WHEN** a character stands on a kinematic platform that moves
- **THEN** the platform's motion SHALL be applied to the character, and on leaving, the configured
  inherited-velocity policy SHALL apply

### Requirement: Constraints
The engine SHALL provide constraints: fixed, point (ball-and-socket), hinge (with limits and a
motor), slider, distance, cone, swing-twist, six-degrees-of-freedom (per-axis limits, motors, and
springs), and rack-and-pinion / gear where the backend supports them.

Constraints SHALL expose a breakable option with a force or torque threshold.

#### Scenario: Motorised hinge
- **WHEN** a hinge motor targets an angular velocity with a maximum torque
- **THEN** the solver SHALL drive it within that limit

#### Scenario: Breakable joint
- **WHEN** a constraint's force exceeds its break threshold
- **THEN** it SHALL be disabled and a `ConstraintBroken` event emitted

### Requirement: Soft bodies and additional simulation
The engine SHALL support, where the backend provides them: soft bodies (cloth), and vehicle
simulation with wheels, suspension, and a drivetrain.

**Ragdolls** SHALL be supported as a configuration of bodies and constraints derived from a
skeleton via a ragdoll profile asset (see `animation-and-skinning`), supporting:

- **Full ragdoll** — simulation-driven, blended in from the current animated pose and velocity
- **Powered ragdoll** — bodies follow an animated target pose through joint motors, with
  configurable strength per body
- **Partial ragdoll** — a subset of bodies simulated while the rest remains animation-driven, with
  a blend region between them

Blending between animation-driven and simulation-driven SHALL be per body and continuous, so
transitions are not instantaneous switches.

#### Scenario: Ragdoll activation
- **WHEN** a character's ragdoll is activated
- **THEN** its bodies SHALL be driven by physics from their current animated pose and velocity,
  with an optional blend period

#### Scenario: Unsupported feature
- **WHEN** a backend does not implement soft bodies
- **THEN** the capability query SHALL report it and creation SHALL fail with a clear diagnostic

#### Scenario: Powered ragdoll follows animation
- **WHEN** a powered ragdoll's motors are configured to follow an animated pose
- **THEN** the bodies SHALL track that pose while still colliding with the world, and an external
  impulse SHALL produce a physical reaction that recovers toward the animation

### Requirement: Determinism
Physics SHALL be deterministic for a fixed sequence of inputs on the same binary and platform:
the same initial state and the same per-tick inputs SHALL produce identical results.

Cross-platform determinism SHALL NOT be guaranteed by default, and this SHALL be documented,
since it depends on floating-point behaviour across architectures.

This limitation is **load-bearing for networking**: `Lockstep` mode (see
`networking-and-replication`) requires bit-identical simulation across every participating peer,
and therefore SHALL be restricted to peers sharing a platform, architecture, and binary build.
Supporting cross-platform lockstep would require a fixed-point or soft-float simulation path,
which the engine does not provide and which is recorded as deferred rather than planned.

The engine SHALL provide a determinism test mode that hashes world state per tick to detect
divergence.

#### Scenario: Replay reproduces a session
- **WHEN** a recorded input sequence is replayed on the same build and platform
- **THEN** the simulation SHALL reproduce the original result

#### Scenario: Divergence is detected
- **WHEN** determinism test mode runs and a hash mismatches at tick N
- **THEN** the tick number and the diverging body SHALL be reported

#### Scenario: Lockstep scope follows from this guarantee
- **WHEN** a lockstep session is formed
- **THEN** participants SHALL be verified to share platform, architecture, and build, because
  physics determinism is guaranteed only within that scope

### Requirement: Physics debugging
The engine SHALL provide debug visualisation of: collider shapes, contact points and normals,
constraint anchors and limits, body sleep state, velocities, centres of mass, broad-phase bounds,
and query shapes and results.

Statistics SHALL include: active body count, contact count, constraint count, step time broken
down by broad phase, narrow phase, and solve, and island counts.

#### Scenario: Diagnosing a slow step
- **WHEN** physics time is high
- **THEN** the breakdown SHALL show whether cost is in broad phase, narrow phase, or solving

#### Scenario: Colliders do not match visuals
- **WHEN** collider debug drawing is enabled
- **THEN** shapes SHALL be drawn at their simulated transforms so mismatches with the visual mesh
  are immediately visible
### Requirement: Heightfield and terrain collision
The physics interface SHALL support **heightfield collision shapes** as a first-class shape type,
registered and updated in bulk rather than as individual bodies.

Terrain collision (see `terrain`) SHALL be registered on cell activation in bulk, and **updated
regionally** when terrain deformation of the gameplay or structural class occurs, without
rebuilding unaffected regions.

Heightfield holes SHALL be representable, so a cave entrance is not blocked by an invisible floor.

Terrain collision resolution SHALL be independent of terrain rendering detail, and a region MAY be
visible with coarse or absent collision according to its streaming channels.

#### Scenario: A region's collision arrives at once
- **WHEN** a cell containing terrain is activated
- **THEN** its collision SHALL be registered in bulk rather than one shape at a time

#### Scenario: A crater updates collision locally
- **WHEN** terrain is deformed by an explosion
- **THEN** only the affected region's collision SHALL be updated

### Requirement: Buoyancy and water interaction
The physics interface SHALL support **buoyancy** driven by water queries (see `water`): displacement
force from submerged volume or from multiple sample points on a body, linear and angular drag
through water, and advection by the water's surface velocity.

Buoyancy SHALL sample the **authoritative** displacement bands declared by the water body, so that
physics and rendering agree about where the surface is.

Character controllers SHALL support swimming and wading states derived from water depth.

Buoyancy SHALL respect the determinism guarantees stated for physics: within a platform, the same
inputs SHALL produce the same result.

#### Scenario: A vessel pitches on swell
- **WHEN** a vessel longer than the wave length floats on swell
- **THEN** multi-point sampling SHALL make it pitch and roll rather than translate rigidly

#### Scenario: Physics and rendering agree
- **WHEN** the renderer displaces the surface by an authoritative band
- **THEN** buoyancy SHALL sample the same displacement, and a floating body SHALL not sit beneath
  or above the visible surface
