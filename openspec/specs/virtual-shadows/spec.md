# virtual-shadows Specification

## Purpose

Defines **CyberShadow**: shadow maps as sparse virtual address spaces whose pages exist because a
visible pixel needs them.

That inversion is the whole efficiency argument. The conventional model allocates resolution per
light and renders whatever casts into it; here, visible pixels are projected into light space on the
GPU, the pages they touch are marked, and only those are allocated and rendered. A light
illuminating a large volume of which two square metres are visible renders two square metres of
shadow.

It also makes caching worth doing, which turns the problem from "render shadow maps" into "keep a
cache valid" — a much better problem, and one that depends on two things being right. Invalidation
must be **precise**, derived from the previous and current bounds the GPU scene already holds, so
one object moving does not dirty a light. And directional clipmaps must be **snapped to page
boundaries**, or moving the camera two centimetres invalidates everything and the system only
appears to cache.

Two further decisions carry weight. Shadow rasterisation selects geometry at a **shadow error
target** measured in shadow texels rather than screen pixels, which is available only because
geometry detail was specified as a continuous function. And shadow rasterisation **never waits on
texture residency**: materials compile a shadow program carrying only opacity and displacement, its
textures are marked shadow-critical with a guaranteed coarse representation, and a missing fine page
is substituted rather than awaited — which is how the cycle between shadows and virtual textures is
broken.

Conventional shadows are not deprecated. They remain the shipping path for constrained profiles and
the fallback where capabilities are absent.

## Requirements

### Requirement: Shadow modes
Each light SHALL declare a **shadow mode**, and the renderer profile SHALL constrain which modes are
available:

| Mode | Behaviour |
|---|---|
| `None` | No shadow |
| `Baked` | Shadowing from baked data only |
| `Conventional` | Cascades and the shadow atlas (see `rendering-lighting-and-shadows`) |
| `Virtual` | Sparse virtual shadow pages, demand-allocated and cached |
| `RayTraced` | Traced visibility through `ray-tracing-infrastructure` |
| `Hybrid` | Virtual pages with traced refinement where it matters |

Virtual shadows SHALL NOT be required. Conventional shadows remain the shipping path for constrained
profiles and the fallback where capabilities are absent, and content SHALL render correctly under
any mode with documented differences in quality and cost.

#### Scenario: Same content, different modes
- **WHEN** a scene is rendered on a high-end profile with virtual shadows and on a mobile profile
  with conventional ones
- **THEN** both SHALL render correctly, differing in shadow quality and cost

#### Scenario: Capability missing
- **WHEN** a device cannot support virtual shadows
- **THEN** the light SHALL fall back to its conventional mode with a diagnostic, not lose its shadow

### Requirement: Virtual shadow address spaces
A shadowed light SHALL own a **virtual shadow address space** far larger than the memory backing it,
subdivided into fixed-size pages resolved through a page table.

Projection kinds SHALL include: **directional clipmaps**, **spot projection**, and a **point light
projection** whose representation — cube faces, octahedral, or another mapping — SHALL be an internal
decision behind the address space abstraction, selected on measured distortion, filtering quality,
and culling cost.

Page size SHALL be a measured platform decision reported by the implementation rather than fixed by
this specification.

Virtual resolution SHALL be a per-light property, so a large area light and a small lamp do not
receive the same logical resolution.

#### Scenario: Logical resolution exceeds memory
- **WHEN** a light declares a very large virtual shadow resolution
- **THEN** only pages required by visible receivers SHALL occupy physical memory

#### Scenario: Point light mapping is internal
- **WHEN** the point light projection is changed
- **THEN** consumers of shadow lookups SHALL be unaffected

### Requirement: Receiver-driven page marking
Shadow pages SHALL be allocated because a **visible receiver needs them**, not because a caster
exists.

Marking SHALL be computed on the GPU: visible pixels from the depth or visibility buffer are
projected into each relevant light's shadow space, the pages they touch are marked, and the marks are
compacted into a unique page set.

There SHALL NOT be a CPU loop over objects and lights to determine shadow work.

