# material-compiler Specification

## Purpose

Defines **CyberMaterial**: how an authored material becomes a compiled program, and what happens
to it at runtime.

The renderer already had a shading model and a shader toolchain. What it did not have was a
compiler between them. This capability is that middle: a typed **material IR** with real
optimisation passes, a **closure** model that lets a surface be layered rather than chosen from a
menu, cost analysis attributed back to the graph nodes that caused it, and a compiled program that
thousands of instances share.

Three decisions carry most of the weight. **Closures are the authoring model and shading models
are the lowered form** — a closure set matching a known model costs exactly what that model costs,
so generality is available without taxing the materials that do not use it. **Parameters are
classified static or runtime by the compiler, from use** — an author cannot promote a number to a
permutation axis, which is what keeps variant counts finite. And **materials request attributes
semantically, never vertex layouts** — under a visibility buffer one program serves every geometry
source, and under a vertex-stage pipeline it does not, which is stated plainly rather than
wished away.

The engine owns the material IR and its passes. It does not own a shader optimiser; it produces
good input to somebody else's.

## Requirements

### Requirement: Engine-owned material compiler
The material compiler SHALL be engine code: the authoring model, the intermediate representation
and its optimisation passes, the closure model, lowering to shading models, parameter
classification, quality tier generation, cost analysis, and the compiled program format.

The compiler SHALL emit **Slang** and SHALL pass through the shader pipeline defined in
`shader-system`. It SHALL NOT introduce a second shader toolchain, a second cache, or
backend-specific source.

The engine SHALL NOT implement a shader optimiser or a backend code generator; those belong to the
shader toolchain.

#### Scenario: One toolchain
- **WHEN** the material compiler produces a program
- **THEN** it SHALL be emitted as Slang and compiled, reflected, cached, and hot-reloaded by the
  existing shader pipeline

#### Scenario: The boundary is the IR
- **WHEN** the shader toolchain is upgraded or replaced
- **THEN** the material IR, its passes, and the cost model SHALL be unaffected

### Requirement: Authoring forms share one representation
A material SHALL be authorable as a **node graph** or as a **text material definition**, and both
SHALL be front-ends producing the same material IR.

Neither form SHALL be able to express something the other cannot represent. The editor SHALL be
able to show, for any material, the IR it produced and the Slang source generated from it.

#### Scenario: Graph and text are equivalent
- **WHEN** a graph and a text definition describe the same material
- **THEN** they SHALL produce identical IR and an identical compiled program

#### Scenario: Generated source is what runs
- **WHEN** a developer inspects a graph-authored material
- **THEN** the editor SHALL show the generated Slang, and that source SHALL be what is compiled —
  not a separate editor-only approximation

### Requirement: Material intermediate representation
The compiler SHALL lower authored materials to a typed **material IR**: a static single-assignment
expression graph whose values carry types and semantic roles, whose leaves are inputs, parameters,
constants, and texture samples, and whose root is a **surface closure set**.

The IR SHALL be serialisable and versioned, so it can be cached, diffed, and inspected.

The IR SHALL be independent of pipeline, renderer profile, quality tier, and backend; those are
inputs to lowering, not to IR construction.

#### Scenario: One IR, many outputs
- **WHEN** a material is compiled for two pipelines and three quality tiers
- **THEN** one IR SHALL be constructed once and lowered six times, rather than the material being
  re-parsed per target

#### Scenario: IR is inspectable
- **WHEN** a material behaves unexpectedly
- **THEN** its IR SHALL be dumpable in a readable form, before and after optimisation

### Requirement: Optimisation passes
The compiler SHALL apply, at minimum: type propagation, constant folding, dead-node elimination,
common subexpression elimination, texture sample deduplication, closure simplification, and
uniform versus varying analysis.

Uniform analysis SHALL identify subexpressions that are constant across a draw or across the
frame, so they can be hoisted into parameter data rather than recomputed per pixel.

Each pass SHALL be individually disableable in development builds, so a suspected miscompilation
can be bisected.

#### Scenario: Duplicate samples collapse
- **WHEN** a graph samples one texture with identical coordinates in three places
- **THEN** the compiled program SHALL contain one sample

#### Scenario: Disconnected work is removed
- **WHEN** a graph contains nodes whose results reach no output
- **THEN** they SHALL be eliminated, and the editor SHALL be able to show which nodes were dropped

