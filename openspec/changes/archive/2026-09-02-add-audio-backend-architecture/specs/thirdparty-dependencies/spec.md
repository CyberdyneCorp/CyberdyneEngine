## ADDED Requirements

### Requirement: Optional proprietary middleware
The engine SHALL permit optional, proprietary middleware to be adopted by a project as a **plugin
implementing an engine-owned interface**, without that middleware becoming an engine dependency.

Such middleware SHALL NOT: appear in the dependency manifest as required, be referenced by engine
code, be fetched or built by default, or be required to build or ship a game.

This exception exists because some studios have substantial existing pipelines built on
proprietary tools, and the engine's interfaces should not exclude them. It SHALL NOT be used to
justify depending on proprietary code for any engine-provided capability.

#### Scenario: Default build is fully open source
- **WHEN** the engine is built with default options
- **THEN** every fetched, built, and linked dependency SHALL carry a permissive open-source
  licence

#### Scenario: Studio-supplied middleware plugin
- **WHEN** a project supplies a proprietary backend plugin for an engine interface
- **THEN** it SHALL load through the normal plugin mechanism, and gameplay code SHALL be unchanged

## MODIFIED Requirements

### Requirement: Intended dependency set
The engine SHALL integrate the following, each behind an engine-owned interface. Versions are
pinned in the dependency manifest, not here.

| Area | Library | Licence | Why not build it |
|---|---|---|---|
| Physics | **Jolt Physics** | MIT | A competitive rigid-body solver is years of work with no differentiating value |
| Low-level audio | **miniaudio** | Public domain / MIT-0 | Device I/O, conversion, decoding and streaming across every target platform, with no dependencies |
| Spatial acoustics | **Steam Audio** | Apache 2.0 | HRTF, occlusion, transmission, reflections and propagation; years of acoustics work, open sourced |
| Text shaping | **HarfBuzz** | MIT | Correct shaping for the world's scripts is effectively unreplicable |
| Unicode | **ICU** | Unicode | BiDi, line breaking, collation, and locale data |
| Font rasterisation | **FreeType** | FreeType/BSD | Hinting and format coverage refined over decades |
| SDF fonts | **msdfgen** | MIT | Well-solved, small, focused |
| Shading language | **Slang** | MIT | A shading language with generics and multi-target output; writing one competes with building the renderer |
| SPIR-V translation | **SPIRV-Cross** | Apache 2.0 | SPIR-V to MSL and HLSL for non-Vulkan backends |
| SPIR-V tooling | **SPIRV-Tools**, **SPIRV-Reflect** | Apache 2.0 | Validation, optimisation, reflection |
| Vulkan loading | **volk**, **Vulkan-Headers**, **VMA** | MIT | Loader and a proven GPU memory allocator |
| Mesh processing | **meshoptimizer** | MIT | Simplification and cache optimisation, best in class |
| UV unwrapping | **xatlas** | MIT | Lightmap UV generation |
| Navigation meshes | **Recast** | Zlib | Voxelisation-based navmesh generation |
| glTF | **cgltf** or **tinygltf** | MIT | Parsing an open spec is not differentiating |
| FBX | **ufbx** | MIT | A closed format best handled by a maintained parser |
| Image codecs | **libpng**, **libjpeg-turbo**, **libwebp**, **tinyexr** | BSD/MIT | Standard codecs |
| Texture compression | **ISPC Texture Compressor** or **bc7enc**, **astc-encoder** | MIT/Apache 2.0 | BC and ASTC encoding |
| Audio codecs | **libvorbis**, **libopus**, **dr_libs** | BSD/Public domain | Formats miniaudio does not decode natively |
| Input and gamepads | **SDL3** | Zlib | Controller database and platform input coverage |
| Compression | **zstd**, **LZ4** | BSD/BSD | Best-in-class ratio and speed |
| Cryptography | **BLAKE3**, **mbedTLS** | CC0/Apache 2.0 | Never hand-roll cryptography |
| Testing | **Catch2** or **doctest** | BSL/MIT | Test framework |
| Profiling | **Tracy** | BSD | Frame profiler with an excellent viewer |

Steam Audio SHALL be optional and capability-gated; the engine SHALL be fully functional without
it.

#### Scenario: Backend can be replaced
- **WHEN** a better physics library appears
- **THEN** it SHALL be adoptable by implementing `PhysicsServer` without changes to gameplay code,
  components, or scene assets

#### Scenario: Optional dependency is excluded
- **WHEN** a feature is disabled at configure time
- **THEN** its dependencies SHALL not be fetched, built, or linked

#### Scenario: Audio backends are separable
- **WHEN** Steam Audio is disabled
- **THEN** miniaudio SHALL still provide device I/O and mixing, and spatialisation SHALL use the
  engine's fallback path

### Requirement: What the engine builds itself
The engine SHALL implement, rather than integrate:

- the **ECS core**, scheduler, and job system — the performance model is the engine's identity
- the **renderer**: RHI, render graph, culling, pipelines, lighting, GI, and post-processing
- the **scene, prefab, and serialization** model
- the **asset pipeline** and package format
- the **AudioServer**, bus graph, voice management, and the audio importance and tiering policy —
  audio scheduling and budget policy are engine concerns that interact with the job system and ECS
- the **UI system**
- the **animation system** and animation graph
- the **networking and replication** model
- the **C ABI and Swift binding** layer
- the **editor**

These are where engine-level decisions compound, and where a general-purpose library would impose
its own architecture.

#### Scenario: Rejecting a general-purpose ECS library
- **WHEN** an existing ECS library is proposed
- **THEN** it SHALL be evaluated against the requirement that the scheduler, change detection, and
  storage layout are co-designed with the renderer and the job system, which a general-purpose
  library cannot be

#### Scenario: Rejecting middleware as the audio foundation
- **WHEN** a full audio middleware engine is proposed as the foundation
- **THEN** it SHALL be evaluated against the requirement that voice budgets, tiering, job system
  integration, and asset residency are engine policy, which a middleware engine owning its own
  threading and asset model cannot provide