Page resolution SHALL be selected from the receiver's projected shadow texel density, so that a
distant receiver marks coarser pages than a near one.

#### Scenario: Only visible geometry costs shadow work
- **WHEN** a light illuminates a large volume of which a small part is visible
- **THEN** only the pages covering visible receivers SHALL be allocated and rendered

#### Scenario: Marking is a GPU pass
- **WHEN** shadow demand is determined
- **THEN** it SHALL be computed from the depth buffer on the GPU and compacted there, with no
  per-object CPU work

### Requirement: Directional clipmaps and snapping
Directional lights SHALL use **camera-centred clipmaps**: concentric levels of roughly constant
shadow texel density covering progressively larger world extents.

Clipmap origins SHALL be **snapped to page boundaries in world space**. Sub-page camera movement
SHALL NOT move a clip level.

Without snapping, small camera movement invalidates the entire cache every frame, producing a system
that appears to cache and never does; snapping is therefore a requirement, not an optimisation.

Level count and extents SHALL be configurable, and the transition between levels SHALL not be
visible as a discontinuity in shadow resolution.

#### Scenario: Small movement changes nothing
- **WHEN** the camera moves a few centimetres
- **THEN** no clip level SHALL move and no cached page SHALL be invalidated by the movement

#### Scenario: Crossing a boundary is incremental
- **WHEN** the camera crosses a page boundary
- **THEN** a small band of new pages SHALL be requested and the remainder SHALL stay cached

### Requirement: Shadow page cache
Rendered shadow pages SHALL persist in a **physical page cache** across frames and SHALL be reused
until invalidated.

The cache SHALL be shared across lights and clip levels, budgeted from the memory budget tree, and
evicted through the shared residency policy, weighted by the cost of re-rendering a page — an
expensive static page is worth retaining longer than a cheap one.

Cache state per page SHALL include: residency, dirty status, whether it is being rendered, its age,
its update class, and whether it is pinned.

Caching SHALL be the default. Re-rendering a page that has not been invalidated SHALL be treated as
a defect.

#### Scenario: Static content is rendered once
- **WHEN** a static scene is viewed from a static camera
- **THEN** shadow pages SHALL be rendered on the first frame and reused thereafter

#### Scenario: Expensive pages persist
- **WHEN** eviction is considered
- **THEN** a page that was expensive to render SHALL be retained in preference to a cheap one of
  equal recency

### Requirement: Precise invalidation
When a caster moves, its **previous and current bounds** — already held per instance in the GPU
scene — SHALL be projected into the shadow spaces of affected lights, and only the overlapping pages
SHALL be marked dirty.

Moving one object SHALL NOT invalidate a light's whole shadow space.

Light movement SHALL invalidate that light's pages, and this SHALL be reported, since a moving
shadowed light is expensive and the cost should be attributable rather than mysterious.

World content streaming in or out SHALL invalidate only the pages its bounds project into, not the
whole cache.

Invalidation SHALL be computed on the GPU from GPU scene change data rather than through scene-graph
callbacks.

#### Scenario: One object moving is cheap
- **WHEN** a character walks across a city
- **THEN** only pages its previous and current bounds project into SHALL be dirtied

#### Scenario: A cell unloads
- **WHEN** a world cell is unloaded
- **THEN** only the shadow pages its bounds project into SHALL be invalidated

#### Scenario: A moving light is attributable
- **WHEN** a shadowed light moves
- **THEN** the resulting invalidation cost SHALL be reported against that light

### Requirement: Deformation and cache validity
Geometry whose shape changes without its transform changing — wind, vertex animation, skinning,
procedural deformation — SHALL declare a **shadow deformation mode**:

| Mode | Cache behaviour |
|---|---|
| `Static` | Never dirties by deformation |
| `Bounded` | Bounds expanded to cover the deformation envelope; dirties only when it moves |
| `Dynamic` | Dirties pages each frame it is visible in |
| `AlwaysDirty` | Never cached; rendered every frame |

Materials that displace geometry SHALL declare their maximum displacement so bounds can be expanded
rather than assuming worst-case dirtying.

`AlwaysDirty` SHALL be reportable per asset, since it is the mode that silently removes the benefit
of caching.

