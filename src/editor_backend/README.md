# Editor backend services

`cy::editor-backend` is the engine-owned implementation behind the editor's C ABI and live
protocol. The editor sends stable operation names and versioned byte payloads; it never links the
material compiler, renderer, Metal, or VFX implementation.

The first vertical slice supports:

- `capabilities.get` — operation discovery and first-slice target feature bits;
- `material.catalogue.get` — deterministic schema-2 envelope/catalogue-version-3 node, pin and
  typed-property catalogue with manifest identities, typed constraints, enum choices, asset-kind
  filters, semantic/stage/domain metadata and required target-capability bits. Schema 1 remains
  readable by the editor for compatibility;
- `material.validate` — parses and lowers canonical CyberGraph input;
- `material.author` — validates a canvas and returns engine-canonical `.cygraph` text;
- `material.preview.set` — validates an unsaved canvas and applies its canonical graph to a
  runtime-owned authored scene. Its schema-1 payload is two little-endian, length-prefixed UTF-8
  strings: project-relative `.cygraph` reference followed by `cymatcanvas` source;
- `material.compile` — compiles the material family and returns its cook identity, source graph
  dependency identity, program count, and stable texture-asset dependency identities;
- `terrain.evaluate` — the editor's terrain modifier stack, evaluated by `cy::terrain` over an
  author's region. It answers with heights, material texels, holes, what meshing and collision left
  open, and the regions whose navigation it marked stale since navigation was last baked. Those
  regions are the navmesh's one stale flag: `navigation.status` reports a bake stale while they
  exist, and a committed `navigation.bake` consumes them
  (`MaterialService::terrain_navigation_rebaked`, called through the host's
  `NavigationSourceRuntime::bake_committed`).
  `TerrainPreview` (`cy/editor/terrain_service.h`) holds the last region for the viewport host and
  specifies the payloads;
- `preview.create`, `preview.destroy`, `preview.parameter.update`, and `preview.reload` — isolated
  generational handles, idempotent destruction, stale-handle diagnostics, exact entity/material-slot
  target acknowledgements, and typed bool/integer/float/vector/texture parameter updates bound to an
  applied artefact generation.

- `audio.capabilities.get`, `audio.mixer.apply`, `audio.cue.preview`, `audio.preview.stop` and
  `audio.state.get` — the editor's audio tools (#29), answered from the host's
  `cy::editor::AudioAuthoring` (`include/cy/editor/audio_authoring.h`) once the host calls
  `MaterialService::set_audio`. Without one they fail with `audio.unavailable`. Every operation
  except the vocabulary answers with the audio server's state after the request: each bus's gain,
  routing, effect chain and last-block peak and RMS, the voices, Play's sources, the loaded cues, and
  the last preview's distance, attenuation gain and pan. `audio_service.h` gives the payloads.
  `AudioAuthoring` reconciles the bus graph with a `cymixer 1` asset by bus name, so a gain change
  keeps its voices, and it refuses a cyclic, dangling or overlong mixer before touching the graph.
  It loads `cycue 1` cues from a generated tone or a 48 kHz WAV. At Play it applies the project's
  `game/audio/mixer.cymixer`, names every project cue for ABI 1.3's `audio_find_cue`, and starts each
  autoplaying `cy::audio::AudioSource`. `tests/data/audio_*` holds the wire the Rust editor's
  suites read and write.

- `script.catalogue.get`, `script.compile`, `script.event.raise` and `script.state.get` — the gameplay
  graph editor's (#29, visual scripting; `include/cy/editor/script_service.h` gives the payloads). The
  catalogue is the material catalogue's schema 3 over `cy::graph`'s script vocabulary without
  `script.entry`, with property choices that are `cy::game_backend::gameplay_graph_externals()`. A
  compile always completes: the program (digests, sizes, handlers, externals, accesses, listing) or
  the diagnostics, each with its node, pin, code and the name it is about. Raise and state reach the
  host's Play through `cy::editor::ScriptPlayRuntime` once the host calls
  `MaterialService::set_scripts`, and are refused with `script.play.unavailable` otherwise.
  `tests/data/script_*` holds the wire the Rust editor's suites read and write.

