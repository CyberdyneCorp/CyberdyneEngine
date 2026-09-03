# CyberdyneEngine

An open-source game engine: **C++20** core, **Swift** for gameplay scripting.

Inspired by Godot's server architecture and scene ergonomics, Unity's component composition and
prefab workflow, and Unreal's render graph and tooling ambition — but not a port of any of them.

> **Status: specification stage.** There is no code yet. The specifications in
> [`openspec/specs/`](openspec/specs/) define what is being built and why, and are the contract
> the implementation must satisfy. Start with
> [the specification index](openspec/specs/README.md).

## What it is

| | |
|---|---|
| **Core language** | C++20, no exceptions, no RTTI |
| **Scripting** | Swift, via a generated overlay over a stable C ABI |
| **World model** | Archetype ECS core with a scene-graph node façade |
| **Gameplay** | CyberGameplay: sessions, rules and teams as data; one command stream for players, AI, network and replay |
| **Input & camera** | CyberInput: users, contexts, triggers. CyberCamera: compiled rigs producing views, listeners and streaming sources |
| **World** | CyberWorld: partitioned streaming cells, layers, persistence overlay, authored from prefabs and scenes |
| **Environment** | CyberTerrain, CyberFoliage and CyberWater over a shared sparse field substrate |
| **Renderer** | Explicit RHI + automatic render graph — Vulkan first, native Metal second, D3D12 later |
| **Shaders** | Slang → SPIR-V (→ MSL) |
| **VFX** | Engine-owned, GPU-first, compiled effect graphs |
| **UI** | CyberUI: retained tree, declarative Swift/C++ authoring, CSS-like styling, GPU-driven |
| **AI** | CyberAI: ECS agents, one compiled graph for states/BT/utility/GOAP, deterministic |
| **Geometry** | CyberGeometry: virtualised clusters, GPU-driven streaming and culling, visibility buffer |
| **Textures & shadows** | CyberTexture and CyberShadow: paged virtual textures and receiver-driven virtual shadows |
| **Materials** | CyberMaterial: graph → IR → closures → compiled program, bindless, GPU material table |
| **Animation** | CyberAnimation: skeleton/rig split, compiled programs, GPU pose world, motion matching |
| **Illumination** | CyberGI: hybrid tracing, surface and radiance caches, shared with reflections |
| **Networking** | CyberNet: ECS component replication, three network modes, priority-scheduled interest |
| **Physics** | Jolt, behind an engine-owned interface |
| **Audio** | Engine-owned AudioServer over miniaudio; Steam Audio for spatial acoustics |
| **Simulation integrity** | Determinism profiles, one command log for replay and rollback, saves as world deltas |
| **Tooling** | Transaction-based editor, live editing to local and remote runtimes, graph-driven builds |
| **Licence** | MIT |

## The shape of it

```
                      Swift game code
                            │
                   CyberdyneKit (generated overlay)
                            │
                    flat C ABI  (versioned, append-only)
                            │
  ┌─────────────────────────┴─────────────────────────┐
  │                    C++20 core                     │
  │                                                   │
  │   Editor                                          │
  │   Scene   ── Node façade, prefabs, serialization   │
  │   ECS     ── archetypes, queries, scheduler       │
  │   Servers ── render, physics, audio, nav, text    │
  │   Backends── Vulkan/Metal · Jolt · platform       │
  │   Core    ── types, memory, math, jobs, assets    │
  └───────────────────────────────────────────────────┘
```

## Design decisions worth knowing up front

**Input becomes intent, and the camera produces everything downstream.** Devices feed a layered
context stack that yields semantic actions, so gameplay never sees a key press, rebinding cannot
change behaviour and accessibility transformations cannot be bypassed. Cameras are composable rigs
compiled to programs — never a base class, never written directly by gameplay — and one evaluated
camera yields the render view, the audio listener anchor and the streaming source with predicted
motion, so the three cannot disagree about where the player is.

**Gameplay structure is composition, not inheritance.** Sessions, rules, participants, teams,
ownership and control are ECS data and scoped services — no actor base class, no per-entity virtual
tick, no one-controller-one-pawn. Players, AI, network peers, replays and automated tests all emit
the same semantic commands, so the simulation cannot tell them apart, and control bindings are
many-to-many: one player commands an army, two players share a tank. Behaviours you attach like
scripts compile into generated systems where they can, and the build tells you when they cannot.

