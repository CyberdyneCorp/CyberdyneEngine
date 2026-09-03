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
| Windowing, input and gamepads | **SDL3** | Zlib | Window and event handling, graphics surface creation, the controller database, rumble and hot-plug, across every desktop platform |
| Compression | **zstd**, **LZ4** | BSD/BSD | Best-in-class ratio and speed |
| Cryptography | **BLAKE3**, **mbedTLS** | CC0/Apache 2.0 | Never hand-roll cryptography, least of all transport security |
| Network transport (to evaluate) | A QUIC implementation, or a reliable-UDP library | Permissive | Reliability, congestion control, and encryption are solved; the replication layer above is not |
| Testing | **doctest** | MIT | Test framework, chosen for compile time: the unit budget is under a millisecond per test across thousands of tests, and compile time is a tax every contributor pays on every build |
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

**SDL3 sits beneath `DisplayServer` and the input backend, not beside them.** It is the initial
desktop implementation of engine-owned interfaces, and no SDL type SHALL appear above
`platform/`. Native per-platform backends — Win32, Cocoa, and X11 with Wayland — are **planned**
rather than assumed, and delivering one before 1.0 is how the abstraction is validated against a
second implementation rather than against a guess.

#### Scenario: SDL does not leak above the platform layer
- **WHEN** the engine is compiled
- **THEN** no header outside `platform/` SHALL include an SDL header, and the check SHALL be a
  build gate

#### Scenario: A second backend validates the interface
- **WHEN** a native backend is implemented for one desktop platform
- **THEN** it SHALL require no change in `src/core/`, `src/ecs/`, `src/servers/`, or `src/scene/`

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
- the **material compiler**: the authoring model, the material IR and its optimisation passes, the
  closure model and its lowering to shading models, parameter classification, quality tier
  generation, cost analysis, material classification and binning, and the GPU material table —
  this is where material cost is actually decided, and it is inseparable from the GPU scene and
  the renderer's budget model. The engine does **not** implement a shader optimiser or backend
  code generator; it produces good input to somebody else's.
- the **renderer budget arbiter**, the temporal framework, and the **denoising framework** — frame
  cost allocation, history management, and edge-aware reconstruction are cross-subsystem policy
  that no library can hold. A vendor or machine-learning denoiser MAY be integrated as a backend
  behind the engine's interface.
- the **illumination architecture**: the GI scene and its representations, the surface cache, the
  radiance cache and its scheduler, the tracing tiers and their selection, the confidence model,
  the resolve, and the illumination budget policy — hybrid GI is a scheduling problem over the
  engine's own scene, streaming, geometry hierarchy, and budget, and a library that owned any of
  those would own the engine
- the **virtual geometry system**: cluster hierarchy and its error metric, page format, GPU
  traversal and culling, the geometry cache and residency manager, the visibility buffer and
  material resolve, and its budget controller — the architecture is native to the GPU scene,
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
- the **virtual texture and virtual shadow systems** and the **residency policy layer** above them:
  address spaces, page tables, physical caches, GPU feedback, runtime page producers, receiver-driven
  shadow page marking, the shadow page cache and its invalidation, and the shared scoring, budgets
  and deadline propagation — these are the systems through which every other subsystem's data
  reaches the frame. Image codecs, block-compression encoders, and GPU decompression facilities are
  integrated beneath them.
- the **procedural generation framework**: typed spatial datasets, the graph compiler and its
  intermediate representation, deterministic derivation and stable generated identity, region
  execution with dependency and radius invalidation, output adapters, the override and provenance
  model, and macro state materialisation — its value is entirely in integration with fields, terrain,
  foliage, world streaming, the build graph and the derived data cache
- the **atmosphere and weather architecture**: the environment state model, weather cells and storm
  phenomena, the wind field and its composition, precipitation and accumulation fields, cloud
  representation driven by weather state, and the separation of authoritative environmental state
  from visual detail. Scattering and cloud rendering follow published research rather than being
  invented, and noise and transform libraries are integrated beneath them.
