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
| **Renderer** | Explicit RHI + automatic render graph — Vulkan first, native Metal second, D3D12 later |
| **Shaders** | Slang → SPIR-V (→ MSL) |
| **VFX** | Engine-owned, GPU-first, compiled effect graphs |
| **UI** | CyberUI: retained tree, declarative Swift/C++ authoring, CSS-like styling, GPU-driven |
| **Physics** | Jolt, behind an engine-owned interface |
| **Audio** | Engine-owned AudioServer over miniaudio; Steam Audio for spatial acoustics |
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
  specs/          Target specifications — 36 capabilities. Start here.
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
