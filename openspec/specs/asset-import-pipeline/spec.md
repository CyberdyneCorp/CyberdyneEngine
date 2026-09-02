# asset-import-pipeline Specification

## Purpose

Defines how source assets become cooked runtime content: the importer framework, the cook cache,
per-format importers, mesh and texture processing, and the build-time asset pipeline.

Source-format parsing is delegated to proven libraries — **glTF** and **ufbx** for scenes,
**meshoptimizer** for simplification, **xatlas** for UV unwrapping — behind engine-owned importer
interfaces.

## Requirements

### Requirement: Importer framework
An **importer** SHALL declare: the source extensions it handles, its version, its options schema
(a reflected settings struct), and the asset kinds it produces.

Import SHALL be a pure function of (source bytes, options, importer version, platform variant),
so its output is cacheable and reproducible.

Each source asset SHALL have a `.meta` sidecar recording its `AssetId`, the importer used, its
option values, and the ids of produced sub-assets.

#### Scenario: Deterministic output
- **WHEN** the same source and options are imported twice
- **THEN** the cooked output SHALL be byte-identical

#### Scenario: Sub-asset identity is stable
- **WHEN** a scene file produces meshes, materials, and animations
- **THEN** each SHALL receive a stable sub-asset id recorded in the `.meta`, so references survive
  re-import

#### Scenario: Options change triggers re-import
- **WHEN** an import option changes
- **THEN** the cache key SHALL change and the asset SHALL be re-cooked

### Requirement: Cook cache
Cooked outputs SHALL be stored in a **content-addressed cache** keyed by the import function's
inputs, shared between developers and CI where a remote cache is configured.

The cache SHALL support: local storage, a shared read-only remote, and a writable remote for CI to
populate.

#### Scenario: Shared cache hit
- **WHEN** a developer pulls a branch whose assets CI already cooked
- **THEN** the cooked artefacts SHALL be fetched from the shared cache rather than re-imported
  locally

#### Scenario: Importer version bump
- **WHEN** an importer's version increases
- **THEN** all assets it handles SHALL be re-cooked, since the version is part of the cache key

### Requirement: Texture import
The texture importer SHALL produce cooked textures with: format selected from declared **usage**
(colour, normal, data, HDR, UI) and target platform, a full mip chain generated in the correct
colour space, and optional alpha-coverage preservation.

Options SHALL include: maximum resolution per platform, compression quality, sRGB flag, normal-map
convention conversion, channel packing and swizzling, alpha handling (opaque, straight,
premultiplied), wrap mode, and whether the texture is streamable.

The importer SHALL detect common mistakes: a normal map marked sRGB, a colour texture marked
linear, a non-power-of-two texture where the target format requires it, and unnecessary alpha
channels.

#### Scenario: Platform variants
- **WHEN** a texture is cooked for desktop and mobile
- **THEN** BC7 and ASTC variants SHALL be produced, and each platform's package SHALL include only
  its own

#### Scenario: Detected mistake
- **WHEN** a texture bound to a roughness slot is flagged sRGB
- **THEN** the importer SHALL warn with the specific slot and the likely-intended setting

### Requirement: Model import
The model importer SHALL support **glTF 2.0** (`.gltf`, `.glb`) as the primary interchange
format and **FBX** via ufbx, producing meshes, materials, textures, skeletons, animations, and a
scene hierarchy as a prefab. **USD** SHALL be supported as an optional, tool-time-only importer.

Import SHALL perform, in a defined order:
1. Parse and convert to engine coordinate conventions (handedness, up axis, unit scale)
2. Build meshes: index and vertex buffers, split by material, weld vertices within a tolerance
3. Generate missing data: normals (with a smoothing angle), tangents, and UV2 for lightmapping
4. Optimise: vertex cache ordering, overdraw reduction, vertex fetch optimisation
5. Generate LOD chain to configured reduction targets
6. Generate collision: none, convex hull, convex decomposition, or triangle mesh, per options and
   node naming conventions
7. Import skeletons, derive bone LOD levels, and remap to a `SkeletonProfile` if configured
8. Import animations with error-bounded compression settings, optionally splitting into clips by
   time ranges, and optionally retargeting through a retarget profile
9. Import materials, mapping source parameters to the standard material
10. Produce a prefab representing the hierarchy

Node-level options SHALL be editable per node in an import settings dialog and stored in the
`.meta`, so an artist's naming convention or a designer's per-node choice both work.

#### Scenario: Coordinate conversion
- **WHEN** a Z-up model is imported
- **THEN** it SHALL be converted at import so no runtime code accounts for source handedness

#### Scenario: Collision from a naming convention
- **WHEN** a node is named with the configured collision suffix
- **THEN** a collider SHALL be generated from it and the node excluded from rendering

#### Scenario: Material extraction
- **WHEN** materials are set to be extracted
- **THEN** they SHALL be written as separate editable assets, and re-import SHALL preserve edits
  rather than overwriting them

