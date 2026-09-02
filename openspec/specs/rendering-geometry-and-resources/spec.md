# rendering-geometry-and-resources Specification

## Purpose

Defines GPU-side content: mesh formats and vertex compression, LOD chains, instancing, skinning
and blend shapes, texture formats and streaming, and procedural geometry. Visual effects and
particles are specified separately in `vfx-system`.

## Requirements

### Requirement: Mesh representation
A **mesh** SHALL consist of one or more **surfaces**, each with a vertex buffer set, an index
buffer, a material slot index, and bounds.

Vertex data SHALL be split into separate streams so passes bind only what they need:

| Stream | Contents | Consumed by |
|---|---|---|
| Position | Position | All passes including depth and shadow |
| Normal/Tangent | Octahedral normal and tangent with sign | Shading passes |
| UV | UV0, UV1 (lightmap), additional channels | Shading passes |
| Colour | Vertex colour | Shading passes when present |
| Skin | Bone indices and weights | Skinned meshes only |

Index buffers SHALL use 16-bit indices where the vertex count permits, otherwise 32-bit.

#### Scenario: Shadow pass binds less
- **WHEN** a shadow pass renders a mesh
- **THEN** it SHALL bind only the position stream (plus UV for alpha-tested materials)

#### Scenario: Streams are optional
- **WHEN** a mesh has no vertex colours
- **THEN** the colour stream SHALL be absent and the pipeline SHALL be specialised accordingly

### Requirement: Vertex compression
Vertex attributes SHALL be compressed by default:

- **Normals and tangents**: octahedral-encoded into two 16-bit signed normalised components,
  with the tangent sign packed into the encoding
- **UVs**: 16-bit unorm relative to a per-surface UV bounding box where the range permits,
  32-bit float otherwise
- **Positions**: optionally quantised to 16-bit unorm relative to the surface's bounding box,
  with the box supplied per draw
- **Colours**: 8-bit unorm

Compression SHALL be selectable per mesh at import, with a documented precision impact, and the
engine SHALL detect when quantisation would produce visible error and warn.

#### Scenario: Quantised positions
- **WHEN** a mesh's bounding box is small relative to its detail
- **THEN** 16-bit quantised positions SHALL be used, halving position bandwidth with no visible
  error

#### Scenario: Precision warning
- **WHEN** a very large mesh would suffer visible quantisation error
- **THEN** the importer SHALL warn and default to full precision

### Requirement: Mesh LOD
A mesh asset SHALL carry a **traditional representation**, a **virtual geometry representation**
(see `virtual-geometry`), or both.

The traditional representation SHALL support LOD chains generated at import using a simplification
library, with per-level screen coverage thresholds, and optional **shadow proxy** and **collision
proxy** levels. Simplification SHALL preserve UV seams, normals within a tolerance, and material
boundaries.

Where an asset has a virtual representation, its traditional representation serves as the
**fallback**: used on devices without the required capabilities, for ray tracing acceleration
structures, for physics proxy generation, and for editor tooling. It SHALL still be produced.

Authored LOD chains SHALL remain supported for the traditional path; they SHALL NOT be required for
assets using virtual geometry.

#### Scenario: LOD generation at import
- **WHEN** a mesh is imported with LOD enabled
- **THEN** a chain SHALL be generated to the configured triangle reduction targets, with UV seams
  preserved

#### Scenario: Manual LODs
- **WHEN** an artist supplies hand-authored LOD meshes
- **THEN** they SHALL be used instead of generated ones, with the same threshold mechanism

#### Scenario: Virtual asset still has a fallback
- **WHEN** an asset is cooked with virtual geometry enabled
- **THEN** a traditional representation SHALL also be produced for fallback, ray tracing, and
  tooling use

#### Scenario: No manual LODs required
- **WHEN** an asset uses virtual geometry
- **THEN** no LOD chain SHALL need to be authored for its primary rendering path

### Requirement: Instancing
The engine SHALL support:

- **Automatic instancing** — draws sharing a mesh, material, and pipeline are merged into
  instanced draws during submission
- **Explicit instanced meshes** — a single instance holding a large transform buffer, with
  optional per-instance colour and custom data, a visible-count cap, and optional GPU culling
  producing indirect draw arguments

#### Scenario: Automatic merge
- **WHEN** 500 entities share one mesh and material
- **THEN** submission SHALL merge them into instanced draws without the content author doing
  anything

#### Scenario: GPU-culled instancing
- **WHEN** an instanced mesh holds 100 000 transforms and GPU culling is available
- **THEN** culling SHALL run as a compute pass producing compacted indirect arguments, with the
  CPU submitting one indirect draw

### Requirement: Skinning
Skinned meshes SHALL be transformed by a **compute pass** writing into per-instance output vertex
buffers, so the result is reusable across passes (depth, shadows, main) without re-skinning.

Bone matrices SHALL be read from the **GPU pose world** (see `animation-and-skinning`), the shared
GPU-side pose representation, rather than from a buffer uploaded independently per consumer.

