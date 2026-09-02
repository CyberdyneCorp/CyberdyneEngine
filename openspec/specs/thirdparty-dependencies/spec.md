# thirdparty-dependencies Specification

## Purpose

Records the engine's dependency policy and the intended dependency set: what CyberdyneEngine
builds itself, what it integrates, and why. The guiding principle is to write the code that
differentiates the engine and integrate the code that does not.

## Requirements

### Requirement: Dependency policy
A third-party dependency SHALL be adopted only when it satisfies all of:

1. **The problem is not differentiating.** Solving it better than the existing library would not
   make the engine meaningfully better.
2. **The library is mature and maintained**, with a track record in shipped software.
3. **The licence is permissive** — MIT, BSD, Apache 2.0, Zlib, or equivalent. Copyleft licences
   (GPL, LGPL for static linking) SHALL NOT be adopted for runtime code.
4. **It can be isolated** behind an engine-owned interface, so it is replaceable.
5. **Its cost is bounded** — build time, binary size, and transitive dependencies are acceptable.

A dependency SHALL be rejected when the engine's requirements would force it far from its intended
use, or when its API would leak into engine headers.

#### Scenario: Dependency is isolated
- **WHEN** a library is adopted
- **THEN** its types SHALL NOT appear in any engine header outside its backend module, so it can
  be replaced without touching consumers

#### Scenario: Copyleft is rejected
- **WHEN** a candidate library is GPL-licensed
- **THEN** it SHALL be rejected for runtime code, since it would impose obligations on games built
  with the engine

#### Scenario: New dependency is a reviewed decision
- **WHEN** a change adds a dependency
- **THEN** it SHALL go through the OpenSpec change flow recording the evaluation against these
  criteria

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
| Mesh processing | **meshoptimizer** | MIT | Simplification, cache optimisation, and meshlet/cluster generation — the algorithms beneath virtual geometry's cooker, with the hierarchy and format engine-owned |
| UV unwrapping | **xatlas** | MIT | Lightmap UV generation |
| Navigation meshes | **Recast / Detour** | Zlib | Voxelisation-based navmesh generation and the reference tiled-navmesh query implementation |
| ML inference (optional) | **ONNX Runtime** | MIT | Portable inference with broad operator coverage; a neural runtime is years of work for no differentiating benefit |
| ML inference (optional, platform) | **Core ML**, **DirectML**, **TensorRT** | Platform / proprietary SDK | Per-device optimised inference where the platform provides it |
| glTF | **cgltf** or **tinygltf** | MIT | Parsing an open spec is not differentiating |
| FBX | **ufbx** | MIT | A closed format best handled by a maintained parser |
| USD (optional, tool-time) | **OpenUSD** | Apache 2.0 | Interchange for pipelines built on it; a large dependency, so editor and cooker only |
| Animation compression | **ACL** (to evaluate) | MIT | Error-bounded clip compression is a well-solved bounded problem; the clip format stays engine-owned |
| Image codecs | **libpng**, **libjpeg-turbo**, **libwebp**, **tinyexr** | BSD/MIT | Standard codecs |
| Texture compression | **ISPC Texture Compressor** or **bc7enc**, **astc-encoder** | MIT/Apache 2.0 | BC and ASTC encoding |
| Audio codecs | **libvorbis**, **libopus**, **dr_libs** | BSD/Public domain | Formats miniaudio does not decode natively |
| Input and gamepads | **SDL3** | Zlib | Controller database and platform input coverage |
| Compression | **zstd**, **LZ4** | BSD/BSD | Best-in-class ratio and speed |
| Cryptography | **BLAKE3**, **mbedTLS** | CC0/Apache 2.0 | Never hand-roll cryptography, least of all transport security |
| Network transport (to evaluate) | A QUIC implementation, or a reliable-UDP library | Permissive | Reliability, congestion control, and encryption are solved; the replication layer above is not |
| Testing | **Catch2** or **doctest** | BSL/MIT | Test framework |
| Profiling | **Tracy** | BSD | Frame profiler with an excellent viewer |

Steam Audio SHALL be optional and capability-gated; the engine SHALL be fully functional without
it. All ML inference runtimes SHALL be optional behind `CY_ML`; platform runtimes are additionally
gated by platform availability.

**OpenUSD SHALL be optional and tool-time only.** It is materially larger than the other importers
and SHALL NOT be linked into a shipped runtime.

Where a table entry is marked *to evaluate*, the requirement is the capability, not the library:
the engine SHALL provide error-bounded clip compression, and whether that is an integrated codec or
an engine implementation SHALL be decided by the implementation change against the criteria in the
dependency policy.

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

#### Scenario: Default build carries no ML runtime
- **WHEN** the engine is built with `CY_ML` disabled
- **THEN** no inference runtime SHALL be fetched, built, or linked, and AI SHALL be fully
  functional without it

#### Scenario: Large dependency is contained
- **WHEN** USD import is enabled
- **THEN** OpenUSD SHALL be built for the editor and cooker only, and a shipped game SHALL contain
  no USD code

#### Scenario: Capability, not library
- **WHEN** the animation compression requirement is satisfied
- **THEN** it SHALL be judged on achieving bounded error with reported ratios, not on which codec
  was chosen

**Platform online services** — matchmaking, lobbies, presence, entitlement — and **voice chat**
SHALL be integration points rather than engine features. They are platform-specific, commercially
licensed, and change independently of the engine. The engine SHALL define the interfaces a session
layer needs and SHALL NOT bundle an implementation.

#### Scenario: Transport is replaceable
- **WHEN** a title ships on a platform requiring its own networking service
- **THEN** it SHALL be implementable as a transport backend, with no change above the transport
  interface