#### Scenario: Uniform expression is hoisted
- **WHEN** a subexpression depends only on material parameters
- **THEN** it SHALL be evaluated once into parameter data rather than per pixel

### Requirement: Surface closures
The material model SHALL be a set of **BSDF closures** with weights, not a fixed struct of
channels. The closure set SHALL include at minimum: diffuse, specular (microfacet), coat,
transmission, subsurface, sheen, and emission.

A material SHALL be able to compose closures — a coat over a metallic base, transmission added to
a diffuse leaf, absorption within a transmissive volume — with the composition expressed in the
IR rather than selected from a menu.

Closure composition SHALL preserve energy: a layer SHALL attenuate the layers beneath it.

#### Scenario: Layered surface
- **WHEN** a car paint material composes a metallic base with a clear coat
- **THEN** the base contribution SHALL be attenuated by the coat's Fresnel term, and total
  reflectance SHALL remain physical

#### Scenario: Closures compose without a new shading model
- **WHEN** an author needs diffuse plus transmission plus sheen
- **THEN** it SHALL be expressible as a closure set, without adding a shading model to the engine

### Requirement: Lowering to shading models
The compiler SHALL **match a material's closure set against the engine's known shading models**
(see `rendering-materials-and-shading`) and lower to that model's evaluation path when it matches.

A closure set with no matching model SHALL lower to a **generic layered evaluator**, whose higher
cost SHALL be reported at cook time.

A material whose closures match a known model SHALL cost the same as a material authored directly
against that model. Generality SHALL NOT impose cost on materials that do not use it.

#### Scenario: Common case pays nothing
- **WHEN** a material's closures are exactly the standard metallic-roughness model
- **THEN** it SHALL lower to that model's path, with cost identical to a material authored against
  it directly

#### Scenario: Generality is priced, not hidden
- **WHEN** a material's closure set has no matching model
- **THEN** it SHALL use the generic evaluator, and the cook report SHALL state the cost difference

#### Scenario: Profile restricts models
- **WHEN** a renderer profile does not support the generic evaluator
- **THEN** cooking a material that requires it for that profile SHALL fail with a diagnostic
  naming the closures responsible

### Requirement: Static and runtime parameters
The compiler SHALL classify every material parameter as **static** — participating in the
compiled program's structure — or **runtime** — stored as data and changeable without
recompilation.

Classification SHALL be **derived from use**, not declared by the author: a parameter is static
only when it feeds a control-flow or resource decision that cannot be expressed as data.

The set of static parameters SHALL constitute the material's permutation axes and SHALL count
against the permutation budget in `shader-system`.

#### Scenario: A number is data
- **WHEN** a parameter only scales a value
- **THEN** it SHALL be a runtime parameter, and changing it SHALL not compile anything

#### Scenario: A structural switch is static
- **WHEN** a parameter selects whether a coat closure exists at all
- **THEN** it SHALL be static, and its values SHALL form a declared permutation axis

#### Scenario: Authors cannot force specialisation
- **WHEN** an author marks a purely numeric parameter as static
- **THEN** the compiler SHALL reject the annotation with a diagnostic, since it would multiply
  programs for no structural difference

### Requirement: Geometry attribute interface
Materials SHALL request geometry attributes **semantically** — position, normal, tangent frame,
UV by index, vertex colour, custom attributes by name — and SHALL NOT reference vertex buffer
slots, offsets, or formats.

Each geometry source SHALL supply an **attribute decoder** implementing that interface: static
mesh, skinned mesh (reading the GPU pose world), virtual geometry (decoding compressed cluster
attributes), terrain, mesh particle, and procedural geometry.

Under a visibility buffer pipeline, one compiled material program SHALL serve every geometry
source, with the decoder selected from the instance's geometry kind at resolve time.

Under a vertex-stage pipeline such as Forward+, a material SHALL require one variant per geometry
source it is used with, because attributes are produced by the vertex stage. This limitation SHALL
be documented, and the variant count SHALL be reported.

#### Scenario: Material does not know its mesh
- **WHEN** a material requests `normal`
- **THEN** the geometry source SHALL supply it, whether from vertex data, a decoded cluster, or a
  skinned pose, with no material change

