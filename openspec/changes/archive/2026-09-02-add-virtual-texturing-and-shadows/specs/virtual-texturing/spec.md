## ADDED Requirements

### Requirement: Residency models
Every texture SHALL declare a **residency model**, and the choice SHALL NOT be visible in the public
handle:

| Model | Used for |
|---|---|
| `Resident` | Small assets: icons, lookup tables, small masks |
| `StreamedMip` | Conventional mip streaming; ordinary assets and the compatibility path |
| `VirtualStreamed` | Pages cooked offline: terrain, large environments, UDIM sets, lightmaps |
| `VirtualRuntime` | Pages produced at runtime: terrain composition, decals, world state, procedural surfaces |

Gameplay and material authoring SHALL use one texture handle type. The material compiler SHALL
select the sampling path from the asset's model.

`StreamedMip` SHALL be retained as a first-class model, not a legacy path: conventional streaming is
simpler and cheaper for ordinary assets, and virtualising every texture would be a regression for
most of a project's content.

#### Scenario: One handle
- **WHEN** gameplay loads a texture
- **THEN** it SHALL receive a texture handle regardless of residency model, and no model-specific
  API SHALL be required

#### Scenario: The model is a cook decision
- **WHEN** a texture's residency model changes
- **THEN** materials referencing it SHALL recompile if their sampling path changes, and no authored
  content SHALL need editing

### Requirement: Virtual address space
A virtual texture SHALL define a **virtual address space** far larger than the physical memory
backing it, addressed by virtual texture identity, mip level, tile coordinates, and layer.

The asset SHALL declare: virtual dimensions, tile size, border size, mip count, format, page index,
a fallback representation, and the semantic of its data.

Physical cache coordinates SHALL NOT appear in the asset or in any authored content.

The address encoding SHALL be versioned so that a future volumetric address space can be added
without invalidating existing content.

#### Scenario: Logical size exceeds memory
- **WHEN** a terrain material's virtual texture is larger than GPU memory
- **THEN** it SHALL be addressable in full while only the resident pages occupy memory

#### Scenario: Physical layout is private
- **WHEN** an asset is cooked
- **THEN** it SHALL contain no reference to physical cache placement

### Requirement: Page tables
Virtual pages SHALL be resolved to physical tiles through a **page table** readable from shaders.

A page table entry SHALL carry at minimum: the physical tile, the resident mip actually available,
state flags (resident, pending, pinned, fallback, invalid, runtime-produced), and a generation for
validation.

The page table implementation — flat or hierarchical — SHALL be an internal decision hidden behind
the lookup, chosen by address space size, since a flat table is faster for small spaces and
untenable for large ones.

Page table updates SHALL be applied on the GPU without a CPU round trip per page.

#### Scenario: Lookup is uniform
- **WHEN** a shader samples a virtual texture
- **THEN** the lookup SHALL be the same regardless of the page table's internal structure

#### Scenario: Updates do not stall
- **WHEN** many pages become resident in a frame
- **THEN** their page table entries SHALL be updated on the GPU without per-page CPU work

### Requirement: Tiles and borders
Physical tiles SHALL carry **borders** of texels replicated from neighbouring pages, sized so that
bilinear, trilinear, and anisotropic filtering never sample across a tile edge into unrelated data.

Border size SHALL be part of the cooked format and SHALL account for the maximum filtering width the
sampling path may use.

Tile size SHALL be a cooker and platform policy, chosen against compression block sizes, hardware
sparse residency granularity where used, and measured cache behaviour — and SHALL be reported rather
than fixed as a constant in this specification.

#### Scenario: No seams between tiles
- **WHEN** anisotropic filtering samples near a tile boundary
- **THEN** border texels SHALL provide the neighbouring data and no seam SHALL appear

#### Scenario: Tile size is a measured decision
- **WHEN** tile size is selected
- **THEN** the choice SHALL be justified by measurement and recorded, not fixed by assumption

### Requirement: Physical tile cache
Physical tiles SHALL live in **shared caches grouped by format class** — block-compressed colour,
two-channel normal, single-channel mask, high dynamic range — rather than one physical cache per
asset.

Cache capacity SHALL be a budget from the memory budget tree, and eviction SHALL follow the shared
residency policy.

Many virtual textures SHALL share one cache, so that a project with thousands of virtual textures
does not fragment its memory across thousands of allocations.

#### Scenario: Caches are shared
- **WHEN** a thousand virtual textures of the same format class are in use
- **THEN** they SHALL share one physical cache

#### Scenario: Format classes are separate
- **WHEN** colour and normal data are cached
- **THEN** they SHALL occupy caches of their respective formats rather than a common uncompressed
  form

### Requirement: Resident mip tail
The coarsest mip levels of every virtual texture — its **mip tail** — SHALL be permanently resident,
so that a surface is never missing, only blurry.