- `animation.catalogue.get`, `animation.compile`, `animation.preview.set`, `animation.preview.get`,
  `animation.preview.stop`, `animation.character.set` and `animation.bake` — the animation panel's
  (#29; `include/cy/editor/animation_service.h` gives the payloads). The catalogue is the material catalogue's schema 3 over `cy::graph::pose`'s
  vocabulary, with a clip node's choices the host's preview clips. A compile always completes:
  `graph::validate`, `compile_pose`, and the authoring checks a program cannot carry (an unwired
  transition, a cut, a missing condition, an empty state, an unknown clip, an event that does not parse
  or lies outside its clip), then the states, transitions, clips and parameters or the diagnostics. The
  previews reach the host's `cy::editor::AnimationPreviewRuntime` once it calls
  `MaterialService::set_animation`, and are refused with `animation.preview.unavailable` otherwise.
  `cy::editor::AnimationPreview` (`include/cy/editor/animation_preview.h`, built with `CY_ANIMATION`)
  is that runtime: a twelve-joint preview character whose pose `cy::animation` evaluates — a state
  machine advanced from its entry state in sixtieths of a second, or one clip sampled — with the events
  each clip's node authors, and the skinning matrices a host draws it with. Its character
  (`include/cy/editor/animation_character.h`) is the built-in mannequin or, after
  `animation.character.set`, a project's imported one: the cooked skeleton, skinned mesh and clips the
  importer wrote, read through the host's `cy::editor::AnimationAssetSource`, a clip cooked for another
  skeleton refused by name. `animation.bake` reaches the host's `cy::editor::AnimationBakeRuntime`
  (`MaterialService::set_animation_baker`); `cy::editor::AnimationRigBaker`
  (`include/cy/editor/animation_rig.h`) is that cook: the graph compiled for the character it names,
  its program and clips written as cooked assets with the graph's events in the clips, and a `cyrig 1`
  manifest a host's Play loads.
  `tests/data/animation_*` holds the wire the Rust editor's suites read and write; the suite is
  `integration.editor_backend_animation`, which imports its project character from an FBX with the
  real importer (`tests/animation_character_fixture.h`).

Requests are copied at submission, identified by nonzero request IDs, cancelled cooperatively, and
publish exactly one terminal event. Payload schemas are versioned independently of ABI 1.2 and of
the live message framing.

Preview operations may be connected to a runtime-owned `MaterialPreviewRuntime`. Compiled
materials are published to that interface before their identities are returned, and create,
reload, parameter-update and destroy acknowledgements are emitted only after the runtime accepts
the operation. The interface carries engine-owned compiled material and stable binding data; no
renderer, Metal or compiler type crosses the C ABI/live protocol or enters the Rust editor.
Hosts without that interface reject `preview.create` with `preview-runtime-unavailable` and omit
the preview feature bit rather than reporting a protocol-only echo as a visible reload.
`material.preview.set` uses a separate `MaterialAuthoringRuntime` host seam; it never writes the
graph asset. Hosts without an authored scene reject it with `material.preview.unavailable`.

## Navigation operations (issue #28)

`NavigationService` (`include/cy/editor/navigation_service.h`) is the engine side of the
navigation authoring editor. The engine runs the tiling loop, owns one `NavMesh` per navigation
world, computes the source fingerprint and answers every query; the editor only sends requests and
records what comes back. The runtime host supplies the data through the pure-virtual
`NavigationSourceRuntime` seam: `worlds`, `gather` (world-space triangles with per-triangle layer,
tag and area, plus the surface and area volumes), `obstacles`, `links`, `store_bake` /
`load_bake` (the content-addressed `.cynavmesh` sidecar) and `pick_ray`.

Every operation uses schema 1 and little-endian payloads. The settings block is seven `f32`
(agent radius, agent height, max slope, step height, cell size, cell height, tile size), `u64`
layers, `u64` tags and a `u8` back end (1 Engine, 2 Recast; Automatic is refused).

| Operation | Request | Result |
|---|---|---|
| `navigation.bake` | `u32` world, settings | one PROGRESS per tile across polls (`u32` done, `u32` total, tile), then COMPLETED: `u32` world, `u64` fingerprint, `u64` bake identity, text sidecar path, report (six `u32` counters, `u64` duration, `u8` back end, `u32` tiles built, `u32` tiles empty), `u32` link failures, `u32` tile count and each tile (`i32` x, z, layer, `u32` polys, `u8` empty, `u64` digest) |
| `navigation.status` | `u32` world, settings, `u64` saved identity, `u64` saved fingerprint | `u32` world, `u8` baked, `u8` stale, `u64` current fingerprint, `u64` saved fingerprint, `u64` identity, `u32` resident tiles, report, `u32` link failures, `u8` sidecar missing. A saved identity the session does not hold is restored from its sidecar through `load_bake`, with the area costs of the host's current sources, which is how an undone bake or a reopened world comes back. When `load_bake` finds no sidecar the answer is `baked = 0`, `sidecar missing = 1` and the current fingerprint, and the session holds no mesh for the world; a sidecar that does not decode fails with `navigation.bake.load-failed` |
| `navigation.update` | `u32` world, dirty `Aabb` (six `f32`) | `u32` world, `u64` fingerprint, `u64` identity, rebuilt tiles and obstacle-marked tiles (each `u32` count plus `i32` x, z, layer), `u32` link failures. Tiles are rebuilt only when the recomputed fingerprint changed: the tiles under the dirty box that the surface region covers are rebuilt and those it no longer covers are removed (`rebake_surface_tiles`), unless the mesh was not current for the previous sources (a stale restore) or the geometry's height range moved, in which case the whole surface region is rebaked. Obstacles and links are re-synced from the seam without a rebuild |
| `navigation.path.query` | `u32` world, start, end, extents (`Vec3` each) | `u8` found, `u8` partial, `u8` budget exceeded, `f32` cost, `u32` nodes expanded, `u32` points and each (`Vec3`, `u8` enters link) |
| `navigation.flowfield.query` | `u32` world, target `Vec3`, region `Aabb`, `f32` cell | `u32` width, `u32` depth, `f32` cell, `u32` unreachable, per cell `f32` dx, `f32` dz, `u8` reachable (at most 65536 cells) |
| `navigation.point.pick` | `u32` world, `u32` viewport, `u64` frame, `f32` x, `f32` y | `u8` hit, `Vec3` point, `u64` polygon, `f32` distance: the host's `pick_ray` intersected with the navmesh polygons |
| `navigation.overlay.set` | `u32` world, `u32` `NavDebugFlags` bits | `u32` world, `u32` flags |
| `navigation.clear` | `u32` world | `u32` world. Drops the world's mesh, so later queries answer `navigation.world.unbaked`; clearing a world with no mesh succeeds. The host sends it when the document stops recording a bake |

A session has one pending request. A second request while one is pending is answered with a FAILED
`navigation.busy` event, so every request still ends in exactly one terminal event. A cancelled
bake ends in CANCELLED and leaves the world's previous mesh in place. Failure codes are stable:
`<op>.unavailable` (no seam; the operations are then also absent from `capabilities.get`),
`navigation.busy`, `navigation.request.malformed`, `navigation.schema.unsupported`,
`navigation.operation.unsupported`, `navigation.settings.invalid`, `navigation.world.unknown`,
`navigation.world.unbaked`, `navigation.world.limit`, `navigation.surface.missing`,
`navigation.source.failed`, `navigation.bake.failed`, `navigation.bake.store-failed`,
`navigation.bake.load-failed`, `navigation.update.failed`, `navigation.path.failed`,
`navigation.flowfield.too-large`, `navigation.flowfield.failed`, `navigation.point.pick.ray-failed`
and `navigation.overlay.invalid`. With a seam, `capabilities.get` sets feature bit
`kNavigationFeature` (0x100).

## One binding, several services

The host binds exactly one `EditorServiceBackend`. `CompositeEditorService`
(`include/cy/editor/composite_service.h`) is that backend when there are several: up to eight
`(prefix, backend)` routes, longest-prefix routing, one child session per backend, round-robin
polling, and a `capabilities.get` answered with the union of every child's list (duplicates
removed, feature bits OR-ed). A child busy with this session's request is asked for its
capabilities once it is idle. An operation no route matches fails with `operation-unsupported`.
`child_session` hands a host the child session for a child's own accessors, such as
`MaterialService::vfx_preview_world` or `NavigationService::mesh` and `overlay`. MaterialService
is unchanged by it.

The navigation and composite suites are `integration.editor_backend_navigation`.