#### Scenario: Visibility buffer collapses the matrix
- **WHEN** one material is used on static meshes, skinned meshes, virtual geometry, and mesh
  particles under a visibility buffer pipeline
- **THEN** one program SHALL serve all four

#### Scenario: The vertex-stage cost is reported
- **WHEN** the same material is used across four geometry sources under Forward+
- **THEN** four variants SHALL be compiled and the count SHALL appear in the material's cost
  report

### Requirement: Material programs and instances
A compiled **material program** SHALL be immutable and shared. A **material instance** SHALL
reference a program plus a parameter block and a resource block, and SHALL be creatable at runtime
without compilation.

An instance SHALL be able to derive from another instance, overriding a subset of parameters.

Any number of instances SHALL share one program. Creating an instance SHALL NOT create a pipeline.

#### Scenario: Variants are data
- **WHEN** a project defines a hundred colour variants of one material
- **THEN** they SHALL share one program and differ only in parameter data

#### Scenario: Runtime instance creation
- **WHEN** gameplay creates a material instance and sets parameters
- **THEN** no shader compilation SHALL occur and the instance SHALL be usable the same frame

### Requirement: GPU material table
Material programs, parameter blocks, and resource references SHALL be resident in a **GPU material
table**, addressed by a material identifier carried in GPU scene instance data.

Shading SHALL reach a material's data by indexing that table, with no CPU-side per-object binding
and no pointer chasing.

Parameter and resource updates SHALL be batched: changed entries SHALL be collected and uploaded
in one transfer per frame.

#### Scenario: GPU-driven shading needs no CPU
- **WHEN** a GPU-generated draw workload shades a pixel
- **THEN** the material SHALL be reached through the instance's material identifier and the
  material table, with no descriptor set bound per object

#### Scenario: Batched update
- **WHEN** two hundred material instances change parameters in one frame
- **THEN** they SHALL be uploaded in a single transfer

### Requirement: Material classification and binning
Under deferred material evaluation, visible pixels SHALL be **classified by material program** and
evaluated in bins, so that the number of shading dispatches scales with the number of distinct
programs rather than with the number of objects or material instances.

Classification SHALL be computed on the GPU, and bin dispatches SHALL be indirect.

Pixels whose program is identical but whose instances differ SHALL share a bin, since instance
data is indexed rather than bound.

#### Scenario: Many instances, one bin
- **WHEN** ten thousand instances of one material program are visible
- **THEN** they SHALL be evaluated in one bin

#### Scenario: Cost scales with programs
- **WHEN** a view contains a thousand material instances derived from twelve programs
- **THEN** material evaluation SHALL dispatch twelve bins

### Requirement: Material quality tiers
The compiler SHALL generate **quality tiers** for a material by progressively removing closures,
inputs, and detail layers whose contribution falls below a declared threshold, producing programs
of decreasing cost from one authored material.

Tiers SHALL be a declared permutation axis of small, fixed cardinality (default 3), counting
against the permutation budget.

Tier selection SHALL be per instance per frame, from projected screen size, importance, and the
renderer budget arbiter's current allocation.

An author SHALL be able to opt a material out of automatic tiering and supply tiers explicitly.

#### Scenario: Distance reduces material cost
- **WHEN** an instance of an expensive hero material is small on screen
- **THEN** a lower tier SHALL be selected, dropping parallax, detail layers, and secondary
  closures

#### Scenario: Tiers do not multiply everything
- **WHEN** a material has three tiers and two static parameters with two values each
- **THEN** the reported permutation count SHALL be twelve, and SHALL count against the budget

#### Scenario: Transitions are not visible
- **WHEN** an instance crosses a tier boundary
- **THEN** the change SHALL be applied with hysteresis, so it does not oscillate frame to frame

### Requirement: Custom code and escape hatches
The system SHALL support: a **custom expression node** embedding Slang within a graph, a
**custom closure** registered by a plugin, a **custom attribute decoder** for a new geometry
source, and a **fully hand-authored material program** bypassing the graph.

Each SHALL integrate through the same IR, cost model, cooking path, and diagnostics. Custom code
SHALL NOT be able to bypass the shader pipeline or the material table.

#### Scenario: Custom node participates in optimisation
- **WHEN** a custom expression node's output is unused
- **THEN** it SHALL be eliminated like any other node

