# virtual-geometry Specification

## Purpose

Defines **CyberGeometry**: virtualised geometry, where a mesh is a hierarchy of small triangle
clusters streamed on demand and selected per cluster so that rendering cost tracks the pixels on
screen rather than the triangles in the asset.

This is where the GPU scene, the render graph, the hierarchical depth buffer, indirect execution
and content-addressed assets were always heading. Artists import source-quality meshes and stop
authoring LOD chains; the engine builds the hierarchy at cook time and picks detail on the GPU
every frame.

The decisions that matter are recorded as requirements rather than left to the implementation.
Detail is a **continuous function of screen-space error**, not a set of discrete levels. Clusters
are **grouped and simplified together** so boundaries stay watertight, which is the whole reason
cracks do not appear at transitions. The streaming unit is the **page**, not the cluster, and a
small root region is **always resident** so an object is never absent — only coarse. A
**visibility buffer** pipeline ships alongside Forward+ rather than replacing it, because each
wins on different content.

Render geometry is explicitly **not** collision geometry: physics, navigation and ray tracing
consume separate representations derived from the same source. Cost is bounded by a geometry
budget with per-object importance, the same shape of controller used by VFX, audio, AI and
animation. Deformation is staged: static first, skinned last, with the growth path written down.

## Requirements

### Requirement: Engine-owned virtual geometry
The virtual geometry system SHALL be engine code: the asset representation, the cluster hierarchy
and its error metric, the page format, GPU traversal and culling, the residency manager, the
rasterisation paths, and the diagnostics.

Bounded algorithms MAY be integrated — mesh simplification and cluster generation — behind
engine-owned interfaces, with the hierarchy, error metric, and page format remaining engine-owned.

The system SHALL be removable at build time via `CY_VIRTUAL_GEOMETRY`, and every asset SHALL remain
renderable through the traditional path when it is absent.

#### Scenario: Owned format, integrated algorithms
- **WHEN** simplification is implemented
- **THEN** the simplifier MAY be a third-party library, and the cluster grouping, error metric, and
  page layout SHALL be engine-owned

#### Scenario: Disabled at build time
- **WHEN** `CY_VIRTUAL_GEOMETRY` is disabled
- **THEN** assets SHALL render through their fallback representation with no content changes

### Requirement: Virtual geometry asset
A mesh MAY be cooked into a **virtual geometry asset** containing: bounds, the cluster hierarchy
description, a page table, material ranges, an always-resident root region, a set of streamable
pages, and a reference to a **fallback mesh**.

Most geometry data SHALL NOT be permanently resident: only the root region is, with the remainder
streamed on demand.

The asset SHALL declare its **deformation class** — static, rigid instanced, terrain,
destructible, or skinned — so the runtime knows which paths apply.

Assets SHALL be cooked once and used by any number of instances; cluster data SHALL exist once per
asset regardless of instance count.

#### Scenario: Large source mesh
- **WHEN** a 30-million-triangle mesh is imported with virtual geometry enabled
- **THEN** it SHALL cook into a cluster hierarchy with a small resident root and streamable pages,
  with the cooked size, resident size, and cluster count reported

#### Scenario: Instances share geometry
- **WHEN** two million instances reference one asset
- **THEN** one hierarchy and one set of pages SHALL exist, with instances differing only in
  transform and material parameters

#### Scenario: Every asset has a fallback
- **WHEN** an asset is cooked
- **THEN** it SHALL carry or reference a fallback mesh usable by the traditional path

### Requirement: Clusters
Geometry SHALL be partitioned into **clusters**: small, independently cullable groups of triangles
with their own vertex and index data.

Cluster size SHALL be a **cooker policy** with configurable minimum, target, and maximum triangle
counts and a maximum vertex count — not a constant baked into the format — so it can be tuned per
platform and evolved without a format change.

Each cluster SHALL carry at minimum: bounds, a **normal cone** (average normal and maximum
deviation), a geometric error, its material range, hierarchy links, and its page.

Cluster metadata SHALL be compactly encoded; the per-cluster metadata cost SHALL be reported, since
it is paid for every cluster in every asset.

#### Scenario: Cluster size is tunable
- **WHEN** a different target cluster size is benchmarked
- **THEN** it SHALL be changeable as a cooker setting, and existing assets SHALL be recookable
  without a format version change