#### Scenario: Wind does not invalidate a forest every frame
- **WHEN** trees sway within a declared envelope
- **THEN** their bounds SHALL cover the sway and their pages SHALL not dirty each frame

#### Scenario: Uncacheable content is visible
- **WHEN** assets are marked `AlwaysDirty`
- **THEN** they SHALL be reported, with their contribution to shadow cost

### Requirement: Shadow geometry detail
Shadow rasterisation SHALL select geometry detail against a **shadow geometry error target**
expressed in shadow texels, independent of the primary visibility error target.

Because a shadow texel is typically much larger than a screen pixel, shadow rasterisation SHALL use
substantially coarser geometry than the camera view, selected from the same continuous hierarchy
(see `virtual-geometry`) rather than from a separate simplified asset.

Error targets SHALL be per class — hero, normal, background — scaled by render importance, and SHALL
be a lever available to the budget controller.

Where an asset provides a shadow proxy, it SHALL remain available as an alternative to hierarchy
selection.

#### Scenario: Shadows are cheaper than the camera view
- **WHEN** a scene is rasterised into shadow pages
- **THEN** cluster selection SHALL use the shadow error target and select coarser geometry than the
  primary view

#### Scenario: Error is a budget lever
- **WHEN** shadow cost exceeds its allocation
- **THEN** the shadow geometry error target SHALL be raised before pages are dropped

### Requirement: GPU-driven caster selection and rasterisation
Casters for dirty pages SHALL be selected entirely on the GPU: page frusta are derived from the
dirty page set, GPU scene instances are culled against them, geometry clusters are culled and
selected at the shadow error target, and rasterisation is dispatched indirectly.

There SHALL NOT be a per-light, per-object CPU draw loop.

Rasterisation SHALL be **page-batched**: many dirty pages SHALL be rendered by a small number of
dispatched workloads rather than a draw call per page.

Mesh shaders MAY be used where available, and compute culling with indirect rasterisation SHALL
remain a fully supported baseline.

#### Scenario: No CPU draw loop
- **WHEN** eight hundred pages are dirty across forty lights
- **THEN** caster selection and rasterisation SHALL be GPU-driven and indirect

#### Scenario: Pages are batched
- **WHEN** many pages are rendered in one frame
- **THEN** they SHALL be batched rather than dispatched individually

### Requirement: Shadow material programs
Materials SHALL compile a **shadow program** carrying only what shadow rasterisation needs: opacity
masking, vertex or geometry displacement, and, where supported, transmission — not the primary
program's full input set.

The shadow program SHALL be a member of the material program family alongside the primary,
secondary, and far-field programs (see `material-compiler`), derived automatically and overridable.

Shadow programs SHALL support a **distance tier**, so a distant shadow page samples a cheaper or
coarser opacity representation than a near one.

Opaque materials SHALL rasterise shadows with no fragment shader at all.

#### Scenario: Foliage shadows are cheap
- **WHEN** a fourteen-texture foliage material rasterises into a shadow page
- **THEN** it SHALL use a shadow program sampling only its opacity mask

#### Scenario: Opaque needs no shader
- **WHEN** an opaque material casts a shadow
- **THEN** rasterisation SHALL be depth-only

### Requirement: Shadow rasterisation never waits on texture residency
Shadow rasterisation SHALL NOT stall on virtual texture residency.

The textures a shadow program samples SHALL be **shadow-critical**: a coarse representation of them
SHALL be guaranteed resident, in the same way a virtual texture's mip tail is.

If a finer page is unavailable, rasterisation SHALL proceed with the resident coarse representation
and the page SHALL be marked for refresh.

This closes the cycle in which shadow rendering needs opacity, opacity may be virtualised, and
virtual texture residency is driven by visibility that depends on shadows.

#### Scenario: The cycle is broken
- **WHEN** an opacity texture's fine pages are not resident during shadow rasterisation
- **THEN** the guaranteed coarse representation SHALL be used, the frame SHALL not stall, and the
  page SHALL be refreshed subsequently