#### Scenario: Plugin closure is a first-class citizen
- **WHEN** a plugin registers a closure
- **THEN** materials using it SHALL cook, tier, classify, and report cost through the same paths

### Requirement: Material cost analysis
The compiler SHALL produce, per material and per tier, a **cost report**: texture sample count,
arithmetic instruction count, branch count, closure count, estimated register pressure, variant
count, and an estimated full-screen cost for a named profile.

Costs SHALL be **attributed back to graph nodes**, so an author sees which nodes are expensive
rather than only that the material is expensive.

Unsupported constructs SHALL be reported per profile, naming the node responsible.

#### Scenario: The expensive node is identified
- **WHEN** an author opens an expensive material
- **THEN** the editor SHALL highlight the nodes contributing most to its cost

#### Scenario: Profile incompatibility is specific
- **WHEN** a material uses a node unavailable on the mobile profile
- **THEN** the report SHALL name that node, not merely report the material as unsupported

### Requirement: Node previews use the real compiler
Every graph node SHALL be previewable, and previews SHALL be generated through the same compiler,
lowering, and shader pipeline as runtime.

There SHALL be no separate editor-only shading path.

#### Scenario: Preview matches the frame
- **WHEN** a node preview shows a surface
- **THEN** it SHALL be produced by the same generated program the renderer would use

### Requirement: Material API
Game code SHALL address materials through handles and typed parameters, never through shaders,
pipelines, descriptor sets, or draw commands.

Parameter names SHALL resolve to **compile-time identifiers**; per-frame string lookup SHALL NOT
be required.

The ECS surface SHALL be a renderable component referencing geometry and material, plus an
optional per-entity material instance override.

The C ABI and the Swift overlay SHALL expose loading a material, creating an instance, setting
typed parameters, and assigning an instance to an entity.

#### Scenario: No strings per frame
- **WHEN** gameplay sets a material parameter every frame
- **THEN** it SHALL use a resolved parameter identifier, not a string lookup

#### Scenario: Gameplay never sees a pipeline
- **WHEN** a Swift behaviour tints a material instance
- **THEN** it SHALL set a typed parameter, and no shader, pipeline, or descriptor concept SHALL
  appear in the API

### Requirement: Cooking, caching, and versioning
Material compilation SHALL be an offline cook step producing: the IR, the generated Slang, the
compiled programs per target and tier, reflection data, the cost report, and pipeline state
metadata.

Cook keys SHALL include the material graph, the compiler version, the IR version, the target
profile, the feature set, and the quality tier — so a compiler change invalidates derived shader
data without invalidating the material asset itself.

Cooked material outputs SHALL use the content-addressed cook cache defined in
`asset-import-pipeline`, including its shared and CI-populated tiers.

#### Scenario: Compiler change invalidates only derived data
- **WHEN** the material compiler version increases
- **THEN** compiled programs SHALL be recooked and the authored material assets SHALL be untouched

#### Scenario: Shared cache hit
- **WHEN** CI has already compiled a material's programs for a target
- **THEN** a developer SHALL fetch them rather than recompiling locally

### Requirement: Material validation
Cooking SHALL fail, with a diagnostic naming the responsible node or parameter, when a material:
declares an unsupported closure and blend mode combination, exceeds its permutation budget, binds
a texture to a slot that does not exist, requests an attribute its geometry source cannot supply,
or uses a construct unavailable on a target profile.

Cooking SHALL warn for: parameters declared but unused, textures assigned but never sampled, and
closures whose weight is provably zero.

#### Scenario: Impossible combination fails at cook time
- **WHEN** a material composes subsurface transmission with an additive blend mode
- **THEN** cooking SHALL fail with an explanation rather than producing undefined shading

#### Scenario: Attribute is unavailable
- **WHEN** a material requests a second UV channel and is assigned to geometry that has none
- **THEN** validation SHALL report it, naming both the material and the geometry

### Requirement: Materials are portable across pipelines and profiles
A material SHALL render under every shipped pipeline and every renderer profile it declares as a
target, differing in fidelity and cost, not in whether it appears.

Where a pipeline cannot support a material feature — a transparency mode under the visibility
buffer path, the generic layered evaluator on mobile — the limitation SHALL be documented and
reported at cook time, and a defined degradation SHALL apply rather than the material failing to
render.