#### Scenario: Online services stay out of tree
- **WHEN** a project integrates a platform matchmaking service
- **THEN** it SHALL implement the engine's session interfaces, and the engine SHALL carry no
  dependency on that service

#### Scenario: Algorithm integrated, architecture owned
- **WHEN** virtual geometry cooking uses a third-party simplifier
- **THEN** the cluster grouping strategy, boundary constraints, error metric, hierarchy, and page
  format SHALL remain engine-owned, so the simplifier is replaceable

### Requirement: What the engine builds itself
The engine SHALL implement, rather than integrate:

- the **ECS core**, scheduler, and job system — the performance model is the engine's identity
- the **renderer**: RHI, render graph, GPU scene, culling, pipelines, lighting, GI, and
  post-processing
- the **virtual geometry system**: cluster hierarchy and its error metric, page format, GPU
  traversal and culling, the geometry cache and residency manager, the visibility buffer and
  material resolve, and the budget controller — the architecture is native to the GPU scene,
  streaming, and budgeting systems, which no external library could be
- the **VFX system**: asset model, graph compiler, particle storage, GPU simulation, event
  routing, scalability policy, and renderers — one of the few areas where an engine can still
  differentiate, and one that must be co-designed with the renderer and the frame budget
- the **AI system**: agent model, the unified behaviour graph and its compiler, perception
  scheduling and batching, the knowledge model, environment queries, smart objects, AI LOD, and
  the deterministic scheduler — the scale and determinism policy are engine concerns, and no
  third-party framework provides them
- the **animation runtime**: asset model, graph and rig compilers, pose evaluation and storage,
  the GPU pose world, animation LOD and pose sharing, motion matching, the constraint framework,
  and retargeting — animation couples to the renderer, physics, AI, and VFX, and its scale
  behaviour is engine policy
- the **UI system**: element storage, layout, styling, input routing, animation, and rendering
- the **scene, prefab, and serialization** model
- the **asset pipeline** and package format
- the **AudioServer**, bus graph, voice management, and the audio importance and tiering policy —
  audio scheduling and budget policy are engine concerns that interact with the job system and ECS
- the **networking and replication** model: replication schemas and encoding, snapshot and delta,
  interest management and its priority scheduler, network LOD and bandwidth budgeting, prediction,
  rollback, lag compensation, session management, and the network profiler — transports and crypto
  are integrated beneath it
- the **C ABI and Swift binding** layer
- the **editor**

These are where engine-level decisions compound, and where a general-purpose library would impose
its own architecture.

Algorithms with published references — noise functions, sorting networks, curl fields,
pathfinding heuristics — MAY be implemented in engine code from those references. Implementing a
published algorithm is not a dependency and does not require a manifest entry.

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

#### Scenario: Rejecting a third-party VFX runtime
- **WHEN** an external VFX runtime is proposed
- **THEN** it SHALL be evaluated against the requirement that the VFX system publishes into the
  engine's GPU scene, compiles through the engine's shader pipeline, and is governed by the
  engine's frame budget controller — coupling a third-party runtime cannot provide

#### Scenario: Integrating within an owned subsystem
- **WHEN** a proven library solves a bounded problem inside an owned subsystem — navmesh
  generation within navigation, or an inference runtime within CyberML
- **THEN** it SHALL be integrated behind the subsystem's own interface, since owning the
  architecture does not require implementing every algorithm

### Requirement: Dependency manifest
Every dependency SHALL be recorded in a single machine-readable manifest containing: name,
version, commit hash, upstream URL, licence identifier, licence file path, whether it is optional
and which feature gates it, whether a system version may be used, and a one-line justification.

The manifest SHALL be the source of truth for the build, the licence report, and the security
audit.

#### Scenario: Licence report
- **WHEN** a game is packaged
- **THEN** a licence report SHALL be generated from the manifest, listing every dependency
  actually linked into that build

#### Scenario: Dependency audit
- **WHEN** a security advisory affects a dependency
- **THEN** the manifest SHALL identify the pinned version and which features would be affected

### Requirement: Vendoring and patching
Dependencies SHALL normally be fetched by pinned commit. A dependency SHALL be vendored only when
it requires engine-specific patches, and each patch SHALL be recorded as a file with a
description and, where applicable, a link to the upstream issue or pull request.

Patches SHALL be minimised and upstreamed where possible.

#### Scenario: Patch is documented
- **WHEN** a dependency is patched
- **THEN** the patch SHALL be a discrete file with a rationale, so a future upgrade can determine
  whether it is still needed

#### Scenario: Upstream fixes the issue
- **WHEN** an upstream release incorporates a patched fix
- **THEN** the patch SHALL be removed at the next version bump

### Requirement: Runtime dependency footprint
The engine SHALL minimise what a shipped game must carry. A dependency used only by the editor,
the cooker, or the importers SHALL NOT be linked into the runtime.

Specifically: Slang, SPIRV-Cross, texture encoders, mesh processing, UV unwrapping, glTF and FBX
parsers, and Recast generation SHALL be **tool-time only**; the runtime SHALL consume their cooked
output.

#### Scenario: Runtime has no shader compiler
- **WHEN** a shipping game is inspected
- **THEN** it SHALL contain no Slang or SPIRV-Cross code, since shaders are compiled at cook time

#### Scenario: Runtime has no model parser
- **WHEN** a shipping game loads a mesh
- **THEN** it SHALL read the cooked format directly, with no glTF or FBX parser present

### Requirement: Attribution
The engine SHALL ship a complete attribution document generated from the manifest, and SHALL
expose it at runtime so games can display required notices without assembling them manually.

#### Scenario: Game credits
- **WHEN** a game needs to display third-party licences
- **THEN** the runtime SHALL provide the text for exactly the dependencies linked into that build
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
