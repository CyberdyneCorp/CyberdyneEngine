# rendering-culling-and-lod Specification

## Purpose

Defines how a scene of instances becomes per-view visible sets: spatial indexing, frustum
culling, occlusion culling, level of detail selection, visibility ranges and HLOD, and shadow
caster culling.

## Requirements

### Requirement: Spatial indexing
Each scene SHALL maintain spatial indices over its instances:

- a `DynamicBvh` over **renderable geometry**
- a `DynamicBvh` over **volumes** (lights, probes, decals, GI volumes, fog volumes)
- a flat array for **always-visible** instances that bypass spatial culling

Instance bounds SHALL be stored in a dense, cache-friendly array parallel to a compact
per-instance flags-and-mask array, so the broad-phase test touches minimal memory.

#### Scenario: Bounds and payload are separated
- **WHEN** frustum culling runs
- **THEN** it SHALL iterate a packed array of bounds plus a 32-bit flags word and a 32-bit layer
  mask, touching heavier per-instance data only for survivors

#### Scenario: Static instances are cheap to maintain
- **WHEN** an instance is marked static
- **THEN** it SHALL be inserted once with tight bounds and never updated

### Requirement: Frustum culling
Culling SHALL reject instances by, in order: layer mask against the view's mask, then
conservative frustum-versus-AABB using precomputed plane sign masks, then optional per-instance
distance limits.

Culling SHALL be parallelised across job workers above a configurable instance-count threshold,
each worker producing a local visible list merged by pointer transfer.

#### Scenario: Layer rejection precedes geometry
- **WHEN** an instance's layer mask does not intersect the view's
- **THEN** it SHALL be rejected before any geometric test

#### Scenario: Parallel culling merges cheaply
- **WHEN** culling runs on eight workers
- **THEN** merging SHALL transfer page or range ownership rather than copying instance data

#### Scenario: Conservative by design
- **WHEN** the AABB test passes
- **THEN** the instance MAY still be off-screen; the pipeline SHALL tolerate false positives

### Requirement: Occlusion culling
The engine SHALL support occlusion culling using a **hierarchical depth buffer** built from the
previous frame's depth, reprojected into the current view.

Instances SHALL be tested by projecting their bounds and comparing against the appropriate HZB mip.
A **visibility hysteresis** SHALL keep recently visible instances visible for a configurable number
of frames to prevent flicker from reprojection error.

The HZB SHALL additionally serve **cluster-granular occlusion** for virtual geometry, and SHALL
support a **two-pass** scheme: a first pass drawing what previous-frame visibility suggests, an HZB
rebuilt from that depth, and a second pass testing uncertain or newly visible geometry.

Instances MAY opt out with an `IgnoreOcclusion` flag. False occlusion SHALL be impossible for
correctness-critical cases: reprojection SHALL be conservative, and newly disoccluded regions SHALL
be treated as visible.

#### Scenario: Object behind a wall
- **WHEN** an instance's projected bounds are fully behind the HZB depth
- **THEN** it SHALL be culled from the visible list while remaining eligible to cast shadows

#### Scenario: Camera cut
- **WHEN** the camera teleports
- **THEN** the previous frame's depth SHALL be discarded and no occlusion culling SHALL be
  applied for that frame

#### Scenario: Fast-moving object
- **WHEN** an object becomes disoccluded between frames
- **THEN** hysteresis and conservative reprojection SHALL prevent it from popping in late

#### Scenario: Two-pass resolves disocclusion
- **WHEN** geometry becomes visible because an occluder moved
- **THEN** the second pass SHALL draw it in the same frame rather than a frame late

### Requirement: GPU-driven culling
Where the device supports compute and indirect drawing, the renderer SHALL support **GPU-driven
culling**: instance bounds are uploaded once, culling runs as a compute pass producing compacted
draw arguments, and drawing uses indirect commands.

For virtual geometry assets, GPU-driven culling SHALL extend to **cluster granularity**: after
instance culling, hierarchy traversal and per-cluster tests select the geometry actually rasterised
(see `virtual-geometry`).

A CPU path SHALL remain for devices lacking the capability and for cases needing CPU visibility
results (audio occlusion, gameplay queries).

#### Scenario: Large static scene
- **WHEN** a scene contains a million static instances
- **THEN** GPU-driven culling SHALL avoid per-instance CPU work, with the CPU submitting a small
  number of indirect draws