- the **environment systems**: the environment field substrate, terrain representation, streaming
  and its bridge to virtual geometry, terrain materials and deformation, the foliage runtime and
  its promotion model, procedural placement, the wind field, water bodies and their backends, the
  shoreline, and buoyancy integration — these are world-scale streamed datasets coupled to the
  engine's own partitioning, geometry, material, and budget systems. Published algorithms —
  spectral wave synthesis and its transforms, erosion, procedural noise — MAY be implemented from
  references or integrated as bounded libraries beneath these interfaces.
- the **scene, prefab, and serialization** model, including the prefab compiler, exposed
  parameters, override addressing, and entity templates
- the **input action model** — users and device ownership, mapping contexts, bindings, processors,
  modifiers, triggers, rebinding, and accessibility — and the **camera system**: rig graphs and their
  compiler, the camera stack and blending, framing, collision and occlusion policy, the impulse bus,
  and the derivation of render views, listeners and streaming sources from one evaluated state.
  Device backends are integrated beneath the first; the renderer consumes the second.
- the **simulation integrity layer**: the simulation clock and commit boundary, determinism profiles
  and their enforcement, deterministic ordering and hashing, random stream derivation, the
  determinism validator and its divergence capture, the command log and replay container, snapshot
  kinds and the side-effect ledger, and the save model with its journal, atomicity and migration.
  Compression codecs and cryptographic implementations are integrated beneath them; the reproducible
  execution model is not something a library can supply.
- the **gameplay framework**: the session and service lifetime model, rules and session state,
  participants, teams and affiliations, ownership, control bindings, the gameplay command stream
  and its validation, gameplay tags, time domains and the simulation clock, spawning, and gameplay
  features — this is the layer that determines whether a game's structure fits the engine's
  execution model, and adopting an external one would adopt its object model with it
- the **persistent world**: partitioning, cell identity and cooking, the streaming scheduler and
  its budgets, residency and activation states, world layers, persistent entity identity and
  cross-cell references, the persistence overlay, world HLOD, and representation tiers — this is
  the layer every other subsystem streams against, and adopting an external world model would
  adopt its object model with it
- the **asset pipeline** and package format
- the **AudioServer**, bus graph, voice management, and the audio importance and tiering policy —
  audio scheduling and budget policy are engine concerns that interact with the job system and ECS
- the **networking and replication** model: replication schemas and encoding, snapshot and delta,
  interest management and its priority scheduler, network LOD and bandwidth budgeting, prediction,
  rollback, lag compensation, session management, and the network profiler — transports and crypto
  are integrated beneath it
- the **C ABI and Swift binding** layer
- the **foundations**: persistent type and field identity and its manifest, the reflection
  generator's engine-specific output, the serialization formats and the migration model, the task
  scheduler and its coroutine integration, and the memory domain, budget, pressure and epoch
  model — these are the contracts every other subsystem encodes into its data and its scheduling.
  Beneath them, a C++ compiler frontend for parsing, a general-purpose heap allocator chosen by
  benchmark, and compression codecs SHALL be integrated rather than written.
- the **diagnostics infrastructure**: the trace schema and transport, buffering and loss policy,
  the profiler's engine-specific views, structured logging, the rolling buffer and automatic capture,
  crash artefacts and breadcrumbs, and reproduction — because every subsystem's diagnostics must
  share one timeline, and a general-purpose tracing library cannot know about archetypes, residency,
  ticks, or determinism hashes. Platform crash handling, symbol formats, and compression are
  integrated beneath it.
- the **sequencing system**: the timeline model, the compiler and its interval indexing, exact time
  and clock domains, binding resolution, arbitration, seek and skip semantics, the preload plan and
  streaming source, and the subsystem adapters — its value is entirely in orchestrating the engine's
  own camera, animation, audio, effect, environment and gameplay systems, which an external timeline
  library would necessarily duplicate
