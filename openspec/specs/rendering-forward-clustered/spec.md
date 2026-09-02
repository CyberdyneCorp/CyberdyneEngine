# rendering-forward-clustered Specification

## Purpose

Defines the default desktop pipeline: a **clustered forward** renderer with a depth prepass,
GPU light/decal/probe assignment, sorted draw submission, and a documented pass order.

Forward+ is chosen over deferred because it handles transparency, MSAA, and varied shading models
without a fat G-buffer, and because clustered light assignment removes forward rendering's
historical light-count limit.

## Requirements

### Requirement: Cluster grid
The renderer SHALL partition the view frustum into a 3D grid of **clusters**: a 2D screen tiling
(default 32×32 pixel tiles) subdivided into depth slices (default 32) distributed
**exponentially** in view depth, so slices are thin near the camera and thick far away.

Depth slice index SHALL be computed as
`slice = floor(log(z / near) * slice_count / log(far / near))`, with the mapping constants
supplied in a uniform buffer so the shader computes it with two instructions.

#### Scenario: Exponential slicing matches depth distribution
- **WHEN** clusters are assigned
- **THEN** slice thickness SHALL grow with distance, matching the distribution of geometry and
  the precision of reversed-Z depth

#### Scenario: Cluster dimensions adapt to resolution
- **WHEN** the view resolution changes
- **THEN** the cluster grid SHALL be resized so tile size in pixels stays constant, and buffers
  SHALL be reallocated only when the count changes

### Requirement: Light and volume assignment
Assignment SHALL run as a **compute pass** producing, per cluster, a compact list of the lights,
decals, and reflection probes affecting it.

The pass SHALL:
1. Compute each cluster's view-space AABB from its tile bounds and depth slice
2. For each volume element, test it against cluster AABBs it could overlap, using a conservative
   bounding volume per element type — sphere for point lights and wide spot lights, cone for
   narrow spot lights, oriented box for decals, reflection probes, and area lights
3. Append matching element indices into a per-cluster range in a global index buffer, allocated
   with an atomic counter
4. Write a per-cluster, per-type `(offset, count)` header

Spot lights whose half-angle exceeds a threshold (**60°**) SHALL be treated as spheres, because a
cone bound becomes both loose and, past 90°, incorrect.

Element counts per type per cluster SHALL be bounded, with overflow reported in diagnostics
rather than silently corrupting the list.

#### Scenario: Fragment finds its lights
- **WHEN** the fragment shader shades a pixel
- **THEN** it SHALL compute its cluster from screen position and view depth, read the
  `(offset, count)` header for each element type, and iterate only that range

#### Scenario: Camera inside a light
- **WHEN** the camera is inside a point light's radius
- **THEN** the light SHALL be assigned from the nearest depth slice, since its bounding volume
  would otherwise be clipped by the near plane

#### Scenario: Cluster overflow
- **WHEN** more elements affect a cluster than the per-cluster limit
- **THEN** the excess SHALL be dropped deterministically (nearest kept) and an overflow counter
  SHALL be reported in render statistics

### Requirement: Depth prepass
The pipeline SHALL render a depth prepass before shading, with a mode selected from what later
passes require:

| Mode | Outputs | Selected when |
|---|---|---|
| `DepthOnly` | Depth | Nothing else needs prepass data |
| `DepthNormal` | Depth, packed normal + roughness | SSAO, SSR, screen-space GI, or a custom pass requests it |
| `DepthNormalVelocity` | Depth, normal + roughness, motion vectors | TAA, temporal upscaling, or motion blur is enabled |

Normals SHALL be stored octahedron-encoded in two 16-bit channels; roughness SHALL share the
remaining channels.

Alpha-tested geometry SHALL participate in the prepass at a documented alpha threshold; fully
transparent geometry SHALL NOT.

#### Scenario: Prepass mode follows requirements
- **WHEN** SSAO is enabled and TAA is not
- **THEN** the prepass SHALL run in `DepthNormal` mode, and no motion vector target SHALL be
  allocated

#### Scenario: Prepass depth is reused
- **WHEN** the opaque pass runs
- **THEN** it SHALL test depth with `Equal` against the prepass result and SHALL NOT write depth,
  eliminating overdraw in shading

### Requirement: Draw sorting
Each draw item SHALL carry a 64-bit **sort key**. For opaque draws the key SHALL be composed,
from most to least significant:

```
[ layer : 4 ][ priority : 8 ][ pipeline : 20 ][ material : 20 ][ mesh : 12 ]
```

so sorting groups by pipeline, then material, then mesh, minimising state changes.

For transparent draws the key SHALL be:

```
[ layer : 4 ][ priority : 8 ][ inverse depth : 24 ][ pipeline : 16 ][ material : 12 ]
```

so back-to-front ordering dominates and state grouping breaks ties.

Sorting SHALL use a radix sort over the packed keys.

#### Scenario: State changes are minimised
- **WHEN** the opaque list is sorted and submitted
- **THEN** all draws sharing a pipeline SHALL be adjacent, and within them all sharing a material

