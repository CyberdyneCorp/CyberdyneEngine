# CyberdyneEngine

An open-source game engine. **C++20** core, **Swift** for gameplay, **Rust** for the editor.

Inspired by Godot's server architecture and scene ergonomics, Unity's component composition and
prefab workflow, and Unreal's render graph and tooling ambition — but not a port of any of them.

![The M11.c beauty shot: the Colonnade, rendered by the engine](docs/design/images/m11c-beauty-shot.png)

*`samples/12-beauty`, the M11.c closing artefact: 31 instances and three materials authored on the
editor's node-graph canvas, compiled by the material compiler and bound as BC7 and BC5 blocks, with
996 ember particles and their trails in the transparent stage and 0 validation errors
([what was authored and what the renderer produced](docs/design/beauty-shot.md)).*

> **Status: M0 through M11.d.5 are closed** — [M11.d.5 · Backends](docs/ROADMAP.md#m11d5--backends)
> closed on the same golden image rendered and matched on Vulkan, Metal and D3D12 hardware. Now:
> [M11.e · Ship](docs/ROADMAP.md#m11e--ship) and 1.0, then [M12 · The Game](docs/ROADMAP.md#m12--the-game),
> an RTS written in Swift that proves the engine by using it; its [test map](#rts-showcase) already runs.

[`openspec/specs/`](openspec/specs/README.md) holds **76 capabilities, 1,262 requirements and 2,837
scenarios** — the contract the implementation must satisfy. Today 17 are Complete, 55 Working, 3
Seed and 1 not started ([`status.yaml`](docs/roadmap/status.yaml), reported by `just roadmap-status`).

---

## Global illumination

CyberGI lights a frame in real time on the GPU: screen probes gather from a world-space radiance
cache, tracing with hardware ray queries where the device has them and a software distance-field
tracer where it does not. It is held to the engine's own path tracer, and every figure below is
measured at 1920x1080 on an NVIDIA GeForce RTX 5060 (Vulkan).

![The Workshop Hall: GI off, CyberGI, the path tracer, and the error map](docs/design/images/gi-hall-comparison.png)

*The Workshop Hall (`content/beauty/hall.cyshot`): GI off, CyberGI, the path tracer and the error
map. The lamp has no light of its own; its emissive glow reaches the bench through the GI.
[A camera swing through the hall](docs/design/videos/gi-hall-orbit.mp4).*

![The Colonnade: GI off, CyberGI, the path tracer, and the error map](docs/design/images/gi-colonnade-comparison.png)

| Scene | GI on the GPU | Error against the path tracer | The path tracer's own noise |
|---|---|---|---|
| Workshop Hall | 3.78 ms hardware · 5.40 ms software | 0.148 | 0.088 |
| Colonnade | 2.73 ms hardware | 0.023 | 0.014 |
| Sealed room behind a 10 cm wall | — | zero light leaked, through both tiers | — |

The same device GI lights the editor's viewport and a Swift game, which tunes it through ABI 1.9.
So far it has run on Vulkan only; its Metal and DXIL shaders are compiled but not yet run on a device.

| | |
|:---:|:---:|
| ![The editor viewport without GI, lit by the device GI, and in four device debug views](docs/design/images/editor-gi-live.png) | ![The RTS sample without GI, with it, and a beacon put out and lit again from Swift](docs/design/images/rts-api-gi.png) |
| **Editor** — the viewport without GI, lit by it, and four of its debug views, all on the device | **Swift game** — `samples/13-rts-api` lit by the GI; its Swift code puts a beacon out and lights it again |

→ [**Global illumination guide**](docs/guides/global-illumination.md) — how it works, the
[measurements](docs/guides/global-illumination.md#7b-beside-the-path-tracer-as-a-picture-a-sunlit-room-a-workshop-hall-and-the-colonnade)
and [turning it on in your game](docs/guides/global-illumination.md#6g-turn-on-gi-in-your-game).

---

## RTS showcase

**The Frontier** (`content/rts/frontier.cymap`) is a 128 m test map — hills, a lake, a forest, a
village, a keep, farms and an enemy camp — cooked from one text source by the engine's terrain and
foliage code and drawn under the Swift RTS of `samples/13-rts-api`.

![The Frontier under the Swift RTS: skinned workers, painted terrain, a keep, blue's fog of war and the HUD](docs/design/images/rts-map-game.png)

*The player's view at 1920x1080: skinned, animated units on painted terrain, a village and a forest,
blue's fog of war over the whole map (black where blue never looked, grey where it looked before),
the game's HUD and minimap, with shadows, ambient occlusion and the GI. 12.1–13.0 ms a frame from
recording to readback on an RTX 5060 (Vulkan), within the 16.7 ms budget `smoke.rts_api_map` asserts.*

| | |
|:---:|:---:|
| ![The same frame with no features, with shadows, with AO and GI, and with fog and HUD](docs/design/images/rts-map-features.png) | ![The overview camera over the level, with the fog of war and the HUD](docs/design/images/rts-map-overview.png) |
| **Feature by feature** — none, shadows, AO and GI, then fog and HUD | **Overview** — the video's camera; [watch the 7 s orbit](docs/design/videos/rts-map.mp4) with the workers and the company walking |
| ![The battle at frames 180, 330, 960 and 1150: the armies closing, the melee with arrows and catapult shot, the watchtower's fall, the barracks burning](docs/design/images/rts-battle.png) | ![The topographic minimap at frame 1150: hillshading, contours, the lake, roads, the fog and the battle's icons](docs/design/images/rts-battle-minimap.png) |
| **Battle** — forty units fought in fixed point in the lockstep path, the same digest on all four CI legs; [watch it](docs/design/videos/rts-battle.mp4) | **Topographic minimap** — hillshading, contours, water and roads under blue's fog, the units it sees as icons |

→ [**RTS map guide**](docs/guides/rts-map.md) · [RTS combat guide](docs/guides/rts-combat.md) · [fog of war guide](docs/guides/fog-of-war.md)

---

## Game demos

Two small games written in Swift against the C ABI, each drawn by the engine's own frame with
shadows, ambient occlusion, the device GI and the temporal upscaler. The game decides everything;
the C++ host builds the level, binds the engine's adapters and draws.

### First person

**`samples/15-first-person`** is a shooter in a small arena: a character that walks, sprints, jumps
and crouches, a first-person camera with recoil and a weapon view, hitscan shots by physics raycast,
crates pushed by impulses and targets driven by the engine's AI.

![The first shot on a target: the flash, the burst at the barrel and the sparks on its chest](docs/design/images/first-person-shot.png)

*A shot on a target: the muzzle flash, the burst at the barrel and the sparks on its chest, played
through ABI 1.15's effects and decals. 6.19 ms a frame at 1080p with the Quality upscaler on an RTX
5060 (Vulkan), against the 16.7 ms `smoke.first_person_render` asserts.
[Watch the 12 s session](docs/design/videos/first-person-demo.mp4).*

| | | |
|:---:|:---:|:---:|
| ![A crate a shot threw](docs/design/images/first-person-crate.png) | ![The floor struck: a scorch mark, and sparks bouncing](docs/design/images/first-person-floor-sparks.png) | ![Down the sights on a target](docs/design/images/first-person-sights.png) |
| **Impulses** — a crate a shot threw, through the physics world | **What a shot leaves** — a scorch mark, and sparks bouncing on the floor | **Sights** — the weapon view raised onto a target |

→ [**First-person demo guide**](docs/guides/first-person-demo.md) · [weapon view guide](docs/guides/first-person-weapon-view.md)

### Third person

**`samples/14-third-person`** is a hero on a cooked hillside: locomotion through a synchronised blend
space with its feet placed on the ground the physics world reports, stairs, slopes and jumps, a
camera rig whose collision node keeps it in front of walls, and footsteps heard on stone and grass.

![The run: on the stairs, a jump on the platform, the camera pulled in by the north wall, down the ramp, a jump onto the hill, and the view back down](docs/design/images/third-person-run.png)

*The run at 1920x1080: the stairs, a jump on the platform, the camera pulled in by the north wall,
down the ramp, a jump onto the hill and the view back down. 7.9 ms a frame on average on an RTX 5060
(Vulkan), within the 16.7 ms `smoke.third_person_render` asserts.
[Watch the run](docs/design/videos/third-person.mp4), with the game's own sound.*

| | | |
|:---:|:---:|:---:|
| ![The stairs with no features, with shadows, with AO and GI, and with the upscaler](docs/design/images/third-person-features.png) | ![The camera pulled in by the north wall](docs/design/images/third-person-wall.png) | ![Down the ramp towards the hill](docs/design/images/third-person-ramp.png) |
| **Feature by feature** — none, shadows, AO and GI, then the upscaler | **Camera collision** — the rig pulled in by a wall | **Slopes** — down the ramp, feet on the ground |

→ [**Third-person demo guide**](docs/guides/third-person-demo.md) · [`content/third-person/`](content/third-person/README.md)

---

## Gallery

Every image below is a frame the engine rendered and read back, committed under
[`docs/design/images/`](docs/design/images/). Most renderer features ship with an off/on pair in
their module README.

![Ungraded, warm and cool grades of the same frame](docs/design/images/grading-beauty-triptych.png)

*Colour grading: the beauty shot ungraded, warm and cool ([`src/rendering/grading/`](src/rendering/grading/README.md)).*

![The same 4K beauty-shot crops with no upscaler, native AA, and the Quality, Balanced and Performance modes](docs/design/images/upscale-beauty-crops-2160.png)

*Temporal upscaling at 3840x2160, the same crops with no upscaler, native AA, Quality (67 %),
Balanced (58 %) and Performance (50 %): the beauty shot with GI takes 16.47 ms of GPU time with native AA and
8.56 ms in Performance, 40.3 dB from native AA, on an RTX 5060 ([guide](docs/guides/temporal-upscaling.md)).*

| | | |
|:---:|:---:|:---:|
| ![Water shading at the shore](docs/design/images/water-shading-shore-on.png) | ![Aerial perspective over distant terrain](docs/design/images/aerial-perspective-on.png) | ![Soft and contact shadows](docs/design/images/soft-shadows-beauty-on.png) |
| **Water** — shading in the shallows ([`samples/10-world`](samples/10-world/README.md)) | **Aerial perspective** — the atmosphere's own table ([`samples/10-world`](samples/10-world/README.md#aerial-perspective)) | **Soft shadows** with contact shadows ([`contact_shadows/`](src/rendering/contact_shadows/README.md)) |
| ![A thousand coloured lights at 1080p](docs/design/images/many-lights-1080p.png) | ![Volumetric fog](docs/design/images/volumetric-fog-beauty-on.png) | ![Depth of field focused on the sphere](docs/design/images/depth-of-field-beauty-sphere.png) |
| **Many lights** — 1,000 lights at 1080p: 0.19 ms to assign on the device, 0.70 ms of shading ([guide](docs/guides/many-lights.md)) | **Volumetric fog** and light shafts ([`fog/`](src/rendering/fog/README.md)) | **Depth of field** — near and far fields ([`depth_of_field/`](src/rendering/depth_of_field/README.md)) |
| ![Decals](docs/design/images/decals-beauty-on.png) | ![Motion blur](docs/design/images/motion-blur-beauty-on.png) | ![A room lit by a rect, a disc, a sphere and a tube light](docs/design/images/area-lit-room.png) |
| **Decals** before the light loop ([`decals/`](src/rendering/decals/README.md)) | **Motion blur** from prepass velocity ([`motion_blur/`](src/rendering/motion_blur/README.md)) | **Area lights** — a rect, a disc, a sphere and a tube, shaded with LTC ([guide](docs/guides/many-lights.md#2-area-lights)) |
| ![A strategy HUD and the developer console over selected units](docs/design/images/rts-hud-2x.png) | ![The widget gallery: buttons, toggles, radios, a slider, text fields, tabs and a list](docs/design/images/ui-widget-gallery.png) | ![The widget gallery with a modal dialog holding focus](docs/design/images/ui-widget-gallery-dialog.png) |
| **HUD** — selection outlines, a squad panel, a minimap and the developer console in one interface pass ([UI guide](docs/guides/ui.md)) | **Widget toolkit** — 65 widgets, 64 µs a frame on the CPU, styled by `.cyss` and loaded from `.cyui` documents ([UI guide](docs/guides/ui.md#7-the-widget-toolkit)) | **Modal dialog** — it blocks the layer below and holds keyboard and gamepad focus; text is shaped and drawn from a distance-field atlas ([text guide](docs/guides/text.md)) |
| ![Vulkan](docs/design/images/m11d5-three-backends-vulkan.png) | ![Metal](docs/design/images/m11d5-three-backends-metal.png) | ![D3D12](docs/design/images/m11d5-three-backends-d3d12.png) |
| **Vulkan** — NVIDIA GeForce RTX 5060 | **Metal** — Apple M2 Max, max delta 1 | **D3D12** — AMD Radeon RX 6900 XT, max delta 1 |
| ![The editor with a live textured FBX on Metal](docs/design/images/editor-live-textured-fbx-metal.png) | ![The material graph editor](docs/design/images/editor-material-graph-nodes-metal.png) | ![The open-world sample on an iPhone 16](docs/design/images/ios-open-world-iphone.png) |
| **Editor** — a live FBX import on Metal ([`editor/`](editor/README.md)) | **Material graph** on the editor's canvas ([`material/`](src/rendering/material/README.md)) | **iOS** — open world on an iPhone 16 at 60 FPS ([building](docs/guides/building.md#ios)) |
| ![The editor's animation tools with a character posed mid-blend](docs/design/images/editor-animation-window.png) | ![A gameplay graph paused at a breakpoint](docs/design/images/editor-gameplay-graph-debugger.png) | ![Virtual geometry clusters at 4 px error](docs/design/images/virtual-geometry-clusters-4px.png) |
| **Animation editor** — a character the engine posed mid-blend, skinned and shadowed in the viewport ([`editor/`](editor/README.md#the-animation-editor)) | **Visual scripting** — a gameplay graph paused at a breakpoint, with watches ([guide](docs/guides/visual-scripting.md#debugging-in-play)) | **Virtual geometry** — 4.48 M triangles, one colour per cluster ([write-up](docs/design/virtual-geometry.md)) |
| ![A Mixamo character mid-run, skinned and drawn in the engine's frame](docs/design/images/animated-character-frame.png) | ![The 500-model, 100-emitter RTS load on an iPhone 16](docs/design/images/ios-rts-load-iphone.png) | ![A generated world from M10](docs/design/images/m10-world.png) |
| **Animation** — a Mixamo character from four FBX files, a skinned draw in the engine's frame casting its shadow ([video](docs/design/videos/animated-character.mp4) · [animation guide](docs/guides/animation.md)) | **GPU skinning** — 500 skinned models and 100 GPU emitters on an iPhone 16 ([building](docs/guides/building.md#rts-capacity-scene)) | **World** — generated from a seed, a day of weather ([video](docs/design/videos/m10-world.mp4) · [`samples/10-world`](samples/10-world/README.md)) |

The three backend images are the M3 golden image, each labelled with the device that answered
(`m11d5-three-backends-*.manifest`); that comparison is M11.d.5's closing artefact.

---

## What works today

The tier of every capability is recorded in [`status.yaml`](docs/roadmap/status.yaml); Working
means the requirements a real project depends on are satisfied and covered by tests, Complete means
every requirement is. The table groups what is in the tree now.

| Area | What exists | Where |
|---|---|---|
| **Core** | Type system with stable field identity, memory domains, math, jobs, assets and IO, platform abstraction | [`src/core/`](src/core/README.md) · [`core-type-system`](openspec/specs/core-type-system/spec.md) |
| **World model** | Archetype ECS (Complete) under a node façade, prefabs and serialization (Complete) | [`src/ecs/`](src/ecs/README.md) · [`src/scene/`](src/scene/) · [`ecs-core`](openspec/specs/ecs-core/spec.md) |
| **RHI** | Vulkan, Metal and D3D12 backends plus a null device; a render graph that computes every barrier | [`src/backends/`](src/backends/README.md) · [`rhi-and-render-graph`](openspec/specs/rhi-and-render-graph/spec.md) |
| **Shaders** | Slang to SPIR-V, permutations, reflection, a tiered cache, hot reload | [`src/backends/shader/`](src/backends/shader/README.md) · [Slang guide](docs/guides/slang.md) |
| **Frame** | Forward clustered shading, GPU culling and a hierarchical depth pyramid, one temporal framework (jitter, history), a temporal upscaler (Quality, Balanced and Performance modes, dynamic resolution, a seam for vendor upscalers), post stack (exposure, AgX tonemap, bloom, grading), a fog of war stage | [`src/rendering/`](src/rendering/README.md) · [`assembly/`](src/rendering/assembly/README.md) · [`upscale/`](src/rendering/upscale/README.md) · [upscaling guide](docs/guides/temporal-upscaling.md) |
| **Text** | FreeType, HarfBuzz, msdfgen and ICU behind one text server: font fallback, shaping, bidirectional lines, distance-field atlases, font import and cooking | [`src/text/`](src/text/README.md) · [text guide](docs/guides/text.md) |
| **Interface** | CyberUI: an element store, layout and an interface pass for HUDs and the developer console; a widget toolkit with `.cyui` documents, `.cyss` style sheets, hot reload, bindings, animation, keyboard and gamepad focus, opacity groups and an HDR composite | [`src/ui/`](src/ui/README.md) · [UI guide](docs/guides/ui.md) |
| **Materials** | Node graph → IR → compiled program, bindless, block-compressed textures | [`material/`](src/rendering/material/README.md) · [`material-compiler`](openspec/specs/material-compiler/spec.md) |
| **Light** | Shadow maps, contact shadows, ambient occlusion, light probes, baked lightmaps; thousands of lights assigned on the device, a stochastic many-light path, and rect, disc, sphere and tube area lights; real-time GI on the GPU (screen probes and a radiance cache, hardware ray queries or a software distance-field tracer), live in the editor and in a Swift game; ray-tracing infrastructure and a shared denoiser | [`gi/`](src/rendering/gi/README.md) · [`gi_gpu/`](src/rendering/gi_gpu/README.md) · [GI guide](docs/guides/global-illumination.md) · [many lights guide](docs/guides/many-lights.md) |
| **Effects** | Volumetric fog, depth of field, motion blur, decals, selection outlines, GPU particles and trails | [`fog/`](src/rendering/fog/README.md) · [`particles/`](src/rendering/particles/README.md) · [`src/vfx/`](src/vfx/) |
| **Scale** | Virtual geometry clusters, virtual texturing (Complete), virtual shadows, one residency policy (Complete) | [`virtual_geometry/`](src/rendering/virtual_geometry/) · [`virtual-geometry`](openspec/specs/virtual-geometry/spec.md) |
| **Environment** | Physical sky and atmosphere, terrain, water, foliage, weather and wind over one field substrate; procedural generation | [`src/environment/`](src/environment/README.md) · [`src/pcg/`](src/pcg/README.md) |
| **Simulation** | Animation with GPU skinning, drawn in the engine's frame and driven from Swift; deterministic fixed-point math and a fixed-point mover for lockstep units; AI, navigation, physics (Jolt), audio (miniaudio), abilities, cameras, cinematics | [`src/animation/`](src/animation/README.md) · [`src/core/detmath/`](src/core/detmath/README.md) · [`src/movement/`](src/movement/README.md) · [`src/physics/`](src/physics/README.md) |
| **Integrity** | Deterministic simulation, replay and rollback, networking and replication, saves; a deterministic fog of war grid with shared vision and ghosts | [`src/replay/`](src/replay/) · [`src/networking/`](src/networking/README.md) · [`src/save/`](src/save/) · [fog of war guide](docs/guides/fog-of-war.md) |
| **Gameplay** | Swift behaviours and systems over a versioned C ABI, now 1.17 (game services, the interface, animation with blend spaces and foot placement, fixed-point math, GI, fog of war, lights for a moment, the widget toolkit, the first-person camera, effects and decals, combat in the lockstep path, and what a game's menus need), with hot reload; visual scripting compiled by the engine, with a breakpoint debugger and reload during Play; the gameplay framework (Complete) | [`bindings/swift/`](bindings/swift/README.md) · [`src/abi/`](src/abi/README.md) · [visual scripting](docs/guides/visual-scripting.md) |
| **Editor** | Rust application: documents and transactions, live viewport and gizmos, FBX import; material, VFX and gameplay graphs; terrain, physics, navigation, lighting and lightmap, audio and animation tools; an MCP agent interface | [`editor/`](editor/README.md) · [`editor-agent-interface`](openspec/specs/editor-agent-interface/spec.md) |
| **Platforms** | SDL3 desktop, a native Linux backend, headless, iOS, and a stub that shares no desktop assumption | [`platform/`](platform/README.md) |
| **Workflow** | One `justfile`, the same recipes CI runs; diagnostics, traces and crash artefacts (Complete) | [`just/`](just/README.md) · [`diagnostics-profiling-and-crash`](openspec/specs/diagnostics-profiling-and-crash/spec.md) |

What is not there yet is stated too: `build-system-and-platforms`, `thirdparty-dependencies` and
`ml-inference` are at Seed, `xr-support` is deferred to M13, and there is no game — that is M12.

---

## The shape of it

Three languages, three processes, two boundaries — and the same boundary serves game code, the
editor, and a game running on a console.

```mermaid
flowchart TB
    subgraph GAME["Game process"]
        SW["Swift gameplay code"]
        KIT["CyberdyneKit<br/><i>generated overlay</i>"]
        SW --- KIT
    end

    subgraph ED["Editor process (Rust)"]
        UI["Panels · view models · commands"]
        SDK["CyberEditor SDK<br/><i>generated overlay</i>"]
        UI --- SDK
    end

    ABI{{"flat C ABI<br/>versioned · append-only"}}
    BRIDGE{{"live bridge protocol"}}

    subgraph CORE["C++20 core"]
        LAYERS["<b>Scene</b> — node façade, prefabs, serialization<br/><b>ECS</b> — archetypes, queries, scheduler<br/><b>Servers</b> — render, physics, audio, nav, text<br/><b>Backends</b> — Vulkan / Metal / D3D12 · Jolt · platform<br/><b>Core</b> — types, memory, math, jobs, assets"]
    end

    KIT --> ABI
    SDK --> ABI
    SDK --> BRIDGE
    ABI --> CORE
    BRIDGE -.->|"local, remote, or console"| CORE

    classDef boundary fill:#1f2937,stroke:#60a5fa,stroke-width:2px,color:#e5e7eb
    class ABI,BRIDGE boundary
```

The editor is a **client**, not a part of the engine ([`editor-rust-application`](openspec/specs/editor-rust-application/spec.md)).
A runtime crash costs a restart, not a session. And because the runtime is already out of process,
editing on a console is the same code path as editing locally.

### How a frame is built

Nothing walks a scene tree at render time. The GPU scene is the renderer's input, and one arbiter
decides what the frame can afford.

```mermaid
flowchart LR
    subgraph SIM["Simulation"]
        W["ECS world<br/>archetype chunks"]
        ANIM["GPU pose world"]
        VFX["VFX simulation"]
    end

    GS[("GPU scene<br/>instances · materials · transforms")]
    W --> GS
    ANIM --> GS
    VFX --> GS

    subgraph GPU["GPU-driven frame"]
        direction TB
        CULL["Cull + LOD"]
        CLUST["Cluster selection<br/><i>screen-space error</i>"]
        VIS["Visibility buffer"]
        MAT["Material resolve"]
        LIGHT["Lighting + GI"]
        POST["Post + temporal + upscale"]
        CULL --> CLUST --> VIS --> MAT --> LIGHT --> POST
    end

    GS --> CULL

    subgraph PAGES["Paged data"]
        direction TB
        VT["Virtual textures"]
        VSM["Virtual shadows"]
        GEO["Geometry pages"]
    end
    PAGES -.->|"feedback drives residency"| GPU

    ARB{{"Renderer budget arbiter<br/><i>one measurer, many allocations</i>"}}
    ARB -.->|allocations| GPU
    ARB -.->|allocations| PAGES

    POST --> OUT["Frame"]

    classDef arb fill:#3b1f1f,stroke:#f87171,stroke-width:2px,color:#fee2e2
    class ARB arb
```

Every paged system degrades along a defined axis — a coarse geometry root, a resident mip tail, a
stale-but-valid shadow page — so a frame is never missing, only coarser. See
[`rendering-culling-and-lod`](openspec/specs/rendering-culling-and-lod/spec.md),
[`temporal-rendering`](openspec/specs/temporal-rendering/spec.md),
[`denoising`](openspec/specs/denoising/spec.md).

### One command stream

Players, AI, network peers, replays, tests and cinematics all emit the same semantic commands. The
simulation cannot tell them apart — which is why replay, rollback and lockstep are one mechanism
instead of five.

```mermaid
flowchart LR
    P["Player input"] --> CS
    AI["AI agents"] --> CS
    NET["Network peers"] --> CS
    REP["Replay log"] --> CS
    TEST["Automated tests"] --> CS
    SEQ["Cinematic sequences"] --> CS

    CS{{"Gameplay command stream<br/>validated · ordered · logged"}}
    CS --> SIMU["Authoritative simulation"]
    SIMU --> LEDGER[("Side-effect ledger")]
    SIMU --> HASH["Hierarchical state hash"]

    HASH -.->|divergence| DIAG["Narrow to a field on an entity"]
    LEDGER -.->|"replayed once, not twice"| ROLL["Rollback"]

    FIRE["Determinism firewall"] -.-> VFXN["VFX · ML inference<br/><i>presentation only</i>"]
    SIMU --- FIRE

    classDef stream fill:#1f2937,stroke:#60a5fa,stroke-width:2px,color:#e5e7eb
    class CS stream
```

A session **declares** how deterministic it needs to be — `ReplayStable`, `SamePlatform`,
`CrossPlatform`, `Lockstep` — and pays for that and no more; a configuration a subsystem cannot meet
is rejected rather than discovered as a desync months later. See
[`replay-and-rollback`](openspec/specs/replay-and-rollback/spec.md),
[`save-and-persistence`](openspec/specs/save-and-persistence/spec.md).

### Content is a graph of derivations

Explicit inputs, deterministic keys, immutable content-addressed outputs — which is what makes cache
sharing and chunk-level patching possible at all.

```mermaid
flowchart LR
    ASSETS["Source assets<br/>glTF · FBX · textures · audio"] --> IMP["Import"]
    GRAPHS["Authored graphs<br/>material · VFX · AI · PCG"] --> COMP["Compile to IR"]
    IMP --> BG
    COMP --> BG
    BG{{"Build graph<br/>derivation keys"}}
    BG <--> DDC[("Derived data cache<br/>content-addressed")]
    BG --> COOK["Cook<br/>archetype blocks · pages"]
    COOK --> PKG["Package"]
    PKG --> PATCH["Chunk-level patch"]
    BG -.->|"live client"| EDITOR["Editor"]
```

A designer authors hierarchies; the runtime gets flat data. Prefabs, scenes and worlds resolve at
cook time into archetype blocks matching the runtime's chunk layout, so activating a streaming cell
is a bulk copy — and a shipping build carries no prefab link at all. See
[`build-and-packaging`](openspec/specs/build-and-packaging/spec.md),
[`serialization-and-prefabs`](openspec/specs/serialization-and-prefabs/spec.md).

---

## Design decisions worth knowing up front

Each links to the specification that owns it.

- **ECS is the storage; the node tree is the interface.** Component data lives in packed
  per-archetype chunks; a `Node` is a named handle onto an entity and never duplicates data. UI
  elements deliberately live *outside* the ECS, because the right structure per subsystem beats one
  structure everywhere. → [`ecs-core`](openspec/specs/ecs-core/spec.md) · [`ui-system`](openspec/specs/ui-system/spec.md)
- **The scripting boundary is a flat C ABI.** Opaque handles, POD structs, a versioned append-only
  table. Swift and Rust bind through *generated* overlays, so they cannot drift.
  → [`native-abi`](openspec/specs/native-abi/spec.md)
- **Barriers are computed, not written.** The render graph owns synchronisation, transient aliasing
  and pass scheduling; `tools/layercheck/layercheck.py --check barriers` fails when a barrier
  symbol appears outside it. → [`rhi-and-render-graph`](openspec/specs/rhi-and-render-graph/spec.md)
- **Cost is bounded by configuration, not by content.** Rendering, audio and VFX each hold a budget
  with importance tiers, so 8,000 noisy entities and 100 simultaneous explosions cost what you
  configured rather than what the scene happens to contain. → [`vfx-system`](openspec/specs/vfx-system/spec.md)
- **Graphs are compiled, never interpreted.** Materials, VFX, AI, animation, camera rigs, PCG,
  abilities, visual scripts and sequences all lower to shared programs with compact per-entity
  state — no interpreter, no virtual tick per entity. → [`visual-scripting`](openspec/specs/visual-scripting/spec.md)
- **Indirect light is a scheduling problem, not an algorithm.** Screen-space, world-space caches,
  distance-field software tracing and hardware rays, chosen per sample by a computed confidence;
  reflections are the same system with a different ray distribution.
  → [`rendering-global-illumination`](openspec/specs/rendering-global-illumination/spec.md)
- **One component owns the frame's cost.** Several subsystems each measuring GPU time would read one
  shared signal and oscillate together, so exactly one arbiter measures and allocates.
  → [`rendering-architecture`](openspec/specs/rendering-architecture/spec.md)
- **Detail is continuous, and geometry is virtual.** Cost tracks pixels on screen rather than
  triangles in the asset; render geometry is explicitly not collision geometry.
  → [`virtual-geometry`](openspec/specs/virtual-geometry/spec.md)
- **Residency is not activation.** Bytes in memory, entities simulating, textures resident, and how
  much a region is thinking are four independent decisions. → [`residency`](openspec/specs/residency/spec.md)
- **Every edit is a transaction.** Semantic operations addressing objects by stable identity — undo,
  autosave, crash recovery, three-way merge and live editing are one mechanism read five ways.
  → [`editor-documents-and-transactions`](openspec/specs/editor-documents-and-transactions/spec.md)
- **The editor decides what is shown; the renderer decides how it is drawn.** No editor-only shading
  path, so the viewport image is the shipping image, and picking runs engine-side.
  → [`editor-viewport-and-gizmos`](openspec/specs/editor-viewport-and-gizmos/spec.md)
- **Persistent identity does not come from names.** Type and field identifiers are assigned once and
  recorded in a committed manifest with a CI gate, so renaming a field breaks no scene, save,
  animation binding or network schema. → [`core-type-system`](openspec/specs/core-type-system/spec.md)
- **Integrate where it isn't differentiating.** Jolt, miniaudio, Steam Audio, HarfBuzz + ICU +
  FreeType, Slang, Recast, meshoptimizer, xatlas — each behind an engine-owned interface.
  → [`thirdparty-dependencies`](openspec/specs/thirdparty-dependencies/spec.md) · [`THIRD_PARTY.md`](THIRD_PARTY.md)
- **Conventions are stated once, normatively.** Right-handed, Y-up, −Z forward. Reversed-Z with a
  `[0,1]` range. Column-major matrices. Metres, seconds, radians.
  → [`engine-architecture`](openspec/specs/engine-architecture/spec.md)

---

## Gameplay code

Behaviours are the ergonomic path. This one ships in `samples/05b-editor-window/project/game/SpinCube.swift`
and runs when you press Play in the editor:

```swift
import CyberdyneKit
import Foundation

@Behaviour(name: "SpinCube", schema: 1)
final class SpinCube: Behaviour {
    @Export(range: 0...360) var degreesPerSecond: Float = 45

    private var angle: Float = 0
    private var transform = ComponentType.invalid

    override func onCreate() throws {
        guard let world else { throw SpinError.missingWorld }
        transform = world.find(component: "cy::scene::LocalTransform")
        guard transform.isValid else { throw SpinError.missingTransform }
    }

    override func onFixedUpdate(_ delta: Double) throws {
        guard let world else { throw SpinError.missingWorld }
        angle += degreesPerSecond * Float(delta) * .pi / 180
        let half = angle * 0.5
        try world.setFloat(sin(half), entity, transform, field: 2)
        try world.setFloat(cos(half), entity, transform, field: 3)
    }
}
```

Systems are the fast path — same language, same scheduler. The access is the query: the `@System`
macro reads it out of the signature, and a query that reads and writes the same component is a
compile error. The inner loop indexes borrowed chunk arrays with no per-entity ABI call:

```swift
@System(stage: .simulation)
func applyGravity(
    _ query: Query<Write<Velocity>, Read<Mass>, Without<Grounded>>,
    _ chunks: ChunkSource
) {
    chunks.forEachChunk(matching: type(of: query).access) { chunk in
        guard let velocities = chunk.array(Velocity.self),
              let masses = chunk.array(Mass.self) else { return }
        for index in 0..<chunk.count {
            velocities[index].y -= 9.81 * masses[index].value
        }
    }
}
```

The system model is complete and tested, but the ABI does not yet have an entry that hands a module
a chunk; `ChunkSource` is that seam (see the header of
[`Systems.swift`](bindings/swift/Sources/CyberdyneKit/Systems.swift)). Larger examples:
[`samples/04-character`](samples/04-character/) (a third-person character),
[`samples/13-rts-api`](samples/13-rts-api/) (an RTS unit written only in Swift) and
[`samples/15-first-person`](samples/15-first-person/) (a first-person shooter: a character, a weapon
view, hitscan shots, AI targets, and a shot's flash, sparks and marks through ABI 1.15).
→ [`swift-scripting`](openspec/specs/swift-scripting/spec.md) · [`bindings/swift/`](bindings/swift/README.md)

---

## Roadmap

The order is specified, with no dates: a date is an estimate that decays, while *after what* is a
design consequence that does not. The ladder has **22 rungs** — it was split and extended rather
than renumbered, so M5.5, M8.a–c, M11.a–e and M11.d.5 are insertions, and M12 and M13 follow 1.0.
Every rung ends in a runnable artefact whose checks stay in continuous integration afterwards.

```mermaid
flowchart TB
    subgraph F["Foundation"]
        direction LR
        M0["M0 · Ground"] --> M1["M1 · Substrate"] --> M2["M2 · World"]
    end
    subgraph P["First playable"]
        direction LR
        M3["M3 · First light"] --> M4["M4 · Playable"] --> M5["M5 · Authorable"] --> M5B["M5.5 · Operable"]
    end
    subgraph S["Production scale"]
        direction LR
        M6["M6 · Scale"] --> M7["M7 · Fidelity"] --> M8A["M8.a · Authorable"] --> M8B["M8.b · Systems"] --> M8C["M8.c · Spectacle"]
    end
    subgraph SH1["Shipping"]
        direction LR
        M9["M9 · Integrity"] --> M10["M10 · Worlds"] --> M11A["M11.a · Foundations"] --> M11B["M11.b · Authoring"] --> M11C["M11.c · Image"]
    end
    subgraph SH2["Shipping, continued"]
        direction LR
        M11D["M11.d · Desktop"] --> M11D5["M11.d.5 · Backends"] --> M11E["M11.e · Ship<br/><i>1.0</i>"]
    end
    subgraph AF["After the engine"]
        direction LR
        M12["M12 · The Game<br/><i>RTS in Swift</i>"] --> M13["M13 · After 1.0"]
    end
    F --> P --> S --> SH1 --> SH2 --> AF

    classDef closed fill:#14532d,stroke:#4ade80,color:#dcfce7
    classDef active fill:#713f12,stroke:#facc15,stroke-width:2px,color:#fef9c3
    classDef ahead fill:#1f2937,stroke:#6b7280,color:#e5e7eb
    class M0,M1,M2,M3,M4,M5,M5B,M6,M7,M8A,M8B,M8C,M9,M10,M11A,M11B,M11C,M11D,M11D5 closed
    class M11E active
    class M12,M13 ahead
```

Green is closed (19 rungs), amber is current, grey is ahead.

| Era | Rungs | Ends with |
|---|---|---|
| **Foundation** | M0 – M2 | A headless simulation that ticks, hashes, and reproduces its hash exactly |
| **First playable** | M3 – M5.5 | A Swift character controller, edited in an editor window that survives a runtime crash and that an agent can drive |
| **Production scale** | M6 – M8.c | A streamed world rendered at film detail, playable as a vertical slice with particles and a cinematic |
| **Shipping** | M9 – M11.e | Four-player rollback, open worlds, a beauty shot authored in the editor, three backends, every platform — 1.0 |
| **After the engine** | M12 – M13 | An RTS that proves the engine by using it; then Android, `ml-inference` and `xr-support` |

→ [**The roadmap**](docs/ROADMAP.md) · [capability matrix](docs/roadmap/capability-matrix.md) ·
[dependencies](docs/roadmap/dependencies.md) · [risks and deferrals](docs/roadmap/risks.md) ·
[implementing the roadmap](docs/roadmap/implementing.md)

---

## Building

Everything the engine links is fetched at pinned commits ([`deps/manifest.toml`](deps/manifest.toml));
you install the toolchain. On Ubuntu 24.04:

```bash
sudo apt install -y build-essential clang cmake ninja-build git just pkg-config python3
# plus the SDL3 system libraries — see the building guide

just env-doctor      # checks every tool and names the fix for anything missing
just build-engine    # configure and build, dev profile
just test-unit
just run-sample empty
```

Run `just` on its own to list every recipe. The [**building guide**](docs/guides/building.md) has
the full Linux prerequisites, Vulkan, Swift and Rust setup, and:

- **macOS** — the Metal editor: `just build-all`, then `just content-new-project <dir>` and
  `just run-editor-live --project <dir>` ([details](docs/guides/building.md#macos-building-and-running-the-metal-editor))
- **iOS** — `just run-ios-simulator`, `just run-ios-device <udid>` and the RTS capacity scene
  ([details](docs/guides/building.md#ios))
- **CI** — Linux x86_64 and ARM64, macOS ARM64, Windows x86_64 ([details](docs/guides/building.md#ci-targets))

Samples are listed in [`samples/README.md`](samples/README.md); `just capture-beauty-shot` renders
the hero image above and `just run-ship` packages and launches `samples/11-ship`.

---

## Documentation

| | |
|---|---|
| [Animation guide](docs/guides/animation.md) | Skeletons, clips, compiled pose programs, retargeting, GPU skinning and ragdolls |
| [Slang guide](docs/guides/slang.md) | Slang, and how the engine compiles and uses its shaders |
| [Swift guide](docs/guides/swift.md) | Swift gameplay: behaviours, components, the game services of ABI 1.3 onward, the RTS sample, the bindings and hot reload |
| [Global illumination guide](docs/guides/global-illumination.md) | CyberGI: the host system and its device half, the tracing tiers, emissive surfaces, GI volumes, turning it on in the editor and a game, the measurements |
| [Lighting guide](docs/guides/lighting.md) | Authoring static lighting in the editor and baking lightmaps and irradiance volumes |
| [Deterministic math guide](docs/guides/deterministic-math.md) | Fixed-point types, their rules and bounds, the float boundary, lockstep movement, the ABI |
| [Visual scripting guide](docs/guides/visual-scripting.md) | Gameplay graphs: authoring, compiling, running in Play, the debugger and reload while Play runs |
| [Temporal upscaling guide](docs/guides/temporal-upscaling.md) | Rendering below the output resolution, the modes, dynamic resolution, the vendor seam, the measurements |
| [Many lights guide](docs/guides/many-lights.md) | Area lights, device light lists, the stochastic path, lights a game spawns for a moment |
| [First-person weapon view guide](docs/guides/first-person-weapon-view.md) | The arms and weapon through their own lens, drawn into the world's frame; the first-person camera from C++ and Swift |
| [Text guide](docs/guides/text.md) | Fonts, shaping and layout, atlases and distance fields, font import, text in the interface |
| [UI guide](docs/guides/ui.md) | CyberUI: the element store, layout, the interface pass, the developer console, a HUD from game code, the widget toolkit, documents and style sheets, opacity groups and HDR, the toolkit from Swift |
| [RTS map guide](docs/guides/rts-map.md) | The Frontier: its source, the cook, drawing it under the Swift RTS, what holds it |
| [Fog of war guide](docs/guides/fog-of-war.md) | The deterministic grid, the frame stage, the minimap, turning it on in a game |
| [Navigation guide](docs/guides/navigation.md) | Baking and checking a navmesh from the editor or over MCP |
| [Physics guide](docs/guides/physics.md) | Physics and Jolt: components, the bridge, stepping, queries from C++ and Swift, determinism |
| [Building and running](docs/guides/building.md) | Toolchains, Linux, macOS editor, iOS, CI targets |
| [Roadmap](docs/ROADMAP.md) | The milestone ladder, exit criteria and the invariants that cannot wait |
| [Specification index](openspec/specs/README.md) | The 76 capabilities, in reading order |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Profiles, building through CMake, the change workflow |
| [`just/README.md`](just/README.md) | How the recipe files are organised |
| [Beauty shot](docs/design/beauty-shot.md) | What the M11.c image is, what was authored and what the renderer produced |
| [Virtual geometry](docs/design/virtual-geometry.md) | The cluster frame, the density, and what the images do not show |
| [Editor visual language](docs/design/editor-visual-language.md) | How the editor looks and what its colours mean |
| [`src/README.md`](src/README.md) · [`editor/README.md`](editor/README.md) · [`platform/README.md`](platform/README.md) | The engine, editor and platform trees; every module has its own README |

## Repository layout

```
src/              Engine: core/ ecs/ scene/ servers/ backends/ rendering/ abi/ and the simulation modules — strictly layered
platform/         desktop-sdl3/, linux-native/, headless/, ios/, stub/, host/. The only place SDL may be named.
bindings/swift/   CyberdyneKit, the generated overlay and macros for Swift gameplay
editor/           The Rust editor (Cargo workspace under crates/)
modules/          Optional functionality, discovered by manifest
samples/          Runnable artefacts, one per milestone; each stays green forever after
content/          Authored content for the samples (content/beauty/)
tests/            unit/ integration/ smoke/ render/ determinism/ editor/ acceptance/
benchmarks/       Throughput and latency, with regression thresholds
cmake/            Build modules; module.cmake carries the layering rule
deps/             manifest.toml — every dependency, pinned to a commit
tools/            layercheck, roadmap, deps, cook, import, material, quality and more
just/             One file per recipe category, imported by the root justfile
docs/             ROADMAP.md, roadmap/ (matrix, status record), design/ (write-ups, images), guides/
openspec/         specs/ (76 capabilities, the contract), changes/ (in-flight proposals), config.yaml
```

## Working on this

Specifications are the source of truth and precede implementation. Changes flow through
[OpenSpec](https://openspec.dev):

```bash
npm install -g @fission-ai/openspec@latest

openspec list --specs                  # what is specified
openspec show engine-architecture      # read one
openspec validate --specs --strict     # check them all
```

To propose a change, use `/opsx:propose` in an agent session, or scaffold with
`openspec new change <name>`, then implement against the generated tasks and archive when done.
See [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Licence

MIT — see [LICENSE](LICENSE). Chosen to match the permissive licensing of the libraries the engine
integrates and to place no obligations on games built with it.
