## MODIFIED Requirements

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