#### Scenario: Metadata cost is visible
- **WHEN** an asset is cooked
- **THEN** the per-cluster metadata size and its total SHALL be reported alongside the geometry size

### Requirement: Cluster hierarchy and grouping
Clusters SHALL be organised into a **hierarchy** built by iterative simplification: neighbouring
clusters are collected into **groups**, each group is simplified as a unit, and the result becomes
a smaller set of parent clusters.

Simplification SHALL be performed **per group, not per cluster**, with shared boundary vertices
constrained, so that a parent representation joins its neighbours without cracks.

Group membership SHALL be re-partitioned between levels rather than nested rigidly, so that
boundaries do not accumulate across the hierarchy and constrain simplification indefinitely.

The hierarchy SHALL be **crack-free at every level and at every transition between levels**: a
frame that renders some clusters at one level and neighbours at another SHALL produce no holes.

#### Scenario: No cracks at a transition
- **WHEN** neighbouring regions of one mesh render at different hierarchy levels in the same frame
- **THEN** their shared boundary SHALL be watertight

#### Scenario: Boundaries do not accumulate
- **WHEN** the hierarchy is built through many levels
- **THEN** group re-partitioning SHALL prevent boundary constraints from progressively preventing
  simplification

#### Scenario: Watertightness is tested
- **WHEN** an asset is cooked
- **THEN** an automated check SHALL verify that adjacent clusters across levels share consistent
  boundaries

### Requirement: Geometric error and detail selection
Every hierarchy node SHALL carry a **geometric error**: a bound on how far its representation
deviates from the source geometry.

At runtime the error SHALL be converted to a **screen-space error** from the node's bounds, the
view projection, and the distance to the camera, and compared against a threshold. A node whose
screen error is within threshold SHALL be rendered; otherwise traversal SHALL descend to its
children.

Quality SHALL be expressed as a **maximum acceptable geometric error in pixels**, with named
presets, rather than as per-asset distances.

Selection SHALL be **view-dependent and per cluster**: different regions of one mesh MAY render at
different levels in the same frame.

Shadow, reflection, and other secondary views SHALL apply their own threshold scaling, so detail is
not spent where it is not visible.

#### Scenario: Detail follows distance continuously
- **WHEN** the camera approaches an object
- **THEN** progressively finer clusters SHALL be selected without a discrete LOD switch or a visible
  pop

#### Scenario: One mesh, several levels
- **WHEN** a large object spans a range of distances
- **THEN** its near regions SHALL render at finer levels than its far regions, in the same frame

#### Scenario: Quality is a meaningful unit
- **WHEN** a project sets a 1-pixel error target
- **THEN** it SHALL apply consistently across every virtual geometry asset without per-asset tuning

#### Scenario: Shadows use coarser geometry
- **WHEN** rendering a shadow map
- **THEN** the threshold SHALL be scaled so coarser clusters are selected than for the main view

### Requirement: Geometry pages
Clusters SHALL be packed into **pages**: the unit of streaming, compression, caching, content
addressing, and eviction.

Page size SHALL be configurable and benchmarked rather than fixed; the specification requires it to
be a policy, and the chosen size and its rationale SHALL be recorded.

Each page SHALL be independently: hashable for content addressing, compressible, streamable,
cacheable, and patchable — so it composes with the asset system's content-addressed store and patch
mechanism.

Pages SHALL be **content-addressed**, so identical geometry data shared between assets can be
stored once and a patch transfers only changed pages.

#### Scenario: Page is the I/O unit
- **WHEN** geometry is streamed
- **THEN** whole pages SHALL be read, not individual clusters

#### Scenario: Shared data is stored once
- **WHEN** two assets contain identical geometry pages
- **THEN** their content hashes SHALL match and the store MAY hold one copy

#### Scenario: Patching transfers changed pages
- **WHEN** an asset is modified and patched
- **THEN** only pages whose content changed SHALL be transferred

### Requirement: Always-resident root
Every virtual geometry asset SHALL keep a small **always-resident** region: the hierarchy root and
the coarsest clusters, sufficient to render the object recognisably.

When a required page is not resident, rendering SHALL use the **nearest resident ancestor** rather
than omitting the object.

An object SHALL never fail to render because streaming has not completed.

The resident cost per asset SHALL be reported at cook time and budgeted in aggregate.