- the **ability module** and the **graph infrastructure and its gameplay lowering**: compiled ability
  and graph programs, compact ECS state, the typed intermediate representations, determinism
  auditing, and semantic diff — their value is that they compile to the engine's own execution model
  rather than adding a second one
- the **editor**, including its document and transaction model, undo, semantic diff and merge, the
  live edit compiler and the live bridge, and the source control abstraction — the transaction log
  is one mechanism serving undo, autosave, crash recovery, diff and live editing at once, which no
  external library could supply
- the **project, module and plugin graph** and the **build pipeline**: layering enforcement, plugin
  lifecycle and resolution, the build graph and its derivation keys, the build service, the derived
  data cache, cooking, the package format, and patch manifests. Compilers, shader toolchains,
  compression codecs, cryptographic implementations, and source control systems are integrated
  beneath them.

These are where engine-level decisions compound, and where a general-purpose library would impose
its own architecture.

Shader toolchains (Slang, SPIR-V tools, DXC where needed, platform shader compilers), GPU vendor
upscalers, and GPU capture tools SHALL be integrated rather than built, behind engine-owned
interfaces that expose no vendor types.

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

#### Scenario: The material IR is owned; the shader backend is not
- **WHEN** the shader toolchain is upgraded or replaced
- **THEN** the material IR, its optimisation passes, and the cost model SHALL be unaffected, since
  the engine owns the material compiler and integrates the shader compiler

#### Scenario: Rejecting a drop-in GI solution
- **WHEN** an external global illumination library is proposed
- **THEN** it SHALL be evaluated against the requirement that illumination reads the engine's GPU
  scene, consumes the virtual geometry hierarchy at its own error target, fills a surface cache
  from engine-compiled secondary material programs, and holds an allocation from the engine's
  budget arbiter — coupling a self-contained solution cannot provide

#### Scenario: Rejecting an external world model
- **WHEN** an external world or streaming framework is proposed
- **THEN** it SHALL be evaluated against the requirement that cells cook into the engine's own
  archetype layout, that activation is a bulk copy into the engine's ECS chunks, and that streaming
  budgets are held alongside the renderer's — coupling an external framework cannot provide

#### Scenario: Algorithm integrated, environment architecture owned
- **WHEN** a proven transform or erosion library is used for wave synthesis or terrain generation
- **THEN** it SHALL sit beneath the engine's own water body, field, and terrain interfaces, and the
  representation, streaming, and budget behaviour SHALL remain engine-owned

#### Scenario: Parsing is integrated, metadata is owned
- **WHEN** the reflection generator parses annotated C++
- **THEN** it SHALL use an established compiler frontend, while the identity model, metadata
  format, and generated output remain engine-owned

#### Scenario: The general heap is a benchmark result
- **WHEN** a general-purpose allocator is adopted
- **THEN** it SHALL be selected by measurement on target platforms and remain replaceable behind
  the allocator interface, rather than an engine-written implementation

#### Scenario: Encoders integrated, virtualisation owned
- **WHEN** a block-compression encoder or image codec is adopted
- **THEN** it SHALL sit beneath the engine's own page format, address space, and residency policy,
  which remain engine-owned and replaceable independently of it

#### Scenario: Rejecting an external build system as the model
- **WHEN** a general-purpose build system is proposed to own the pipeline
- **THEN** it SHALL be evaluated against the requirement that derivation keys span code, assets,
  shaders, pages and packages in one graph, that outputs are content-addressed for patching, and
  that the editor is a live client of the same graph — coupling an external system cannot provide,
  though it MAY be used to build individual native modules

#### Scenario: The plugin boundary reuses the engine ABI
- **WHEN** a binary plugin interface is required
- **THEN** it SHALL use the engine's existing stable C ABI rather than introducing a second
  versioned boundary to maintain

#### Scenario: Rejecting an object-oriented gameplay framework
- **WHEN** an established gameplay framework is proposed as the model
- **THEN** it SHALL be evaluated against the requirement that gameplay concepts are ECS data and
  scoped services rather than a runtime object hierarchy, that control is many-to-many and
  channelled, and that human, AI, network and replay share one command stream — coupling an
  object-per-entity framework cannot provide

