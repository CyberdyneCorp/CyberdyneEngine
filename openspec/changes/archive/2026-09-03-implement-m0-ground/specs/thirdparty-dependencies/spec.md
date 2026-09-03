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