#### Scenario: Same content, two pipelines
- **WHEN** a scene is rendered under Forward+ and under the visibility buffer pipeline
- **THEN** its materials SHALL render in both, with documented differences

#### Scenario: Degradation is specified, not accidental
- **WHEN** a material's feature is unsupported on a target profile
- **THEN** the declared degradation SHALL apply and the cook report SHALL state it
### Requirement: Secondary material programs
A material SHALL compile to a **family of programs**, so that a consumer does not pay the cost of
camera-visible shading for work that does not need it:

| Program | Used for | Content |
|---|---|---|
| `Primary` | Camera-visible shading | The full graph |
| `Secondary` | Filling the surface cache and answering illumination queries | Reduced texture samples, microdetail removed, secondary closures dropped where their contribution to outgoing radiance is small |
| `FarField` | Distant illumination | Constants and averaged values |
| `Shadow` | Shadow rasterisation | Opacity masking and geometric displacement only, plus transmission where supported |

Derivation SHALL be **automatic**, by removing inputs and closures whose contribution to the
program's purpose falls below a threshold, and **overridable per material**, because automatic
derivation can be wrong: a material whose base reflectance or whose opacity is produced by a node the
compiler treats as microdetail would bleed the wrong colour or cast the wrong silhouette.

The `Shadow` program SHALL additionally support **distance tiers**, so a distant shadow page samples
a cheaper opacity representation than a near one. Opaque materials SHALL produce no fragment work in
the shadow program at all.

Textures sampled by the `Shadow` program SHALL be marked **shadow-critical**, so that a coarse
representation of them is guaranteed resident and shadow rasterisation never waits on texture
residency (see `virtual-shadows` and `residency`).

The compiler SHALL report, per material, the cost of each program and the difference in average
albedo, emission, and opacity coverage between them, so an incorrect derivation is visible rather
than latent.

Each program SHALL count as a permutation axis against the permutation budget.

#### Scenario: A secondary hit is cheap
- **WHEN** the surface cache is filled for a forty-node material
- **THEN** the secondary program SHALL be used, with substantially fewer texture samples than the
  primary program

#### Scenario: Wrong derivation is visible
- **WHEN** automatic derivation changes a material's average albedo or opacity coverage
  significantly
- **THEN** the cook report SHALL flag it, so an author can override the derivation

#### Scenario: Author override
- **WHEN** an author marks a node as contributing to base reflectance or to opacity
- **THEN** it SHALL be retained in the corresponding program regardless of the automatic heuristic

#### Scenario: Far field is constant
- **WHEN** a material is used kilometres from the camera
- **THEN** its far-field program SHALL supply averaged constants rather than sampling textures

#### Scenario: Foliage casts a correct silhouette cheaply
- **WHEN** an alpha-tested foliage material rasterises into a shadow page
- **THEN** the shadow program SHALL sample only the opacity mask and its declared displacement, and
  the silhouette SHALL match the primary program's

### Requirement: Environment-aware material inputs
Materials SHALL be able to request **environmental inputs** semantically, alongside geometry
attributes: world position, slope, altitude, curvature, and any declared **environment field** —
biome, moisture, wetness, snow depth, water distance, water depth, flow, temperature, and burn
state (see `environment-fields`).

Field access SHALL go through the field substrate's bindless GPU path, so a material samples a
field without per-draw binding and without the material system knowing which system produced it.

The compiler SHALL record field usage as a dependency, so that a material's cost report states
which fields it samples and a missing field is a cook-time diagnostic rather than a runtime
default.

Field-driven material logic SHALL compose with authored data: a painted value SHALL be able to
override a field-driven rule locally.

#### Scenario: Slope becomes rock without a painted mask
- **WHEN** a terrain material declares that slopes above a threshold are rock
- **THEN** the compiler SHALL bind the necessary inputs and the rule SHALL evaluate without an
  authored mask

#### Scenario: Wetness is not a terrain concept
- **WHEN** a rock prop sitting at a river's edge samples wetness
- **THEN** it SHALL read the same field a terrain material reads, with no terrain-specific
  material path

#### Scenario: Missing field is a diagnostic
- **WHEN** a material samples a field that the project does not declare
- **THEN** cooking SHALL fail naming the field and the material, rather than silently substituting
  a default
