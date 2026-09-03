# core-math Specification

## Purpose

Defines the mathematics layer: value types, the coordinate and depth conventions every other
subsystem depends on, SIMD strategy, spatial acceleration structures, geometry utilities, curves,
and random number generation.

Conventions are stated first and normatively, because a silent mismatch here corrupts every
downstream subsystem.

## Requirements

### Requirement: Coordinate conventions
CyberdyneEngine SHALL use a **right-handed, Y-up** 3D coordinate system in which an object's
local **−Z axis is forward**, +X is right, and +Y is up. Cameras look down their local −Z.

2D SHALL use screen coordinates with the origin at the top-left of a viewport and +Y downward.

Rotations SHALL be counter-clockwise about the axis when viewed from the positive axis toward the
origin. Default Euler order SHALL be **YXZ** (yaw, pitch, roll), and Euler angles SHALL be used
only at authoring and interchange boundaries, never as runtime rotation storage.

Units SHALL be **metres**, **seconds**, **kilograms**, and **radians** internally. Degrees appear
only in the editor and in explicitly named conversion helpers.

#### Scenario: Identity transform faces −Z
- **WHEN** an entity has an identity transform
- **THEN** its forward vector SHALL be `(0, 0, -1)`, its up `(0, 1, 0)`, and its right `(1, 0, 0)`

#### Scenario: Importer converts on ingest
- **WHEN** an asset authored in a Z-up or left-handed system is imported
- **THEN** the importer SHALL convert it to engine conventions at import time, so no runtime code
  branches on source handedness

### Requirement: Depth and projection conventions
Projection matrices SHALL map the view frustum to a **`[0, 1]` depth range** with **reversed Z**:
the near plane maps to 1.0 and the far plane to 0.0.

Depth buffers SHALL be cleared to 0.0, depth comparison SHALL be `GreaterEqual` for opaque
geometry, and shadow comparison samplers SHALL use `Greater`.

Reversed Z with a floating-point depth buffer SHALL be the only supported configuration, so
precision behaviour is uniform across backends.

#### Scenario: Depth test direction
- **WHEN** an opaque pipeline is created
- **THEN** its depth compare op SHALL be `GreaterEqual` and depth clear SHALL be 0.0

#### Scenario: Infinite far plane
- **WHEN** a perspective projection is created with an infinite far plane
- **THEN** reversed Z SHALL keep precision well-distributed at distance, and the projection
  helper SHALL expose this as a supported mode

### Requirement: Precision
Runtime math SHALL use 32-bit `float` for transforms, geometry, and rendering. `double` SHALL be
used only where explicitly required: the simulation clock, accumulated time, geodetic and
interchange coordinate helpers, and analysis tooling.

Large-world support SHALL be provided by **cell-relative coordinates** and **camera-relative
rendering** — positions are stored relative to a spatial cell and rebased to the camera before
submission — rather than by making the whole engine double precision.

The authoritative persistent form of a world position SHALL be cell-relative (see
`world-partition-and-streaming`): a cell coordinate plus a 32-bit local offset. Because the local
offset is bounded by cell size, 32-bit precision is sufficient at any distance from the world
origin. A 64-bit accessor exists for tooling and interchange and is not the runtime representation.

**Deterministic profiles constrain floating point** (see `simulation-and-determinism`). Under
`SamePlatform`, authoritative code requires a controlled floating-point environment — declared
rounding mode, denormal handling, and contraction policy — and transformations that alter results
are disallowed on authoritative paths. Under `CrossPlatform` and `Lockstep`, authoritative
computation requires **deterministic math types** provided as an optional module: fixed-point
scalars, vectors, angles, and deterministic transcendental approximations.

The deterministic math module SHALL NOT replace this library. Rendering, animation, and effects
continue to use ordinary floating point, and deterministic types are used only where an
authoritative path requires them.

The engine SHALL NOT claim that arbitrary floating-point code produces identical results across
architectures, compilers, or vector widths.

#### Scenario: Distant object stays stable
- **WHEN** an object is 100 km from the world origin
- **THEN** its cell-relative position SHALL be exact in 32-bit floats, and camera-relative
  rebasing SHALL keep its rendered transform free of visible jitter

#### Scenario: Time accumulation
- **WHEN** the main loop accumulates elapsed time
- **THEN** the accumulator SHALL be `double` so drift does not accumulate over long sessions

#### Scenario: Double precision does not spread
- **WHEN** a subsystem is tempted to store global double-precision positions
- **THEN** the cell-relative form SHALL be used instead, so precision handling stays confined to
  the world layer

#### Scenario: Cross-platform determinism has a mechanism
- **WHEN** a project requires cross-platform reproducibility of authoritative simulation
- **THEN** it SHALL use deterministic math types on those paths, rather than relying on ordinary
  floating point behaving identically everywhere

### Requirement: Math types
`core/math` SHALL provide: `Vec2`, `Vec3`, `Vec4`, `IVec2`, `IVec3`, `IVec4`, `Quat`, `Mat3`,
`Mat4`, `Transform` (rotation quaternion, translation, non-uniform scale), `Transform2D`,
`Rect`, `IRect`, `Aabb`, `Obb`, `Plane`, `Ray`, `Sphere`, `Frustum`, `Color`.