**ECS is the storage; the node tree is the interface.** Component data lives in packed
per-archetype chunks so systems iterate contiguously. A `Node` is a named handle onto an entity —
it never duplicates data. Designers and Swift developers get the object-oriented tree; the runtime
gets the packed arrays. The coherence invariants that keep those two honest are specified, not
assumed.

**The scripting boundary is a flat C ABI.** C++ has no stable ABI across compilers or standard
library versions, so the boundary is C: opaque handles, POD structs, a versioned append-only
function table. Swift binds through a *generated* overlay, so the two can never drift. This is
what makes hot reload, stable game modules, and future language bindings tractable.

**Barriers are computed, not written.** The render graph owns synchronisation, transient memory
aliasing, and pass scheduling. No renderer code writes a barrier or a layout transition.

**Parallelism is safe by construction.** Systems declare `Read` / `Write` / `Exclude` access;
the scheduler derives the dependency graph and runs non-conflicting systems concurrently.
Undeclared access is an assertion, not a race waiting to be found.

**Integrate where it isn't differentiating.** Jolt for physics, miniaudio and Steam Audio for
audio, HarfBuzz + ICU + FreeType for text, Slang for shaders, Recast for navmeshes, meshoptimizer
and xatlas for mesh processing. Each sits behind an engine-owned interface and must stay
replaceable. We build the ECS, renderer, scene model, asset pipeline, UI, animation, networking,
audio graph and policy, and the editor — the parts where engine-level decisions compound.

**Determinism is a contract, decided per subsystem.** AI is deterministic — it drives gameplay
that must survive network reconciliation and replay — so its scheduling derives from simulation
state, never from measured frame time, and the budget controller offers a deterministic mode and an
adaptive one that is honest about forfeiting lockstep. VFX and ML inference are non-deterministic
and firewalled from authoritative state. Each boundary is written down, because the failure mode
otherwise is an unexplained desync months later.

**ECS is not the answer to everything.** Component storage is right for gameplay entities and
wrong for UI: a strategy game with 5,000 units has 20,000-plus labels, icons and containers, whose
workload is a tree walk, not an archetype scan, and which need element-granular invalidation. So
UI elements live in their own data-oriented storage and the gameplay world stays gameplay-sized.
Using the right structure per subsystem beats one structure everywhere.

**Cost is bounded by configuration, not by content.** The renderer has culling and LOD budgets;
audio has importance tiers with per-tier source budgets; VFX has importance classes driven by a
frame-time budget controller. 8,000 noisy entities and 100 simultaneous explosions cost what you
configured rather than what the scene happens to contain. Deciding how much simulation each thing
deserves is the performance lever that actually matters at scale.

**Reproducibility is a chosen profile, and divergence is findable.** A session declares how
deterministic it needs to be — replay-stable, same-platform, cross-platform, lockstep — and pays for
that and no more, with the configuration rejected if a subsystem cannot meet it. One command log
serves replay, rollback and lockstep; what genuinely cannot be reproduced is recorded rather than
re-run. And when two runs disagree, hierarchical hashing narrows it to a field on an entity while a
chaos scheduler surfaces the ordering dependency that caused it.

**Every edit is a transaction; every build is a graph.** Editor mutations are semantic operations
addressing objects by stable identity, which makes undo, autosave, crash recovery, three-way merge
and live editing one mechanism rather than five. Builds are graphs of derivations with explicit
inputs, deterministic keys and immutable content-addressed outputs — so an incremental build is
dependency-driven rather than timestamp-driven, and a one-texture change ships as a patch of the
pages that changed.

**Designers author hierarchies; the runtime gets flat data.** Prefabs, scenes and worlds are
authoring assets with nesting, variants and overrides — all resolved at cook time into archetype
blocks that match the runtime's chunk layout, so activating a streaming cell is a bulk copy rather
than a hundred thousand object constructions, and a shipping build carries no prefab link at all.
A prefab can expose a deliberate parameter surface, so its internals stay refactorable instead of
becoming part of its contract with every instance.

**The environment shares one substrate.** Biome, moisture, wind, flow, wetness and burn state live
in sparse world-scale fields that terrain, vegetation, water, VFX, audio and AI all sample by
position — so a river writes wetness that a terrain material reads and foliage placement avoids,
without terrain and water knowing anything about each other. Terrain is a geometry source feeding
the same virtual-geometry path as everything else rather than a renderer of its own; a million
trees are GPU instances that get promoted to entities only when gameplay touches them; and a water
body declares which wave bands physics and rendering must both obey.

