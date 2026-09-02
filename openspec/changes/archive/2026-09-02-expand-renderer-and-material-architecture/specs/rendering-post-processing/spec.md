## MODIFIED Requirements

### Requirement: Anti-aliasing
The engine SHALL support:

- **MSAA** (2×/4×/8×) in the forward pipeline
- **TAA** — velocity-reprojected temporal accumulation with neighbourhood clamping, a variance
  or YCoCg clipping strategy, and separate handling for disoccluded regions
- **FXAA** and **SMAA** as post-tonemap spatial options
- **Alpha-to-coverage** for masked materials under MSAA

TAA SHALL consume the **temporal framework** (see `temporal-rendering`) for jitter, motion
vectors, history storage, reprojection, disocclusion classification, and history invalidation.
It SHALL NOT implement its own.

TAA SHALL provide a sharpening pass to counteract the softening it introduces.

#### Scenario: Ghosting is suppressed
- **WHEN** an object moves over a contrasting background
- **THEN** neighbourhood clamping SHALL reject stale history, avoiding a trail

#### Scenario: Disocclusion
- **WHEN** geometry is revealed from behind an occluder
- **THEN** the temporal framework SHALL classify those pixels as disoccluded, history SHALL be
  rejected for them, and they SHALL be reconstructed spatially

#### Scenario: TAA requires prerequisites
- **WHEN** TAA is requested without motion vectors
- **THEN** the temporal framework SHALL enable their production, or TAA SHALL be refused with a
  diagnostic

#### Scenario: Camera cut is handled by the framework
- **WHEN** the camera cuts
- **THEN** TAA's history SHALL be invalidated by the framework's cut event, not by logic TAA
  implements itself

### Requirement: Temporal upscaling
The engine SHALL support rendering the 3D scene at a reduced internal resolution and
reconstructing it at output resolution, with UI drawn at native resolution.

Upscalers SHALL be pluggable behind one interface taking colour, depth, motion vectors, exposure,
and jitter — the latter three supplied by the temporal framework. The engine SHALL ship a
**built-in temporal upscaler** and SHALL define the integration seams for vendor upscalers (FSR,
DLSS, XeSS, MetalFX) as optional modules, since their licensing and distribution differ. Public
renderer interfaces SHALL NOT depend on vendor types.

Dynamic resolution scaling SHALL be supported, and internal resolution SHALL be a **budget
allocation held by the renderer budget arbiter** (see `rendering-architecture`), not an
independent controller measuring frame time. Resolution SHALL adjust within configured bounds on
the arbiter's time constant.

#### Scenario: Half-resolution rendering
- **WHEN** internal resolution is 50 % with temporal upscaling
- **THEN** the 3D scene SHALL be reconstructed at full resolution using jitter, motion vectors,
  and history, while UI is composited at native resolution

#### Scenario: Dynamic resolution holds a budget
- **WHEN** GPU frame time exceeds the budget
- **THEN** the arbiter SHALL reduce the resolution allocation within configured bounds, and raise
  it again when headroom returns

#### Scenario: Vendor upscaler absent
- **WHEN** a vendor upscaler module is not present
- **THEN** the built-in upscaler SHALL be used with no content changes

#### Scenario: Resolution does not fight other controllers
- **WHEN** resolution is reduced, lowering every subsystem's measured cost
- **THEN** subsystem controllers SHALL NOT interpret the change as headroom to spend; only the
  arbiter SHALL re-evaluate allocations