#### Scenario: CPU needs visibility
- **WHEN** gameplay queries whether an instance is visible
- **THEN** the CPU path SHALL provide it, or the GPU result SHALL be read back with one frame of
  latency, documented as such

#### Scenario: Culling continues below the instance
- **WHEN** a virtual geometry instance survives instance culling
- **THEN** cluster-granular culling SHALL further reduce the geometry rasterised, rather than
  drawing the whole object

### Requirement: Level of detail
The engine SHALL support two detail strategies, selected per asset:

- **Discrete LOD chains** — an instance declares levels, each with a mesh and a screen-space error
  or coverage threshold, selected per instance
- **Virtual geometry** — detail selected per cluster on the GPU from projected geometric error,
  with no authored levels (see `virtual-geometry`)

For discrete LOD, selection SHALL be computed from projected screen coverage, adjusted by a global
LOD bias, a per-instance bias, and the view's quality setting. Shadow and reflection views SHALL
use their own bias so lower LODs are used where detail is not visible.

Transitions SHALL support **dithered cross-fade** over a configurable distance band to avoid
popping, resolved by temporal anti-aliasing.

Both strategies SHALL respond to the same quality settings and view-specific biases, so a project
mixing them behaves coherently.

#### Scenario: Coverage drives selection
- **WHEN** an instance's projected coverage falls below a level's threshold
- **THEN** the next LOD SHALL be selected, with hysteresis to prevent oscillation at the boundary

#### Scenario: Shadows use coarser LODs
- **WHEN** rendering a shadow map
- **THEN** the shadow LOD bias SHALL select coarser levels than the main view for the same
  instance

#### Scenario: Cross-fade
- **WHEN** an instance is inside a transition band
- **THEN** both levels SHALL be drawn with complementary dither masks, and TAA SHALL resolve the
  blend

#### Scenario: Mixed strategies in one scene
- **WHEN** a scene contains both discrete-LOD and virtual geometry assets
- **THEN** both SHALL respond to the same quality settings, and neither SHALL require the other to
  be disabled

### Requirement: Visibility ranges and HLOD
An instance MAY declare a visibility range (`begin`, `end`) with fade margins, and MAY declare a
**visibility parent**, forming a hierarchy in which a parent's visibility replaces its children's.

This mechanism SHALL implement HLOD: a distant cluster of buildings is replaced by a single
merged proxy.

Fade modes SHALL be: none (hard switch), self (fade the instance's own alpha), and dependents
(cross-fade parent and children).

#### Scenario: HLOD swap
- **WHEN** the camera moves far enough for a proxy to become visible
- **THEN** the proxy SHALL fade in while its children fade out, with both drawn during the
  transition

#### Scenario: Nested HLOD
- **WHEN** HLOD proxies are themselves grouped under a coarser proxy
- **THEN** the hierarchy SHALL resolve to exactly one visible level per branch

### Requirement: Shadow caster culling
For each shadow-casting light, culling SHALL produce a caster list per shadow view (per cascade
for directional lights, per face for cube shadows).

Casters SHALL additionally be rejected when they cannot cast into the camera frustum, by testing
against the convex volume swept between the light and the camera frustum.

Where a light's shadow is rendered for multiple camera views in one frame, the tighter culling
SHALL be disabled for that light so one shadow map is valid for all of them.

#### Scenario: Caster outside the shadow-receiving region
- **WHEN** a caster is inside the light's volume but cannot project into the camera frustum
- **THEN** it SHALL be excluded from the shadow render list

#### Scenario: Shared shadow map
- **WHEN** two views in one frame use the same light's shadow
- **THEN** the shadow SHALL be culled against the union of their frustums

### Requirement: Culling results
Culling SHALL produce, per view, typed lists: opaque instances, transparent instances,
instances requiring motion vectors, lights, decals, reflection probes, GI volumes, and per-shadow
caster lists.

Lists SHALL be allocated from the frame arena and sorted before submission (see
`rendering-forward-clustered`).

#### Scenario: Lists are frame-scoped
- **WHEN** the frame ends
- **THEN** all culling result memory SHALL be released by resetting the frame arena

### Requirement: Culling diagnostics
The engine SHALL report per view: instances tested, rejected by layer, rejected by frustum,
rejected by occlusion, rejected by visibility range, and finally visible; plus LOD level
histograms and culling wall-clock time.

#### Scenario: Diagnosing a slow frame
- **WHEN** a frame has an unexpectedly high draw count
- **THEN** the statistics SHALL show at which stage instances survived that were expected to be
  culled