When a requested page is not resident, sampling SHALL walk to the nearest resident coarser level and
sample that, and SHALL record the deficit.

A sampling path SHALL NOT produce a placeholder pattern, a black texel, or a stall for a
non-resident page in a shipping build. Debug visualisation of missing pages SHALL be a development
mode.

#### Scenario: Never missing, only coarse
- **WHEN** fine pages have not yet arrived
- **THEN** the surface SHALL render from the resident coarser level

#### Scenario: The deficit is recorded
- **WHEN** a coarser level is substituted
- **THEN** the difference between desired and resident level SHALL be recorded for diagnostics and
  for request priority

### Requirement: GPU feedback
Page requests SHALL be produced by **GPU feedback**: shaders record the virtual pages they would
have sampled, and the results are deduplicated, compacted, and prioritised on the GPU before any
CPU involvement.

A per-pixel request stream SHALL NOT reach the CPU.

Feedback **density** SHALL be adjustable — one sample per pixel block rather than per pixel — and
SHALL be a quality lever driven by camera motion, texture pressure, resolution, and budget.

Feedback SHALL be produced from the pass where material inputs are evaluated, and SHALL work in both
the Forward+ and visibility buffer pipelines.

#### Scenario: Requests are compacted on the GPU
- **WHEN** a million pixels sample one page
- **THEN** one request SHALL reach the residency scheduler

#### Scenario: Density adapts
- **WHEN** texture pressure is high
- **THEN** feedback density MAY be reduced, with the effect on accuracy reported

### Requirement: Predictive prefetch
Virtual texture residency SHALL accept **prediction hints** through the residency layer: world cell
prefetch, camera motion, cinematic camera tracks, teleport destinations, and network preloading.

Prediction SHALL request **coarse pages ahead of need**; GPU feedback SHALL refine to the exact
pages sampled.

The division SHALL be explicit: prediction covers latency, feedback establishes accuracy. Neither
SHALL be relied on for the other's role.

#### Scenario: Content is warm on arrival
- **WHEN** the world predicts arrival at a region
- **THEN** coarse pages for its materials SHALL be requested ahead of time, so the first frame is
  blurry at worst rather than missing

#### Scenario: Feedback corrects prediction
- **WHEN** prediction requested pages that were not sampled
- **THEN** they SHALL age out normally, and the miss SHALL be reported as prediction accuracy

### Requirement: Runtime producers
Virtual texture pages SHALL be producible at runtime through a **producer interface**, invoked by
the residency system when a page is required.

The engine SHALL provide producers for at minimum: cooked disk tiles, terrain material composition,
decal accumulation, world-state data, and procedural generation. Projects and plugins SHALL be able
to register their own.

A producer SHALL render or compute into the physical cache through the render graph, SHALL declare
its cost, and SHALL be budgeted like any other GPU work.

Runtime production SHALL be **cached**: a produced page persists until evicted or invalidated, so an
expensive composition is evaluated once rather than per frame.

#### Scenario: Expensive terrain material is evaluated once
- **WHEN** a terrain material graph composes many layers
- **THEN** it SHALL be evaluated when a page is produced and sampled cheaply thereafter, rather than
  per pixel per frame

#### Scenario: Custom producer
- **WHEN** a project registers a producer
- **THEN** its pages SHALL be requested, budgeted, cached, evicted, and diagnosed like any other

### Requirement: Runtime page invalidation and persistence
A producer SHALL be able to **invalidate** pages when its inputs change — terrain deformed, a decal
applied, world state altered — so only affected pages are re-produced.

Runtime page content SHALL declare its persistence class: transient, derived (regenerable and
therefore not saved), save-game persistent, or replicated.

Persistent runtime pages SHALL be stored as a **delta over the cooked base**, recorded in the world
persistence overlay, so that a terraformed region saves the changed pages rather than a new texture.

#### Scenario: Terraforming persists
- **WHEN** players permanently alter a region's surface
- **THEN** the changed pages SHALL be recorded as a delta in the persistence overlay and restored on
  load

#### Scenario: Derived pages are not saved
- **WHEN** wetness pages are produced from a field
- **THEN** they SHALL be classified derived and regenerated rather than written to the save

### Requirement: Texture semantics and mip generation
Every texture SHALL declare a **semantic** — colour, normal, mask, height, data, high dynamic range,
or user interface — which SHALL drive compression format, colour space, filtering defaults, cache
class, and mip generation.

Mip generation SHALL be **semantic-aware**: normals filtered as vectors and renormalised, masks
filtered to preserve coverage, alpha-tested textures preserving alpha coverage, height data filtered
by its declared reduction, and colour filtered in the correct space.

Semantically incorrect mip generation SHALL be treated as a defect rather than a tuning issue,
because it is magnified by virtualisation: a wrong coarse mip is what a distant surface actually
shows.