Skinning SHALL support up to 4 or 8 influences per vertex (selectable per mesh) and **dual
quaternion** skinning as an option alongside linear blend skinning.

Output buffers SHALL be double buffered so the previous frame's positions are available for
motion vectors, with a tolerance so stepped animation still yields correct velocities.

Skinning SHALL respect the instance's animation LOD tier: instances at a baked tier SHALL use pose
textures or vertex animation rather than skeletal skinning, and instances at reduced bone LOD SHALL
use mesh LODs whose influences reference only retained joints.

#### Scenario: Skinned once, used many times
- **WHEN** a skinned mesh appears in the depth prepass, a shadow pass, and the opaque pass
- **THEN** skinning SHALL run once and all three SHALL read the same output buffer

#### Scenario: Skinned bounds
- **WHEN** a skinned mesh animates
- **THEN** its bounds SHALL be computed from bone transforms and per-bone bounds, not from the
  bind pose

#### Scenario: One pose representation
- **WHEN** skinning, motion vector generation, and VFX attachment all need bone transforms
- **THEN** all SHALL read the GPU pose world rather than each maintaining its own upload

### Requirement: Blend shapes
Meshes SHALL support blend shapes (morph targets) stored as sparse deltas of position, normal,
and tangent, applied in the same compute pass as skinning with per-shape weights.

Sparse storage SHALL record only vertices a shape actually moves.

#### Scenario: Facial animation
- **WHEN** 50 blend shapes exist and 5 have non-zero weight
- **THEN** only the 5 active shapes' deltas SHALL be read and applied

### Requirement: Texture formats and compression
The engine SHALL support uncompressed formats (R8, RG8, RGBA8, R16F, RG16F, RGBA16F, R32F,
RG11B10F, RGB9E5) and compressed formats **BC1–BC7** (desktop) and **ASTC** (mobile), selected
per platform at cook time.

Textures SHALL declare their **usage** (colour, normal, data, HDR) so the cooker selects an
appropriate format and colour space automatically.

Cooked textures SHALL include a full mip chain generated in the correct colour space (linear for
data, sRGB-aware for colour) with optional alpha-coverage preservation for masked materials.

#### Scenario: Usage drives format
- **WHEN** a texture is marked as a normal map
- **THEN** it SHALL be cooked to BC5 (two-channel) on desktop and the appropriate ASTC mode on
  mobile, and stored linear

#### Scenario: Alpha coverage preserved
- **WHEN** a masked foliage texture is mipped
- **THEN** mip alpha SHALL be rescaled to preserve the coverage of the alpha test, so distant
  foliage does not thin out

### Requirement: Texture streaming
Textures SHALL support partial residency by mip level, driven by renderer feedback (which mips
were actually sampled) and by distance-based heuristics where feedback is unavailable.

Streaming SHALL: maintain a residency budget, prioritise by sampled mip and screen coverage,
prefetch on visibility prediction, and never block the frame — a non-resident mip SHALL fall back
to the highest resident one.

The lowest few mips SHALL always be resident so no texture is ever entirely missing.

#### Scenario: Approach a surface
- **WHEN** the camera approaches and higher mips are sampled
- **THEN** they SHALL be scheduled and swapped in when ready, with the lower mip shown meanwhile

#### Scenario: Budget pressure
- **WHEN** the residency budget is exceeded
- **THEN** the least recently sampled mips SHALL be evicted first, and the eviction reported

### Requirement: Procedural and dynamic geometry
The engine SHALL support:

- **Dynamic meshes** — vertex and index buffers updated per frame from CPU or compute, with
  ring-buffered storage
- **Immediate-mode geometry** — a builder for debug and UI-like drawing, batched per frame
- **A mesh builder** — CPU-side construction with vertex welding, normal and tangent generation,
  and LOD-friendly output

#### Scenario: Runtime-generated terrain chunk
- **WHEN** a system generates a mesh at runtime
- **THEN** it SHALL write into a dynamic mesh whose buffers are ring-buffered across frames in
  flight

### Requirement: Resource residency and lifetime
GPU resources SHALL be reference counted through their asset handles and released only after the
GPU has finished all frames that could reference them.

The engine SHALL report GPU memory by category (textures, meshes, render targets, buffers,
acceleration structures) and support a memory budget with eviction of streamable content.

#### Scenario: Level unload
- **WHEN** a level is unloaded
- **THEN** its GPU resources SHALL be released after the in-flight frames complete, and the
  reclaimed memory SHALL be reported

### Requirement: Mesh and texture diagnostics
The engine SHALL provide debug views and reports for: triangle and vertex counts per mesh and
per frame, LOD level in use, texture resolution and streaming state (a mip-level heat map),
overdraw, and vertex bandwidth per pass.

#### Scenario: Texture too large for its screen size
- **WHEN** the mip-level debug view shows a texture never sampling above mip 4
- **THEN** the artist SHALL be able to reduce its authored resolution with confidence