#### Scenario: Streaming has not caught up
- **WHEN** the camera moves rapidly toward an object whose fine pages are not resident
- **THEN** it SHALL render at the coarsest resident level and refine as pages arrive, never
  disappearing

#### Scenario: Resident budget is visible
- **WHEN** a project's assets are cooked
- **THEN** the total always-resident geometry footprint SHALL be reported

### Requirement: GPU page table and residency
The GPU SHALL address geometry through a **page table** mapping virtual page identifiers to
locations in a **GPU geometry cache** of fixed budgeted size.

Page table entries SHALL carry at minimum a physical location, a generation counter, and state
flags (resident, requested, loading, pinned, invalid), so that a stale reference is detected rather
than reading recycled memory.

The cache SHALL be a **single shared allocation**, not a per-asset buffer, managed by a residency
manager with a configured budget.

Eviction SHALL be scored, not least-recently-used alone: visibility, projected area, recent usage,
object importance, and predicted usage SHALL contribute.

#### Scenario: One cache, many assets
- **WHEN** many assets are visible
- **THEN** their pages SHALL occupy one shared cache within its budget, not per-asset allocations

#### Scenario: Stale reference is detected
- **WHEN** a page is evicted and its slot reused
- **THEN** the generation counter SHALL cause the old reference to miss rather than read the new
  page

#### Scenario: Eviction is scored
- **WHEN** the cache is full and a new page is needed
- **THEN** the page with the lowest expected value SHALL be evicted, not simply the least recently
  used

### Requirement: GPU-driven streaming feedback
Page requests SHALL originate on the **GPU**, which knows which pages traversal actually needed.

During traversal, encountering a non-resident page SHALL append a request to a GPU buffer. Requests
SHALL be compacted and deduplicated on the GPU before the CPU reads them.

The request path SHALL be **asynchronous**: the CPU SHALL read the previous frame's request buffer
without a synchronising readback, and pages SHALL become resident some frames later, with the
resident-root fallback covering the interval.

The system SHALL additionally support **predictive streaming**, prefetching from camera position
and velocity, world streaming state, object importance, and anticipated visibility, rather than
being purely reactive.

#### Scenario: GPU asks, CPU services
- **WHEN** traversal needs a non-resident page
- **THEN** a request SHALL be recorded on the GPU, deduplicated, and serviced without a
  synchronising readback

#### Scenario: Fast camera motion
- **WHEN** an RTS camera pans rapidly across a map
- **THEN** predictive prefetching SHALL reduce how often the resident-root fallback is visible,
  and reactive requests SHALL still cover what prediction missed

#### Scenario: Requests are bounded
- **WHEN** more pages are requested than the streaming budget allows
- **THEN** requests SHALL be prioritised and the remainder deferred, with the shortfall reported

### Requirement: Instance culling
Before any hierarchy traversal, whole instances SHALL be culled on the GPU against: frustum,
projected screen size, HZB occlusion, and layer or visibility masks.

Instance culling SHALL read the **GPU scene**, so virtual geometry does not traverse ECS entities
or maintain its own instance list.

#### Scenario: Objects that cannot matter are removed first
- **WHEN** millions of instances exist and a small fraction are visible
- **THEN** instance culling SHALL reduce the set before any cluster work is performed

#### Scenario: GPU scene is the source
- **WHEN** virtual geometry renders
- **THEN** it SHALL consume GPU scene instance records, with no CPU per-instance submission

### Requirement: GPU hierarchy traversal and cluster culling
For surviving instances, hierarchy traversal and cluster selection SHALL run **entirely on the
GPU**, producing a list of clusters to rasterise.

Traversal SHALL descend from the root, testing each node's screen-space error, and SHALL prune
subtrees that fail visibility tests before descending into them.

Candidate clusters SHALL then be culled by: frustum, **normal cone** backface rejection, screen
size, and HZB occlusion.

The CPU SHALL NOT traverse the hierarchy, and SHALL NOT issue per-cluster or per-instance draw
calls. Work SHALL be dispatched indirectly from GPU-generated arguments.

#### Scenario: No CPU draw loop
- **WHEN** hundreds of thousands of clusters are visible
- **THEN** the CPU SHALL submit a small, bounded number of passes with indirect arguments

#### Scenario: Normal cone rejects cheaply
- **WHEN** a cluster's normal cone faces entirely away from the camera
- **THEN** it SHALL be rejected without testing individual triangles