#### Scenario: Device backends integrated, action model owned
- **WHEN** a controller database or device backend is adopted
- **THEN** it SHALL supply normalised timestamped events beneath the engine's own user, context,
  binding and trigger model, which remains engine-owned

#### Scenario: Rejecting an external save or replay framework
- **WHEN** an external serialisation or replay library is proposed to own this layer
- **THEN** it SHALL be evaluated against the requirement that snapshots key off the engine's own
  commit boundary, that state classification drives hashing, rollback, replay and save from one
  declaration, and that rollback and save deliberately use different encodings — coupling an
  external framework cannot provide, though codecs and cryptography beneath it SHALL be integrated

#### Scenario: Rejecting an external procedural framework
- **WHEN** an external procedural generation tool is proposed to own this layer
- **THEN** it SHALL be evaluated against the requirement that generation is a set of derivations in
  the engine's build graph, that outputs are the representations its subsystems use, and that
  generated identity is stable enough to carry author overrides and persistence — coupling an
  external tool cannot provide, though noise, sampling and geometry algorithms SHALL be integrated

#### Scenario: Rejecting a general-purpose tracing library as the model
- **WHEN** an external profiler or tracing framework is proposed to own diagnostics
- **THEN** it SHALL be evaluated against the requirement that one timeline carries archetype,
  residency, tick, determinism and budget events from every subsystem, and that captures, crash
  artefacts and reproductions link to each other — coupling a general library cannot provide, though
  platform crash handling and compression SHALL be integrated beneath it

#### Scenario: Rejecting an interpreted scripting runtime
- **WHEN** an external visual scripting or embedded scripting runtime is proposed
- **THEN** it SHALL be evaluated against the requirement that graphs compile to systems over
  archetypes with generated state and scheduler access declarations, with no interpreter instance per
  entity — which an engine-external runtime cannot provide

#### Scenario: Rejecting an external timeline runtime
- **WHEN** an external sequencing or timeline library is proposed
- **THEN** it SHALL be evaluated against the requirement that a sequence produces batched commands
  for engine-owned subsystems, crosses the gameplay command boundary for authoritative change, and
  compiles a preload plan the residency layer consumes — coupling an external runtime cannot
  provide without reimplementing camera, animation, audio and environment control

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

### Requirement: Rust editor dependencies
The editor's Rust dependencies SHALL be governed by the same policy as the engine's C++ dependencies:
declared, pinned, licence-reviewed, vendored or reproducibly acquired, and justified.

The Rust ecosystem's low cost of adding a dependency SHALL NOT be treated as a reason to add them
freely. The editor SHALL prefer a small, audited set, and SHALL account for **transitive** dependency
count in the decision, since that is where the cost actually accrues.

The **interface toolkit** SHALL be an integrated dependency selected on measurement, kept behind the
editor's own abstractions as required by `editor-rust-application`, and replaceable. It SHALL NOT
appear in plugin-facing or protocol interfaces.

The editor SHALL build itself, rather than integrate, the following: its document and transaction
model, its command and service architecture, its view model layer, its engine SDK, its protocol
client, and its viewport transport. These are where the editor's behaviour is decided.

A Rust dependency SHALL be evaluated for maintenance status, licence, `unsafe` usage, build time
cost, and whether it can be replaced without changing editor architecture.

#### Scenario: The toolkit is an integration, not an architecture
- **WHEN** the interface toolkit is evaluated
- **THEN** it SHALL be judged replaceable behind editor abstractions, and a toolkit that would own
  the editor's state or command model SHALL be rejected

#### Scenario: Transitive cost counts
- **WHEN** a crate is proposed
- **THEN** its transitive dependency set SHALL be part of the evaluation, not only the crate itself

#### Scenario: Reproducible acquisition
- **WHEN** the editor is built from a clean checkout
- **THEN** its dependency set SHALL be reproducible from pinned versions, as for the engine