#### Scenario: Shadow-critical data is declared
- **WHEN** a material's shadow program samples a virtual texture
- **THEN** that texture SHALL be marked shadow-critical and its coarse representation pinned

### Requirement: Update classes and staleness
Dirty pages SHALL carry an **update class** — critical, dynamic, normal, or background — determining
how promptly they must be re-rendered.

A page MAY remain **stale** for a bounded number of frames according to its class and its render
importance, and the system SHALL track page age and a confidence derived from age, motion, and
contribution.

The budget controller SHALL spend its allocation on the pages where staleness would be visible,
rather than enforcing a fixed cap on pages per frame — a hard cap turns a camera cut into a stall,
while prioritised staleness degrades gracefully.

Content whose shadow must be exact — a character's contact shadow — SHALL be classified critical and
SHALL NOT be allowed to go stale.

#### Scenario: Distant foliage tolerates staleness
- **WHEN** distant foliage sways
- **THEN** its shadow pages MAY be refreshed less often, within its declared class

#### Scenario: A camera cut does not spike
- **WHEN** a cut dirties a very large number of pages
- **THEN** they SHALL be refreshed over several frames by priority, with stale pages used meanwhile,
  rather than producing a single long frame

### Requirement: Shadow budget
Virtual shadows SHALL hold an allocation from the renderer budget arbiter and distribute it across:
pages rendered per frame, shadow geometry error, page resolution selection, filtering quality, and
refresh rates by update class.

The system SHALL measure and report its own cost and SHALL NOT measure total frame time. It SHALL
declare a reserved minimum and report when it reaches it.

Reduction SHALL degrade in a declared order and SHALL be hysteretic, and pinned mode SHALL be global
with the arbiter.

#### Scenario: Cost is held
- **WHEN** shadow cost exceeds its allocation
- **THEN** geometry error, refresh rates, and filtering SHALL be adjusted in the declared order, with
  each lever reported

#### Scenario: Hero shadows survive pressure
- **WHEN** the allocation is reduced
- **THEN** high-importance receivers SHALL retain shadow quality while background shadows degrade

### Requirement: Filtering and softness
Shadow lookups SHALL support the filtering defined in `rendering-lighting-and-shadows`, adapted to
paged lookups: filter kernels SHALL account for page boundaries so that filtering never samples
across into unrelated pages.

Softness SHALL be driven by **physical source shape** — angular radius for directional lights, radius
or dimensions for local lights — rather than an arbitrary per-light softness parameter.

A **stochastic path** SHALL be available in which few samples per pixel are taken and reconstructed
through the shared denoiser (see `denoising`) and temporal framework, as an alternative to large
per-frame kernels.

Filtering quality SHALL be a budget lever.

#### Scenario: Softness follows the light
- **WHEN** a light's radius increases
- **THEN** its shadows SHALL soften accordingly, without a separate softness parameter to tune

#### Scenario: Few samples, denoised
- **WHEN** the stochastic path is used
- **THEN** samples SHALL be reconstructed through the shared denoiser as a visibility term rather
  than as colour

### Requirement: Bias derived, not tuned
Shadow bias SHALL be **derived** from the shadow texel's world-space footprint, the surface slope
relative to the light, and the geometric error of the caster representation used.

Artists SHALL be able to override the derived values, and an override SHALL be reportable, since a
scene requiring many overrides indicates a defect in the derivation.

Receiver-plane information SHALL be used where available to reduce acne without detaching contact
shadows.

#### Scenario: No per-light tuning by default
- **WHEN** a light is placed
- **THEN** its shadow bias SHALL be derived from footprint, slope, and geometric error and SHALL
  require no manual tuning in the common case

#### Scenario: Overrides are visible
- **WHEN** many lights carry bias overrides
- **THEN** this SHALL be reportable, since it indicates the derivation needs correction

### Requirement: Contact and traced refinement
Where shadow texel footprints are too coarse for contact detail, the system SHALL support
**refinement**: a short screen-space trace, or a short traced ray through
`ray-tracing-infrastructure` where available, applied selectively by importance and budget.

Refinement SHALL be an addition to the paged result, not a replacement for it, so that increasing
virtual resolution is not the only remedy for contact quality.