#### Scenario: Pruning before descending
- **WHEN** a hierarchy node is entirely outside the frustum or occluded
- **THEN** its subtree SHALL not be traversed

### Requirement: Occlusion culling for clusters
Cluster occlusion SHALL use the renderer's **hierarchical depth buffer**, testing cluster screen
bounds against the appropriate HZB mip.

The system SHALL support **two-pass occlusion**: a first pass rendering what the previous frame's
visibility suggests is visible, an HZB built from the resulting depth, and a second pass testing
geometry that was uncertain or newly visible.

Occlusion SHALL be conservative: geometry that becomes visible SHALL be drawn rather than omitted,
and disocclusion SHALL not produce a frame of missing geometry.

#### Scenario: Fine-grained occlusion
- **WHEN** a large object is mostly hidden behind a wall
- **THEN** its occluded clusters SHALL be culled individually, not only the whole object

#### Scenario: Newly visible geometry appears immediately
- **WHEN** geometry becomes visible as the camera moves
- **THEN** the second pass SHALL draw it in the same frame rather than a frame late

### Requirement: Rasterisation paths
Visible clusters SHALL be rasterised through a **hardware** path by default.

The architecture SHALL permit a **compute rasterisation** path for micro-triangles, selected per
cluster or per triangle by projected size, because hardware rasterisers become inefficient when
triangles cover well under a pixel.

The compute path is **not required initially** and is recorded as a later phase; the specification
requires only that the architecture accommodate it — cluster dispatch, output format, and the
visibility buffer SHALL not assume hardware rasterisation exclusively.

Where available, **mesh shaders** MAY be used, but SHALL NOT be required: compute plus indirect
draw SHALL remain a fully supported baseline, because mesh shader support is not universal.

#### Scenario: Mesh shaders are optional
- **WHEN** a device does not support mesh shaders
- **THEN** the compute and indirect-draw path SHALL render virtual geometry with no content
  differences

#### Scenario: Path selection is per cluster
- **WHEN** a compute rasteriser exists and a cluster's triangles are sub-pixel
- **THEN** that cluster MAY be routed to the compute path while larger clusters use hardware
  rasterisation

### Requirement: Visibility buffer and material resolve
The engine SHALL provide a **visibility buffer** pipeline in which the geometry pass writes only
what is needed to identify a surface — instance identifier, primitive identifier, and sufficient
information to recover barycentrics — and material evaluation happens in a later pass.

Material resolve SHALL **classify pixels by material** into bins and evaluate each bin, so that a
scene with many materials does not become many draw calls.

The resolve pass SHALL reconstruct attributes — position, normal, tangent frame, UVs — from the
identified primitive rather than interpolating them through the geometry pass.

Virtual geometry SHALL also function in the **Forward+** pipeline, rasterising clusters into the
existing depth and colour passes, with the limitations documented — notably that micro-triangle
efficiency and material binning benefits are unavailable there.

#### Scenario: Many materials, few passes
- **WHEN** a view contains thousands of distinct materials
- **THEN** material resolve SHALL evaluate them in bins rather than issuing per-material draws

#### Scenario: Works without the visibility buffer
- **WHEN** a project uses the Forward+ pipeline
- **THEN** virtual geometry SHALL render correctly, with the performance limitations documented

#### Scenario: Attributes are reconstructed
- **WHEN** a pixel is shaded
- **THEN** its attributes SHALL be derived from the identified instance and primitive, not carried
  through the geometry pass

### Requirement: Tangent policy
Assets SHALL declare a **tangent policy**: tangents stored per vertex, derived in the shader from
position and UV derivatives, or absent where no material requires them.

The cooker SHALL default to deriving tangents where the materials assigned to an asset permit it,
and SHALL report the storage saved.

#### Scenario: Tangents are not stored unnecessarily
- **WHEN** an asset's materials can use derived tangents
- **THEN** per-vertex tangents SHALL be omitted and the saving reported

#### Scenario: Policy is per asset
- **WHEN** an asset requires stored tangents for correctness
- **THEN** it SHALL declare so, and the cooker SHALL store them

### Requirement: Geometry compression
Geometry pages SHALL use encoding tuned to their content:

- **Positions** quantised relative to cluster or page bounds rather than stored as world-space
  32-bit floats