#### Scenario: Animation-only re-import
- **WHEN** a source file is re-imported with meshes and materials disabled
- **THEN** only animations SHALL be produced, which is the fast path for animation iteration

#### Scenario: USD is tool-time only
- **WHEN** USD import is enabled
- **THEN** it SHALL be available in the editor and cooker only, and no USD code SHALL be linked
  into a shipped runtime

### Requirement: Mesh processing
Mesh processing SHALL provide, as reusable steps available to importers and to runtime tools:

- vertex welding within position, normal, and UV tolerances
- normal generation with a smoothing angle, and tangent generation with a documented convention
- vertex cache optimisation, overdraw optimisation, and vertex fetch optimisation
- LOD simplification with error bounds, seam and boundary preservation, and attribute weighting
- UV2 generation with configurable texel density, chart padding, and distortion limits
- convex hull generation and convex decomposition for collision

#### Scenario: Simplification preserves seams
- **WHEN** a mesh with UV seams is simplified
- **THEN** seam vertices SHALL be preserved or collapsed only along the seam, avoiding texture
  distortion

#### Scenario: Tangent convention
- **WHEN** tangents are generated
- **THEN** they SHALL follow the documented convention matching the normal map convention, so
  imported and generated tangents agree

### Requirement: Audio import
The audio importer SHALL produce cooked audio with: format selected by intended use (decoded PCM,
in-memory compressed, or streamed), sample rate conversion, channel handling (force mono for 3D
sources), loop point preservation, normalisation, and silence trimming.

#### Scenario: 3D sound forced to mono
- **WHEN** a stereo file is imported for use as a 3D source
- **THEN** it SHALL be downmixed to mono, since stereo cannot be spatialised meaningfully

### Requirement: Other importers
The engine SHALL provide importers for: fonts (see `text-and-fonts`), images used as sprites or
UI (with atlas packing and nine-slice metadata), shaders and shader includes, localisation tables
(CSV, PO, or a structured format producing per-locale assets), video, and generic data files
(JSON, TOML) as typed data assets.

**Custom importers** SHALL be registrable from modules and from Swift, with the same options,
caching, and dialog integration as built-ins.

#### Scenario: Sprite atlas
- **WHEN** a folder of sprites is imported as an atlas
- **THEN** they SHALL be packed with padding, and each source SHALL yield a sub-asset referencing
  its region

#### Scenario: Custom importer
- **WHEN** a project registers an importer for a proprietary format
- **THEN** it SHALL participate in the cache, the import dialog, and dependency tracking without
  engine changes

### Requirement: Dependency tracking
Import SHALL record the dependencies of each cooked asset: source files it read (including
referenced textures and includes), other assets it referenced, and engine settings it consulted.

A change to any dependency SHALL invalidate the cooked output.

#### Scenario: Shader include changes
- **WHEN** a shared shader include is edited
- **THEN** every shader that includes it SHALL be re-cooked

#### Scenario: Referenced texture moves
- **WHEN** a texture referenced by an imported material moves
- **THEN** the reference SHALL resolve by `AssetId` and no re-import SHALL be needed

### Requirement: Import performance
Import SHALL run in parallel on the job system, ordered by dependency, with progress reporting
and cancellation.

Long imports SHALL be incremental where the format allows, and the pipeline SHALL report the
slowest importers so bottlenecks are visible.

#### Scenario: Parallel import
- **WHEN** a project with thousands of assets is first opened
- **THEN** import SHALL saturate available cores in dependency order, with a progress display

#### Scenario: Cancellation
- **WHEN** the user cancels an import
- **THEN** it SHALL stop at the next step boundary, leaving the cache consistent

### Requirement: Build-time cooking and packaging
A **cook** step SHALL produce, for a target platform and configuration: all cooked assets in
their platform variants, packages (`.cypak`) organised into chunks by a configurable rule
(by scene, by folder, or by an explicit manifest), a pipeline manifest for shader warm-up, and a
content manifest for patching.

Cooking SHALL support **content trimming**: excluding assets not reachable from declared roots,
with a report of what was excluded.

#### Scenario: Unreferenced asset is excluded
- **WHEN** an asset is not reachable from any root scene or explicit include
- **THEN** it SHALL be excluded from the package and listed in the report

#### Scenario: Chunked packaging
- **WHEN** packages are chunked per level
- **THEN** each level's package SHALL be independently mountable, enabling partial downloads and
  patches

### Requirement: Import diagnostics
The pipeline SHALL report per asset: import duration, output size, format chosen, warnings and
errors, and the cache outcome (hit, miss, or invalidated with the reason).

A project-level report SHALL summarise: total cooked size by category, the largest assets, assets
with warnings, and assets whose import is slowest.

#### Scenario: Finding size regressions
- **WHEN** a build's package grows unexpectedly
- **THEN** the size report SHALL identify which assets and categories grew