#### Scenario: Normals stay normalised
- **WHEN** a normal map is mipped
- **THEN** vectors SHALL be filtered and renormalised rather than averaged as colour

#### Scenario: Distant foliage does not thin
- **WHEN** an alpha-tested texture is mipped
- **THEN** alpha SHALL be rescaled to preserve coverage

### Requirement: Compression and page storage
Cooked virtual texture pages SHALL be stored **individually compressed and content-addressed**, so
that pages deduplicate across assets and patches transfer only changed pages.

Pages SHALL be stored in a **GPU-ready block-compressed format** where the platform supports it, so
that loading is decompress-container-then-upload rather than decode-to-uncompressed-then-compress.

The path SHALL be specified so that **GPU decompression** and direct storage-to-GPU transfer can be
adopted without changing the page format or the request path.

#### Scenario: A patch transfers changed pages
- **WHEN** one region's textures change
- **THEN** only the affected content-addressed pages SHALL appear in the patch

#### Scenario: No uncompressed round trip
- **WHEN** a page is loaded
- **THEN** it SHALL be uploaded in its block-compressed form rather than decoded to uncompressed
  pixels first

### Requirement: UDIM and multi-layer assets
The cooker SHALL combine **UDIM tile sets** authored as separate images into one virtual address
space, so that runtime and materials are unaware of the authoring file structure.

A virtual texture MAY carry **multiple layers** sampled together — colour, normal, and roughness for
one surface — packed so that one page request services all layers of a surface region rather than
issuing independent requests that arrive at different times.

#### Scenario: UDIM authoring is invisible at runtime
- **WHEN** a character is authored across sixteen UDIM tiles
- **THEN** it SHALL cook to one virtual texture and materials SHALL sample it with ordinary
  coordinates

#### Scenario: Layers arrive together
- **WHEN** a surface's colour and normal are in one multi-layer virtual texture
- **THEN** one page request SHALL make both resident, so the surface never shows new colour with
  stale normals

### Requirement: Virtual lightmaps
Baked lighting (see `rendering-global-illumination`) SHALL be storable as virtual textures, so that
only visible lightmap pages are resident.

Virtual lightmaps SHALL use the same address space, feedback, residency, and fallback machinery as
other virtual textures, with a semantic appropriate to their encoding.

#### Scenario: Large baked levels are affordable
- **WHEN** a large level's lightmaps exceed memory
- **THEN** only visible pages SHALL be resident, with the mip tail guaranteeing coverage

### Requirement: Sampling and derivatives
Material authoring SHALL sample textures uniformly; the material compiler SHALL emit a virtual or
resident sampling path according to the asset's residency model, without artist-facing distinction.

Virtual sampling requires correct texture-space derivatives for mip selection and anisotropy. Under
a **visibility buffer** pipeline, where shading is decoupled from rasterisation, derivatives SHALL be
**reconstructed analytically** from the identified instance and primitive rather than taken from
screen-space quad derivatives.

Incorrect derivative reconstruction SHALL be treated as a renderer defect: it manifests as
whole-frame shimmering or blurring and is easily misattributed to the texture system.

The engine SHALL provide a diagnostic visualising selected mip level and anisotropy so derivative
errors are directly observable.

#### Scenario: Same authoring, either path
- **WHEN** a material samples a texture
- **THEN** the same authored expression SHALL compile to a resident or virtual sampling path
  according to the asset

#### Scenario: Correct mips under the visibility buffer
- **WHEN** shading occurs in a material resolve pass
- **THEN** derivatives SHALL be reconstructed from the primitive, and mip selection SHALL match the
  forward path within a declared tolerance

#### Scenario: Mip selection is inspectable
- **WHEN** shimmering is observed
- **THEN** the mip and anisotropy visualisation SHALL show whether derivative reconstruction is the
  cause

### Requirement: Virtual texture diagnostics
The engine SHALL provide visualisations for: virtual page boundaries, selected mip level, physical
cache occupancy, page age, requested pages, non-resident pages, fallback substitution, feedback
density, request priority, page source (disk or producer), runtime page updates, and churn.

Selecting a pixel SHALL report: the material and texture sampled, the virtual page requested, the
resident page used, its age and source, the requesting view, and its priority.

The profiler SHALL report: physical cache size and utilisation, requests and pages loaded per frame,
fallback sampling rate, cache hit rate, production cost for runtime pages, and churn.

#### Scenario: Why is this surface blurry
- **WHEN** a surface renders coarser than expected
- **THEN** the diagnostic SHALL report the desired and resident mip and the reason for the deficit

#### Scenario: Thrashing is visible
- **WHEN** pages are repeatedly evicted and re-requested
- **THEN** churn SHALL be reported rather than appearing only as an unexplained hitch