- **Normals** octahedral-encoded at a declared bit depth
- **Tangents** derived where the tangent policy permits
- **UVs** quantised relative to a per-cluster range
- **Colours** at reduced precision
- **Indices** encoded compactly given the bounded vertex count per cluster
- **Hierarchy and metadata** bit-packed

Encoding precision SHALL be a cooker policy with reported error, so quality is a decision rather
than a default.

The achieved bytes per triangle SHALL be reported per asset.

#### Scenario: Positions are cluster-relative
- **WHEN** positions are encoded
- **THEN** they SHALL be quantised within cluster or page bounds and reconstructed on the GPU,
  rather than stored as full-precision world coordinates

#### Scenario: Compression cost is visible
- **WHEN** an asset is cooked
- **THEN** bytes per triangle and the quantisation error introduced SHALL be reported

### Requirement: Geometry budget and importance
The system SHALL adjust the screen-error threshold to hold the **geometry allocation** issued by
the renderer budget arbiter (see `rendering-architecture`), raising the threshold under pressure
and lowering it when headroom exists within that allocation.

The system SHALL measure its own GPU cost and report it to the arbiter. It SHALL NOT measure total
frame time.

The system SHALL declare a **reserved minimum** quality and SHALL report when it has reached it,
so the arbiter reallocates rather than continuing to coarsen geometry that cannot coarsen further.

Adjustment SHALL be smooth and hysteretic: detail SHALL NOT visibly pump between frames, and SHALL
operate on a shorter time constant than the arbiter's reallocation.

Objects SHALL declare an **importance** — critical, gameplay, normal, background — scaling their
effective threshold, so gameplay-relevant geometry keeps detail while background geometry degrades
first.

The threshold SHALL be settable per view, and each view SHALL draw from its own allocation, so
secondary views cost less and degrade before the primary view does.

**Pinned mode** SHALL be global: when the arbiter is pinned, this controller SHALL be pinned with
it.

#### Scenario: Budget is held
- **WHEN** measured virtual geometry GPU time exceeds its allocation
- **THEN** the threshold SHALL be raised until the allocation is met, with the adjustment reported

#### Scenario: Important geometry keeps detail
- **WHEN** the scene is overloaded
- **THEN** background geometry SHALL coarsen before gameplay-critical geometry does

#### Scenario: No visible pumping
- **WHEN** load hovers around the allocation
- **THEN** hysteresis and rate limiting SHALL prevent detail from oscillating visibly

#### Scenario: Minimum reached
- **WHEN** geometry has coarsened to its declared minimum and the frame is still over budget
- **THEN** it SHALL report that it is at its minimum, and SHALL NOT coarsen further

### Requirement: Instancing and assemblies
Instances of one asset SHALL share its hierarchy and pages entirely, differing only in transform
and material parameters, and SHALL be represented as GPU scene instances.

The system SHALL support **assemblies**: an asset composed by referencing other assets with
transforms, so repeated sub-parts are stored once rather than baked into the parent's geometry.

Assemblies SHALL be resolved during traversal, so a referenced part's clusters are streamed and
culled like any other geometry.

#### Scenario: Repeated parts are not duplicated
- **WHEN** a model contains two hundred instances of one bolt
- **THEN** the bolt's geometry SHALL be stored once and referenced, not baked two hundred times

#### Scenario: Assembly parts stream independently
- **WHEN** an assembly is partially visible
- **THEN** only the referenced parts that are visible SHALL have their pages streamed

### Requirement: Fallback and platform paths
Every asset SHALL have a **fallback mesh** representation, used for: devices without the required
GPU capabilities, ray tracing acceleration structures, physics proxy generation, editor tooling,
and any path where virtual geometry is unavailable.

The renderer SHALL select a **geometry path** per device: virtual geometry, mesh-shader-accelerated,
or traditional indexed rendering, based on capability queries rather than device identity.

Content SHALL NOT depend on virtual geometry being available; a project SHALL be shippable on a
device restricted to the fallback path, with reduced detail rather than missing content.

Ray tracing SHALL initially use the fallback representation. Native ray tracing against virtual
geometry is recorded as deferred.

**Illumination representations** SHALL be drawn from the cluster hierarchy rather than from the
fallback mesh where the hierarchy is available: the GI scene (see `rendering-global-illumination`)
selects a level meeting an illumination error target measured in world-space distance, which is
far coarser than the primary visibility target. One hierarchy SHALL serve both, and no separate
simplified mesh SHALL be cooked for illumination.