Selection SHALL be per pixel by importance and budget, not a global toggle.

#### Scenario: Contact detail without absurd resolution
- **WHEN** an object rests on a surface and the shadow page footprint is coarse
- **THEN** refinement SHALL supply the contact detail rather than requiring a much higher virtual
  resolution

#### Scenario: Refinement is budgeted
- **WHEN** the shadow allocation is under pressure
- **THEN** refinement SHALL be reduced before paged shadow quality is

### Requirement: Fallback chain
When a required shadow page is unavailable, the system SHALL degrade along a defined chain and SHALL
NOT stall: the requested page, then a coarser page, then a stale cached page, then a screen-space or
traced approximation, then unshadowed.

The substitution used SHALL be recorded per page and reportable, so that unexpected softness or
missing shadow is diagnosable rather than mysterious.

The frame SHALL never wait for shadow page production.

#### Scenario: Nothing stalls
- **WHEN** a page cannot be produced this frame
- **THEN** the chain SHALL supply a substitute and the frame SHALL complete

#### Scenario: Substitution is recorded
- **WHEN** a substitute is used
- **THEN** it SHALL be reportable, so a soft or missing shadow can be explained

### Requirement: Content-specific caster policy
Content classes that rasterise poorly into shadows SHALL declare a caster policy:

- **Foliage** SHALL progress from leaf geometry near the camera, through simplified clusters, to
  aggregate canopy representations at distance
- **Ground cover** SHALL cast blade-level shadows only within a configured distance, patch-level
  approximations beyond, and none at long range
- **Terrain** SHALL cast through the same geometry path as other content, with no separate terrain
  shadow renderer
- **Water** SHALL NOT cast opaque shadows by default; transmitted light and caustics are an
  illumination concern
- **Translucent** materials SHALL be staged: masked and opaque in the first tier, coloured
  transmission approximation later, higher-quality translucent shadowing after that

Policy SHALL be selected from the shadow texel footprint, so the transition is driven by measurement
rather than by authored distances.

#### Scenario: A distant forest is one caster
- **WHEN** a forest is far from the light's receivers
- **THEN** it SHALL cast from an aggregate representation rather than from millions of alpha-tested
  leaves

#### Scenario: Water does not shadow itself opaque
- **WHEN** water is present
- **THEN** it SHALL not cast an opaque shadow by default

### Requirement: Multi-view page sharing
Views that share a light — stereo eyes, split screen, a reflection probe and the main camera — SHALL
merge their shadow page requirements into one request set, and each page SHALL be rendered once and
sampled by every view that needs it.

Views SHALL contribute demand weighted by their own priority, so a minimap or an editor thumbnail
does not raise shadow quality to main-camera levels.

Where a foveation mask is supplied, page resolution selection SHALL follow it.

#### Scenario: Stereo renders each page once
- **WHEN** two eye views need the same shadow page
- **THEN** it SHALL be rendered once and sampled by both

#### Scenario: Secondary views cost less
- **WHEN** a reflection probe and the main camera both need shadows
- **THEN** the probe's demand SHALL carry lower priority and select coarser pages

### Requirement: Shadow diagnostics
The engine SHALL provide visualisations for: virtual page boundaries, page residency, dirty and
cached pages, page age, clip level, selected shadow geometry error, caster counts per page, effective
shadow resolution, filter radius, refinement usage, and invalidation sources.

Selecting a page SHALL report: its light, clip level, virtual and physical page, state, **why it is
dirty** — naming the instance, light movement, streaming event, or deformation responsible — its
caster count, and its last render cost.

The profiler SHALL report per frame: pages resident, requested, dirty, and rendered; the cost of page
marking, caster culling, rasterisation, and filtering; and the shadow allocation against measured
cost.

#### Scenario: Why is this page dirty
- **WHEN** a page is re-rendered unexpectedly often
- **THEN** the diagnostics SHALL name what invalidated it

#### Scenario: Shadow cost is attributable
- **WHEN** shadow cost is high
- **THEN** the profiler SHALL attribute it to lights, pages, and caster content