**Residency is not activation.** A region's bytes being in memory, its entities participating in
simulation, its textures being resident, and how much it is thinking are four independent
decisions. Collapsing them is what makes crossing a boundary mean *load everything now*; kept
apart, approach becomes a gradient where every step is cheap.

**Indirect light is a scheduling problem, not an algorithm.** Rather than one GI technique, the
engine combines screen-space tracing, world-space radiance and surface caches, software tracing
against a distance field, and hardware rays — picking per sample the cheapest source that can give
a trustworthy answer, where trust is a confidence value the system computes. A traced hit reads
cached radiance instead of re-evaluating a material graph, which is what makes the whole thing
affordable and which produces multi-bounce lighting as a side effect. Reflections are the same
system with a different ray distribution, not a second one.

**One component owns the frame's cost.** A renderer measures GPU time and lowers quality to fit a
budget — but if several subsystems each do that independently, they read one shared signal, correct
for costs they did not cause, and oscillate together. So exactly one arbiter measures the frame and
hands out allocations; the VFX, geometry, and resolution controllers hold their own allocation with
their own levers and report cost back. Capture and cinematics pin all of them at once, because
half-pinned is not a state that should exist.

**A material is compiled, and its cost is visible.** Authoring is a graph or a text definition;
both lower to one typed intermediate representation that is optimised, cost-analysed, and then
generated as shader source. Surfaces are composed from BSDF closures rather than picked from a
menu of models — but a closure set that matches a known model compiles to that model's path and
costs exactly what it costs, so layering is there when needed and free when not. The editor shows
every stage from graph to binary, with cost attributed back to the nodes that caused it.

**World scale does not determine memory scale.** Geometry, textures and shadows are all paged
against a shared residency policy: a project may hold terabytes of source content while a frame
holds only what the platform's budget permits. Shadow pages exist because a *visible pixel* needs
them rather than because a caster exists, and persist across frames until something actually
invalidates them. Every one of these systems degrades along a defined axis — a coarse geometry root,
a resident mip tail, a stale-but-valid shadow page — so a frame is never missing, only coarser.

**Detail is continuous, and geometry is virtual.** A mesh is a hierarchy of small triangle
clusters, and the GPU picks which clusters to draw each frame against a screen-space error target —
so cost tracks the pixels on screen rather than the triangles in the asset, and artists stop
authoring LOD chains. Clusters are simplified in groups so shared boundaries stay watertight;
streaming works in pages with an always-resident root, so an object is never absent, only coarse.
Render geometry is explicitly not collision geometry: physics, navigation and ray tracing get their
own representations from the same source.

**Persistent identity does not come from names.** Types and fields carry identifiers assigned once
and recorded in a committed manifest, so renaming a field or moving a class into a namespace leaves
every scene, prefab override, save file, animation binding and network schema still resolving —
and a CI gate fails the build if an identifier ever changes by accident. Serialization is two
deliberate modes rather than one compromise: tagged and migratable for authoring and saves, packed
and untagged for runtime data that loads by bulk copy.

**Nothing blocks a worker, and nothing allocates without a budget.** Asynchronous work is
coroutines whose continuations resume as tasks, so a file read or a GPU fence suspends rather than
occupying a core; every task carries its scratch allocator and cancellation token. Memory is
apportioned by a budget tree with a pressure level that tells every cache to trim at once — the
counterpart of the renderer's GPU-time arbiter, because an over-budget frame is a stutter and an
over-budget heap is a crash.

**Conventions are stated once, normatively.** Right-handed, Y-up, −Z forward. Reversed-Z with a
`[0,1]` depth range. Column-major matrices. Metres, seconds, radians. A silent mismatch here
corrupts everything downstream, so it is written down rather than discovered.

## Gameplay code, roughly

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

…and for bulk work, the same language drops to data-oriented systems:

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

Behaviours are the ergonomic path. Systems are the fast path. Both are scheduled by the same
scheduler, and a project can use either or both.

## Repository layout

```
openspec/
  specs/          Target specifications — 62 capabilities. Start here.
  changes/        In-flight proposals (propose → apply → validate → archive)
  config.yaml     Project context and locked architectural decisions
```

Source directories (`src/`, `bindings/`, `editor/`, `tools/`) will appear as the specifications are
implemented.

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
The reasoning behind a decision stays attached to it.

## Licence

MIT — see [LICENSE](LICENSE). Chosen to match the permissive licensing of the libraries the engine
integrates and to place no obligations on games built with it.