#### Scenario: Unsupported device
- **WHEN** a device lacks the required capabilities
- **THEN** assets SHALL render through the fallback path, with the reduced detail reported

#### Scenario: Ray tracing uses the fallback
- **WHEN** acceleration structures are built
- **THEN** the fallback representation SHALL be used, and the resulting detail difference SHALL be
  documented

#### Scenario: Illumination uses a coarser hierarchy level
- **WHEN** the GI scene requests a representation for an asset
- **THEN** it SHALL receive a level of the existing cluster hierarchy chosen for a world-space
  error target, not a separately authored or cooked simplification

### Requirement: Collision and navigation are separate representations
Render geometry SHALL NOT be used as collision or navigation geometry.

Collision representations — proxy meshes, convex decompositions, or signed distance fields — SHALL
be derived at cook time from the source mesh, with their own complexity budgets, independently of
the virtual geometry representation.

Navigation geometry SHALL likewise be derived independently.

The cooker SHALL report render triangle count and collision triangle count separately, so a
mismatch is visible.

#### Scenario: High-detail visual, low-detail collision
- **WHEN** a 50-million-triangle asset is cooked
- **THEN** its collision representation SHALL be a separately generated proxy of bounded
  complexity, and the physics system SHALL never receive the render geometry

#### Scenario: Counts are reported separately
- **WHEN** an asset is cooked
- **THEN** render and collision complexity SHALL be reported as distinct figures

### Requirement: Deformation classes and growth path
Assets SHALL declare a **deformation class**, and the system SHALL support them incrementally:

| Class | Support |
|---|---|
| `Static` | Required |
| `RigidInstanced` | Required |
| `Terrain` | Supported: produced by `terrain` as a geometry source |
| `Destructible` | Deferred |
| `Skinned` | Deferred |

**Terrain** produces virtual geometry clusters from its tile representation like any other geometry
source, with no parallel streaming or rendering path. Its clusters SHALL be watertight across tile
and detail boundaries.

Deferred classes SHALL have their architectural seams reserved:

- **Destructible** — fragments pre-cooked with cluster mappings and activated as subsets, never
  rebuilt at runtime
- **Skinned** — clusters carrying bone influence sets, deformed from the **GPU pose world**
  (see `animation-and-skinning`) so only visible clusters are deformed, integrated with animation
  LOD

A change adding a deferred class SHALL go through the OpenSpec flow.

#### Scenario: Terrain is one representation
- **WHEN** terrain is rendered
- **THEN** it SHALL produce virtual geometry rather than requiring a duplicate representation
  streamed alongside it

#### Scenario: Destruction does not rebuild at runtime
- **WHEN** an object is destroyed
- **THEN** pre-cooked fragment subsets SHALL be activated, with no runtime hierarchy construction

#### Scenario: A change would close a seam
- **WHEN** a proposal would make the cluster format unable to carry bone influences
- **THEN** it SHALL be flagged against this requirement

### Requirement: Aggregate and thin geometry classification
Assets SHALL declare a **surface class** — solid, aggregate, foliage, hair, or thin — because
simplification and occlusion behave differently for each.

Aggregate and thin geometry — foliage, hair, chain-link, grates — simplifies poorly and occludes
poorly, and the cooker SHALL apply class-appropriate clustering and simplification policies rather
than treating every triangle identically.

Where a class is not well served by virtual geometry, the cooker SHALL say so at import rather than
producing a poor result silently.

#### Scenario: Foliage is not treated as solid
- **WHEN** a foliage asset is cooked
- **THEN** class-appropriate policies SHALL be applied, and any known limitation SHALL be reported
  at import

#### Scenario: Unsuitable content is flagged
- **WHEN** an asset's topology is poorly suited to virtual geometry
- **THEN** the cooker SHALL report it with the reason, rather than producing degenerate clusters

### Requirement: Authoring experience
Enabling virtual geometry SHALL require no manual LOD authoring. Import SHALL expose: an enable
toggle, quality and compression policy, fallback generation, and collision proxy generation.

Import SHALL report: source triangle count, cluster count, hierarchy depth, cooked size, resident
size, bytes per triangle, and any warnings about content suitability.

