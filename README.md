# CyberdyneEngine

An open-source game engine. **C++20** core, **Swift** for gameplay, **Rust** for the editor.

Inspired by Godot's server architecture and scene ergonomics, Unity's component composition and
prefab workflow, and Unreal's render graph and tooling ambition — but not a port of any of them.

> **Status: [M0 — Ground](docs/ROADMAP.md) has landed. M1 — Substrate is next.**
> The build configures and runs on Linux: `just build-all`, `just test-all`, and a sample that opens
> a window and writes a trace. Windows and macOS are authored and wait on their first CI run.
> [`openspec/specs/`](openspec/specs/) holds **75 capabilities · 1,192 requirements · 2,624 scenarios**
> that define what is being built and why, and are the contract the implementation must satisfy.
> Start at [the specification index](openspec/specs/README.md), then
> [the roadmap](docs/ROADMAP.md) for the order they are built in, and
> [Building on Linux](#building-on-linux) to compile it.

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
        LAYERS["<b>Scene</b> — node façade, prefabs, serialization<br/><b>ECS</b> — archetypes, queries, scheduler<br/><b>Servers</b> — render, physics, audio, nav, text<br/><b>Backends</b> — Vulkan / Metal · Jolt · platform<br/><b>Core</b> — types, memory, math, jobs, assets"]
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

---

## What it is

| | | |
|---|---|---|
| **Core** | C++20, no exceptions, no RTTI; archetype ECS with a node façade | [`ecs-core`](openspec/specs/ecs-core/spec.md) · [`scene-graph-and-nodes`](openspec/specs/scene-graph-and-nodes/spec.md) |
| **Scripting** | Swift over a generated overlay on a stable C ABI | [`swift-scripting`](openspec/specs/swift-scripting/spec.md) · [`native-abi`](openspec/specs/native-abi/spec.md) |
| **Editor** | Separate Rust application; MVVM, commands, hosted runtime | [`editor-rust-application`](openspec/specs/editor-rust-application/spec.md) · [`editor-ui-ux`](openspec/specs/editor-ui-ux/spec.md) |
| **Renderer** | Explicit RHI + automatic render graph — Vulkan first, Metal second | [`rhi-and-render-graph`](openspec/specs/rhi-and-render-graph/spec.md) · [`rendering-architecture`](openspec/specs/rendering-architecture/spec.md) |
| **Geometry** | CyberGeometry: virtualised clusters, GPU culling, visibility buffer | [`virtual-geometry`](openspec/specs/virtual-geometry/spec.md) |
| **Materials** | CyberMaterial: graph → IR → closures → compiled program, bindless | [`material-compiler`](openspec/specs/material-compiler/spec.md) |
| **Illumination** | CyberGI: hybrid tracing, surface and radiance caches, shared with reflections | [`rendering-global-illumination`](openspec/specs/rendering-global-illumination/spec.md) |
| **Paging** | Virtual textures and receiver-driven virtual shadows over one residency policy | [`virtual-texturing`](openspec/specs/virtual-texturing/spec.md) · [`virtual-shadows`](openspec/specs/virtual-shadows/spec.md) · [`residency`](openspec/specs/residency/spec.md) |
| **World** | CyberWorld: partitioned cells, layers, persistence overlay | [`world-partition-and-streaming`](openspec/specs/world-partition-and-streaming/spec.md) |
| **Environment** | Terrain, foliage, water, weather and sky over one sparse field substrate | [`environment-fields`](openspec/specs/environment-fields/spec.md) · [`terrain`](openspec/specs/terrain/spec.md) · [`water`](openspec/specs/water/spec.md) |
| **Procedural** | CyberPCG: compiled graphs, region-incremental, stable generated identity | [`procedural-content-generation`](openspec/specs/procedural-content-generation/spec.md) |
| **Gameplay** | Sessions, rules and teams as data; one command stream for every producer | [`gameplay-framework`](openspec/specs/gameplay-framework/spec.md) |
| **Simulation** | Animation, AI, physics and audio — compiled programs; Jolt and miniaudio behind engine-owned interfaces | [`animation-and-skinning`](openspec/specs/animation-and-skinning/spec.md) · [`ai-system`](openspec/specs/ai-system/spec.md) · [`physics`](openspec/specs/physics/spec.md) |
| **Networking** | CyberNet: component replication, priority-scheduled interest, rollback | [`networking-and-replication`](openspec/specs/networking-and-replication/spec.md) |
| **Integrity** | Determinism profiles, one command log for replay, rollback and cinematics | [`simulation-and-determinism`](openspec/specs/simulation-and-determinism/spec.md) · [`sequencing-and-cinematics`](openspec/specs/sequencing-and-cinematics/spec.md) |
| **Diagnostics** | One trace, rolling capture, crash artefacts that reproduce | [`diagnostics-profiling-and-crash`](openspec/specs/diagnostics-profiling-and-crash/spec.md) |
| **Workflow** | One `justfile` — the same recipes CI runs | [`developer-workflow-and-just`](openspec/specs/developer-workflow-and-just/spec.md) |
| **Licence** | MIT | [LICENSE](LICENSE) |

---

## How a frame is built

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
stale-but-valid shadow page — so a frame is never missing, only coarser.
See [`rendering-culling-and-lod`](openspec/specs/rendering-culling-and-lod/spec.md),
[`temporal-rendering`](openspec/specs/temporal-rendering/spec.md),
[`denoising`](openspec/specs/denoising/spec.md).

---

## One command stream

Players, AI, network peers, replays, tests and cinematics all emit the same semantic commands. The
simulation cannot tell them apart — which is precisely why replay, rollback and lockstep are one
mechanism instead of five.

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
is rejected rather than discovered as a desync months later.
See [`replay-and-rollback`](openspec/specs/replay-and-rollback/spec.md),
[`save-and-persistence`](openspec/specs/save-and-persistence/spec.md).

---

## Content is a graph of derivations

Not a script, not timestamps. Explicit inputs, deterministic keys, immutable content-addressed
outputs — which is what makes cache sharing and chunk-level patching possible at all.

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
is a bulk copy — and a shipping build carries no prefab link at all.
See [`build-and-packaging`](openspec/specs/build-and-packaging/spec.md),
[`serialization-and-prefabs`](openspec/specs/serialization-and-prefabs/spec.md).

---

## Design decisions worth knowing up front

Each links to the specification that owns it. The reasoning stays attached to the decision.

- **ECS is the storage; the node tree is the interface.** Component data lives in packed
  per-archetype chunks. A `Node` is a named handle onto an entity — it never duplicates data.
  Designers get the tree, the runtime gets the arrays, and the coherence invariants are specified
  rather than assumed — but UI elements deliberately live *outside* the ECS, because the right
  structure per subsystem beats one structure everywhere.
  → [`ecs-core`](openspec/specs/ecs-core/spec.md) · [`ui-system`](openspec/specs/ui-system/spec.md)
- **The scripting boundary is a flat C ABI.** Opaque handles, POD structs, a versioned append-only
  table. Swift and Rust bind through *generated* overlays, so they cannot drift.
  → [`native-abi`](openspec/specs/native-abi/spec.md)
- **Barriers are computed, not written.** The render graph owns synchronisation, transient aliasing
  and pass scheduling. No renderer code writes a barrier.
  → [`rhi-and-render-graph`](openspec/specs/rhi-and-render-graph/spec.md)
- **Cost is bounded by configuration, not by content.** Rendering, audio and VFX each hold a budget
  with importance tiers, so 8,000 noisy entities and 100 simultaneous explosions cost what you
  configured rather than what the scene happens to contain. Deciding *how much simulation each thing
  deserves* is the lever that actually matters at scale. → [`vfx-system`](openspec/specs/vfx-system/spec.md)
- **Graphs are compiled, never interpreted.** Materials, VFX, AI, animation, camera rigs, PCG,
  abilities, visual scripts and sequences all lower to shared programs with compact per-entity
  state. No object, no interpreter, no virtual tick per entity — the whole reason to build an ECS.
  → [`visual-scripting`](openspec/specs/visual-scripting/spec.md)
- **Indirect light is a scheduling problem, not an algorithm.** Screen-space, world-space caches,
  distance-field software tracing and hardware rays, chosen per sample by a computed confidence
  value. Reflections are the same system with a different ray distribution.
  → [`rendering-global-illumination`](openspec/specs/rendering-global-illumination/spec.md)
- **One component owns the frame's cost.** Several subsystems each measuring GPU time would read one
  shared signal, correct for costs they did not cause, and oscillate together. So exactly one
  arbiter measures and allocates; the rest hold allocations and report cost back.
  → [`rendering-architecture`](openspec/specs/rendering-architecture/spec.md)
- **Detail is continuous, and geometry is virtual.** Cost tracks pixels on screen rather than
  triangles in the asset, and artists stop authoring LOD chains. Render geometry is explicitly not
  collision geometry. → [`virtual-geometry`](openspec/specs/virtual-geometry/spec.md)
- **Residency is not activation.** Bytes in memory, entities simulating, textures resident, and how
  much a region is thinking are four independent decisions. Collapsing them is what makes crossing a
  boundary mean *load everything now*. → [`residency`](openspec/specs/residency/spec.md)
- **Every edit is a transaction.** Semantic operations addressing objects by stable identity — which
  makes undo, autosave, crash recovery, three-way merge and live editing one mechanism read five
  ways. → [`editor-documents-and-transactions`](openspec/specs/editor-documents-and-transactions/spec.md)
- **The editor decides what should be shown; the renderer decides how it is drawn.** No second
  renderer, no editor-only shading path, so the viewport image is the shipping image — and picking
  runs engine-side, so what is picked is what was actually rendered.
  → [`editor-viewport-and-gizmos`](openspec/specs/editor-viewport-and-gizmos/spec.md)
- **Persistent identity does not come from names.** Type and field identifiers are assigned once and
  recorded in a committed manifest with a CI gate, so renaming a field leaves every scene, override,
  save, animation binding and network schema resolving.
  → [`core-type-system`](openspec/specs/core-type-system/spec.md)
- **Integrate where it isn't differentiating.** Jolt, miniaudio, Steam Audio, HarfBuzz + ICU +
  FreeType, Slang, Recast, meshoptimizer, xatlas — each behind an engine-owned interface, each
  replaceable. → [`thirdparty-dependencies`](openspec/specs/thirdparty-dependencies/spec.md)
- **Conventions are stated once, normatively.** Right-handed, Y-up, −Z forward. Reversed-Z with a
  `[0,1]` range. Column-major matrices. Metres, seconds, radians.
  → [`engine-architecture`](openspec/specs/engine-architecture/spec.md)

---

## Gameplay code, roughly

Behaviours are the ergonomic path:

```swift
@Behaviour
final class PlayerController: Behaviour {
    @Export var speed: Float = 6.0
    @Export(range: 0...20) var jumpVelocity: Float = 8.0

    private var velocity = Vec3.zero

    override func onFixedUpdate(_ dt: Double) {
        let move = Input.vector("move")
        velocity.x = move.x * speed
        velocity.z = move.y * speed
        if Input.justPressed("jump"), characterController.isGrounded {
            velocity.y = jumpVelocity
        }
        velocity.y += Physics.gravity * Float(dt)
        characterController.move(velocity * Float(dt))
    }
}
```

Systems are the fast path — same language, same scheduler:

```swift
@System(stage: .simulation)
func applyGravity(
    _ query: Query<Write<Velocity>, Read<Mass>, Without<Grounded>>,
    time: Res<Time>
) {
    for (velocity, _) in query {
        velocity.linear.y -= 9.81 * Float(time.fixedDelta)
    }
}
```

A project can use either or both. Behaviours that can be batched compile into generated systems, and
the build tells you when they cannot. → [`swift-scripting`](openspec/specs/swift-scripting/spec.md)

---

## Roadmap

There is no code yet, and the interesting question is not *what* to build — that is written down —
but *in what order*. Some of the commitments in these specifications are properties of every line of
code written after them: computed barriers, stable field identity, one command stream into the
simulation, transactions as the only write path. Established at the right moment they cost almost
nothing; established late, everything downstream has to be revisited.

So the order is specified too, in twelve milestones with no dates — because a date is an estimate
that decays, while *after what* is a design consequence that does not.

| Era | | Ends with |
|---|---|---|
| **Foundation** | M0 Ground · M1 Substrate · M2 World | A headless simulation that ticks, hashes, and reproduces its hash exactly |
| **First playable** | M3 First light · M4 Playable · M5 Authorable | A character controller written in Swift, edited in an editor that survives a runtime crash |
| **Production scale** | M6 Scale · M7 Fidelity · M8 Game systems | A streamed multi-kilometre world at film detail, playable as a real game |
| **Shipping** | M9 Integrity · M10 Worlds · M11 Reach | Four-player rollback, open worlds, every platform — 1.0 |

Every milestone ends in a runnable artefact committed to the repository, and once its checks are
green they stay in continuous integration — so the M4 character controller still runs at M11.

→ [**The roadmap**](docs/ROADMAP.md) · [capability matrix](docs/roadmap/capability-matrix.md) ·
[dependencies](docs/roadmap/dependencies.md) · [risks and deferrals](docs/roadmap/risks.md)

---

## Repository layout

```
src/              Engine. core/ ecs/ servers/ backends/ scene/ runtime/ abi/ — strictly layered
platform/         Platform layer: desktop-sdl3/, headless/. The only place SDL may be named.
modules/          Optional functionality, discovered by manifest
samples/          Runnable artefacts. One per milestone; each stays green forever after.
tests/            unit/ integration/ smoke/ — and render/ determinism/ awaiting M3 and M9
benchmarks/       Throughput and latency, with regression thresholds
cmake/            Build modules. module.cmake carries the layering rule.
deps/             manifest.toml — every dependency, pinned to a commit
tools/            layercheck, roadmap, deps, trace inspection
just/             One file per recipe category, imported by the root justfile
docs/
  ROADMAP.md      Milestone ladder, exit criteria, and the invariants that cannot wait
  roadmap/        Capability matrix, dependency graphs, risk register, status record
  design/         The editor's visual language, with reference imagery
openspec/
  specs/          Target specifications — 75 capabilities. Start here.
  changes/        In-flight proposals (propose → apply → validate → archive)
  config.yaml     Project context and locked architectural decisions
justfile          One entry point for every developer task — run `just` to list them
```

`bindings/` and `editor/` are placeholders until M4 and M5. Directories appear as the milestones
that specify them land, not before.

## Building on Linux

> **The build lands with [M0](docs/ROADMAP.md).** Until that milestone closes, `openspec/` is the
> whole repository and there is nothing to compile — skip to [Working on this](#working-on-this).
> The prerequisites below are what M0 needs, and they are worth installing before it does.

Reference platform: **Ubuntu 24.04 LTS "noble"** and derivatives (Linux Mint 22.x). Other
distributions work; only the package names differ.

### Prerequisites

Everything the engine *links* — SDL3, doctest, Tracy, zstd, BLAKE3, and later Jolt, Slang and the
rest — is fetched and built from source by CMake at pinned commits recorded in `deps/manifest.toml`.
You do not install those. What you install is the toolchain, plus the system libraries SDL3 itself
links against.

```bash
# Toolchain
sudo apt install -y build-essential clang cmake ninja-build git just pkg-config python3

# System libraries SDL3 builds against (windowing, input, audio, IME)
sudo apt install -y libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
                    libxfixes-dev libxss-dev libxkbcommon-dev \
                    libwayland-dev wayland-protocols libdecor-0-dev \
                    libudev-dev libasound2-dev libpulse-dev libibus-1.0-dev
```

`libudev-dev` is not optional in practice — it is what gives SDL3 gamepad hot-plug on Linux, which
[`core-platform-abstraction`](openspec/specs/core-platform-abstraction/spec.md) requires.

| | Minimum | Why |
|---|---|---|
| CMake | **3.28** | Required by [`build-system-and-platforms`](openspec/specs/build-system-and-platforms/spec.md); presets and target-level layering |
| Ninja | 1.11 | The default generator |
| Clang | 18 | Or GCC 13. The engine is C++20 with `-fno-exceptions -fno-rtti` |
| just | 1.14 | Needs `import`; noble ships 1.21 |
| Python | 3.10 | Code generation and the layering, roadmap and dependency tools |

### First build

```bash
git clone <this repository> && cd CyberdyneEngine

just env-doctor      # checks every tool above and names the fix for anything missing
just build-engine    # configure and build, dev profile
just test-unit       # under a minute
just run-sample empty
```

Run `just` on its own to list every recipe with a description — that listing is the workflow's
documentation, and a task you are expected to perform always has a recipe. Four profiles —
`debug`, `dev`, `profile`, `release` — mean the same thing across every toolchain
([`developer-workflow-and-just`](openspec/specs/developer-workflow-and-just/spec.md)).

### Additional prerequisites, by milestone

Each becomes a check in `just env-doctor` when its milestone arrives. Nothing below is needed
before then.

**M3 · First light** — Vulkan. You also need a working driver and ICD; Mesa or the proprietary
NVIDIA/AMD drivers provide one, and `/usr/share/vulkan/icd.d/` is where to check.

```bash
sudo apt install -y libvulkan-dev vulkan-tools vulkan-validationlayers spirv-tools glslang-tools
vulkaninfo --summary        # must list a device, with its apiVersion
vkcube                      # a spinning cube confirms the swapchain path works
```

There is no `vulkan-validationlayers-dev` on noble — that package name is from 22.04 and was
dropped. Noble's `vulkan-tools` is from SDK 1.3.275 while a current driver will report Vulkan 1.4;
that mismatch is fine, and the LunarG SDK is only worth adding if newer validation coverage turns
out to be needed.

**M4 · Playable** — the Swift toolchain, via swift.org's own installer. Two things about this
install are not obvious and both have already cost time; they are written out below rather than
left to be rediscovered.

```bash
sudo apt install -y binutils gnupg2 libc6-dev libcurl4-openssl-dev libedit2 libgcc-13-dev \
                    libpython3-dev libsqlite3-0 libstdc++-13-dev libxml2-dev libz3-dev \
                    tzdata unzip zlib1g-dev

curl -O https://download.swift.org/swiftly/linux/swiftly-x86_64.tar.gz
tar zxf swiftly-x86_64.tar.gz && ./swiftly init
. ~/.local/share/swiftly/env.sh
swiftly install --use latest --platform ubuntu24.04
```

**On a Ubuntu derivative, name the platform.** `/etc/os-release` reports `ID=linuxmint` rather than
`ubuntu`, so swiftly stops and asks you to pick a platform from a menu. Passing
`--platform ubuntu24.04` answers it up front — which matters because a script or a CI job cannot
answer a prompt. The Ubuntu 24.04 toolchain is the correct choice on Mint 22.x; `UBUNTU_CODENAME`
in `/etc/os-release` is what confirms the base.

**Then put the environment line in `~/.bashrc`, not just `~/.profile`.** `swiftly init` writes it
to `~/.profile`, which only *login* shells read. A new terminal window is an interactive
*non-login* shell that reads `~/.bashrc` and never touches `~/.profile` — so `swift` appears to
vanish the moment you open a second terminal, and build tooling that spawns its own shells does not
see it either:

```bash
printf '\n# Added by swiftly\n. "$HOME/.local/share/swiftly/env.sh"\n' >> ~/.bashrc
```

Verify from a **new** terminal, which is the case that actually fails:

```bash
swift --version                                  # expect 6.x, x86_64-unknown-linux-gnu
echo 'print("ok")' > /tmp/t.swift && swift /tmp/t.swift
```

Any script that must not depend on shell configuration should source the environment explicitly
instead: `bash -lc '. ~/.local/share/swiftly/env.sh; swiftc …'`.

**M5 · Authorable** — Rust for the editor, via [rustup](https://rustup.rs). The engine and the
editor are separate builds; `just` drives both.

### Other platforms

Windows and macOS are supported targets from M0 and are built in continuous integration on every
change. Only Linux is documented here because it is the platform the engine is being developed on;
the CI workflow files are the authoritative recipe for the other two.

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

## Licence

MIT — see [LICENSE](LICENSE). Chosen to match the permissive licensing of the libraries the engine
integrates and to place no obligations on games built with it.