#### Scenario: Transparency ordering is correct
- **WHEN** transparent surfaces overlap
- **THEN** they SHALL be drawn back to front by depth, with priority overriding depth where set

### Requirement: Instance data
Per-draw data SHALL live in a large GPU storage buffer indexed by an instance index supplied
through the draw call, containing: the world transform (3×4), the previous world transform for
motion vectors, a compact bounds representation, the material index, per-instance parameter
offsets, a layer mask, LOD and fade factors, and GI/lightmap addressing.

The buffer SHALL be written once per frame during Prepare, grown geometrically, and mapped
directly on devices with host-visible device-local memory.

#### Scenario: Material change does not rebind
- **WHEN** bindless is available
- **THEN** the material index in instance data SHALL select material data and textures from
  global arrays, so consecutive draws with different materials need no descriptor rebinding

#### Scenario: Motion vectors need the previous transform
- **WHEN** an instance is newly created
- **THEN** its previous transform SHALL be initialised to its current one, producing zero
  velocity rather than a spurious streak

### Requirement: Pass order
A frame for one view SHALL execute in this order:

1. **Prepare** — instance, light, decal, probe, and material buffers; skinning; particle
   simulation
2. **Depth prepass** (mode per above), plus MSAA depth resolve if required
3. **Cluster assignment** compute pass
4. **Screen-space passes needing depth/normals** — ambient occlusion, screen-space GI
5. **Opaque pass** — clustered forward shading into HDR colour
6. **Sky** pass
7. **Screen-space reflections**, if enabled
8. **Transparent pass** — back-to-front, reading a copy of opaque colour where refraction is used
9. **Resolve** — MSAA resolve of colour and depth
10. **Temporal** — TAA or temporal upscaling
11. **Post-process chain** — see `rendering-post-processing`
12. **UI and debug** — see `ui-system`, drawn after tonemapping unless the UI opts into HDR
13. **Present**

Custom passes SHALL be inserted at the extension points defined in `rendering-architecture`.

#### Scenario: Transparent refraction
- **WHEN** a transparent material samples scene colour
- **THEN** a copy of the opaque result SHALL be made before the transparent pass, and the graph
  SHALL synchronise it

#### Scenario: Pipeline adapts to disabled features
- **WHEN** ambient occlusion, SSR, and TAA are all disabled
- **THEN** their passes SHALL be absent from the graph and their targets unallocated

### Requirement: Shading model dispatch
The forward shader SHALL support multiple shading models selected per material through a
specialization constant or a compact branch: `Lit` (standard PBR), `Unlit`,
`SubsurfaceScattering`, `ClearCoat`, `Anisotropic`, `Cloth`, `Hair`, and `Foliage` (two-sided
with translucency).

Models SHALL share the same light iteration loop; only the BRDF evaluation differs.

#### Scenario: Uncommon model does not cost the common one
- **WHEN** a view contains no subsurface materials
- **THEN** the specialised pipeline for `Lit` SHALL contain no subsurface code

### Requirement: MSAA and alpha coverage
The pipeline SHALL support MSAA at 2×, 4×, and 8× for the opaque and transparent passes, with
depth and colour resolved before post-processing.

Alpha-tested materials SHALL support **alpha-to-coverage** so foliage antialiases correctly under
MSAA.

#### Scenario: MSAA with screen-space effects
- **WHEN** MSAA and SSAO are both enabled
- **THEN** depth and normals SHALL be resolved before the screen-space passes, which operate at
  single-sample resolution

### Requirement: Multi-view rendering
The pipeline SHALL support rendering multiple views in a single pass using layered render targets
and view indices, for stereo XR and cubemap capture.

Per-view matrices SHALL be indexed by view index in the shader; geometry SHALL be submitted once.

#### Scenario: Stereo in one pass
- **WHEN** an XR view requests two sub-views
- **THEN** geometry SHALL be submitted once and amplified to both layers

### Requirement: Mobile pipeline differences
The Mobile pipeline SHALL share the renderer's structure but:

- omit the depth prepass by default (bandwidth over overdraw on tile GPUs)
- use per-object light lists rather than a cluster grid, bounded per object
- run tonemapping as a **subpass** so HDR colour never leaves tile memory
- omit screen-space reflections, screen-space GI, and subsurface scattering
- prefer memoryless attachments for depth and MSAA targets

#### Scenario: Tile memory is respected
- **WHEN** the mobile pipeline runs on a tiled GPU
- **THEN** the HDR colour attachment SHALL be declared memoryless and resolved to the swap chain
  within the same render pass

### Requirement: Pipeline diagnostics
The renderer SHALL report per frame: cluster occupancy statistics (average and maximum elements
per cluster, overflow count), draw calls and triangles per pass, sort key distribution, pipeline
cache hits and misses, and per-pass GPU time.

#### Scenario: Cluster overflow is visible
- **WHEN** a scene places many lights in one region
- **THEN** the overflow counter SHALL rise and the cluster occupancy debug view SHALL show the
  affected region