#### Scenario: Import without manual LODs
- **WHEN** a high-polygon mesh is imported with virtual geometry enabled
- **THEN** no LOD chain SHALL need to be authored, and the report SHALL state the resulting cluster
  and page structure

#### Scenario: Cost is visible at import
- **WHEN** an asset is imported
- **THEN** its cooked size, resident footprint, and bytes per triangle SHALL be reported

### Requirement: Visualisation and diagnostics
The engine SHALL provide visualisation modes, because GPU-driven virtualised geometry is otherwise
close to undebuggable: clusters, hierarchy level, triangle density, screen-space error, page
residency, streaming activity, overdraw, occlusion results, material bins, rasterisation path,
fallback usage, and per-object geometry cost.

The profiler SHALL report per frame: instance counts before and after culling, candidate and
visible cluster counts, visible triangle count, geometry cache occupancy and budget, streaming
throughput, missing page count, and GPU time per stage (instance cull, hierarchy traversal, cluster
cull, rasterisation, material resolve).

It SHALL answer **causal** questions: why a given cluster was rendered, why a page was streamed,
why the geometry cache is thrashing, and which assets produce the most clusters.

A per-object inspector SHALL report, for a selected instance: source triangles, visible triangles,
visible clusters, resident and requested pages, GPU memory, achieved screen error, and its share of
geometry GPU time.

#### Scenario: Why was this cluster rendered
- **WHEN** a developer selects a cluster
- **THEN** the diagnostic SHALL report its screen error, the threshold in force, and the tests it
  passed

#### Scenario: Cache thrashing is diagnosable
- **WHEN** the geometry cache evicts and reloads the same pages repeatedly
- **THEN** the diagnostic SHALL identify it and the assets responsible

#### Scenario: Finding the expensive asset
- **WHEN** geometry cost is high
- **THEN** the per-asset breakdown SHALL identify which assets contribute the most clusters and
  time

### Requirement: Streaming integration seams
Virtual geometry page streaming SHALL integrate with content and texture streaming rather than
operating independently.

Three levels of streaming SHALL be distinguished: **content streaming** determines which objects
exist (see `world-partition-and-streaming`), **geometry streaming** determines their shape detail,
and **texture streaming** determines their surface detail (see `virtual-texturing`). All three SHALL
be driven from a shared notion of the viewer's region of interest and SHALL be scored and budgeted
through the shared residency policy (see `residency`).

Cell membership SHALL drive geometry **prefetching**: when a cell becomes resident, the root pages
of its geometry SHALL be requested, so that an activated cell is never visible without at least its
coarsest representation.

Geometry and texture residency SHALL be **jointly budgeted**: under memory pressure the residency
layer SHALL decide between geometry detail and texture detail by importance and visible impact,
rather than each system evicting independently.

Geometry residency decisions SHALL remain the responsibility of this system; the shared layer
supplies policy, priority, and budget, not storage.

#### Scenario: Object existence and object detail are distinct
- **WHEN** a region streams in
- **THEN** object existence SHALL be established by content streaming, and geometry and texture
  detail SHALL be streamed separately on demand

#### Scenario: Seams remain open
- **WHEN** a further paged subsystem is introduced
- **THEN** it SHALL be able to share the residency policy and budget without changes to this
  system's page table or request path

#### Scenario: An activated cell is never blank
- **WHEN** a world cell is activated
- **THEN** the root pages of its geometry SHALL already have been requested, so its objects render
  at coarse detail immediately rather than appearing later

#### Scenario: Geometry and textures compete fairly
- **WHEN** memory pressure forces a reduction
- **THEN** the residency layer SHALL weigh geometry detail against texture detail by importance,
  rather than each subsystem reducing independently

### Requirement: Gameplay API
Gameplay SHALL see no cluster, page, or hierarchy concepts. A renderable entity SHALL declare a
geometry asset handle, a material handle, and optional virtual geometry settings — enable, quality
bias, and importance.

The same component SHALL work whether the asset uses the virtual or traditional path.

#### Scenario: Gameplay is unaware
- **WHEN** a developer places a renderable entity
- **THEN** the component surface SHALL be a geometry handle and a material handle, regardless of
  the underlying representation

#### Scenario: Importance is the only tuning exposed
- **WHEN** a developer needs a hero object to retain detail
- **THEN** setting its importance SHALL suffice, with no exposure of thresholds or clusters