`Transform` SHALL be the canonical 3D transform representation — decomposed TRS, not a matrix —
so interpolation, comparison, and inversion are exact and cheap. Matrices are derived for
rendering.

Matrices SHALL be **column-major** in memory and use column-vector convention (`M * v`), matching
GLSL and the SPIR-V pipeline so no transposition occurs at upload.

#### Scenario: TRS interpolation
- **WHEN** two `Transform`s are interpolated
- **THEN** translation and scale SHALL be lerped and rotation slerped, with no matrix
  decomposition and no shear artifacts

#### Scenario: Upload without transposition
- **WHEN** a `Mat4` is written to a uniform buffer
- **THEN** its memory layout SHALL be directly consumable by the shader without a transpose

### Requirement: SIMD strategy
Vector and matrix operations SHALL be implemented against a thin SIMD abstraction with backends
for SSE4.2/AVX2 (x86-64), NEON (ARM64), and a portable scalar fallback.

`Vec3` SHALL be 12 bytes (three floats) for storage density; SIMD paths SHALL operate on `Vec4`
or on batched arrays. The engine SHALL NOT pad `Vec3` to 16 bytes.

Batch operations (transform arrays of points, cull arrays of AABBs) SHALL be provided as explicit
array-wide functions so vectorisation is deliberate rather than hoped for.

#### Scenario: Bulk transform
- **WHEN** a system transforms 10 000 points by one matrix
- **THEN** it SHALL call the batch API, which processes multiple points per SIMD instruction

#### Scenario: Portable correctness
- **WHEN** the scalar fallback is used
- **THEN** results SHALL match the SIMD paths within documented tolerance, verified by tests

### Requirement: Spatial acceleration structures
The engine SHALL provide:

- `DynamicBvh` — incremental, self-balancing BVH with fat AABBs, supporting insert, update,
  remove, AABB query, frustum query, and ray query; used by render culling and broadphase-style
  queries
- `Bvh<T>` — static BVH built with surface-area heuristic, for triangle meshes and baked data
- `SpatialHash` — uniform grid for evenly distributed dynamic objects
- `Octree` — for volumetric queries where a regular subdivision is preferable

`DynamicBvh` SHALL expand inserted bounds by a configurable margin so small movements do not
trigger restructuring.

#### Scenario: Small movement does not restructure
- **WHEN** an object moves within its expanded AABB
- **THEN** the tree SHALL not be modified

#### Scenario: Frustum query is conservative
- **WHEN** a frustum query runs
- **THEN** it MAY return nodes outside the frustum but SHALL NOT omit any that intersect it

### Requirement: Frustum culling primitives
`Frustum` SHALL store six planes with precomputed per-plane sign masks, and SHALL provide a
conservative AABB test that selects the extreme corner by sign mask.

The test SHALL be documented as conservative: false positives are accepted in exchange for
branch-free speed.

#### Scenario: Rejection is exact per plane
- **WHEN** an AABB's sign-selected extreme corner lies behind any frustum plane
- **THEN** the AABB SHALL be rejected

### Requirement: Geometry utilities
`core/geometry` SHALL provide: ray/AABB, ray/sphere, ray/plane, ray/triangle intersection;
segment/segment closest points; point-in-polygon; convex hull (2D and 3D); triangulation
(ear clipping and Delaunay); polygon boolean operations and offsetting; plane-set clipping;
rectangle atlas packing; and mesh utilities for tangent generation and vertex welding.

#### Scenario: Ray/triangle for picking
- **WHEN** the editor picks in the viewport
- **THEN** a ray query against the static BVH followed by ray/triangle tests SHALL return the
  nearest hit with barycentric coordinates

### Requirement: Curves and easing
The engine SHALL provide `Curve` (1D keyframed with tangents), `Curve2D`/`Curve3D` (splines with
arc-length parameterisation and baked lookup), `Gradient`, and a shared easing function table
covering linear, sine, quad, cubic, quart, quint, expo, circ, back, elastic, and bounce, each
with in, out, in-out, and out-in variants.

Arc-length parameterisation SHALL be baked so that sampling a path at constant speed is O(1).

#### Scenario: Constant-speed path following
- **WHEN** an entity follows a `Curve3D` at fixed speed
- **THEN** sampling by distance SHALL use the baked arc-length table, not per-frame integration

### Requirement: Random number generation
The engine SHALL provide `Random`, a PCG-family generator with explicit seeding and streams,
plus helpers for uniform integers and floats, normal distribution, and sampling on a sphere,
hemisphere, and disk.

Global implicit random state SHALL NOT exist; every generator is an explicit object, so
simulation can be made reproducible.

#### Scenario: Reproducible simulation
- **WHEN** a simulation is run twice with the same seed and the same fixed-step schedule
- **THEN** it SHALL produce identical results

#### Scenario: Independent streams
- **WHEN** several systems each hold their own `Random`
- **THEN** their sequences SHALL be independent, and adding a system SHALL not perturb the others
