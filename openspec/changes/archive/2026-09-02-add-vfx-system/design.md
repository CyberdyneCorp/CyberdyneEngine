# Design: VFX system

## Context

Visual effects sit at an unusual intersection: they are simultaneously content (authored by
artists, iterated constantly), a compiler problem (graphs must become efficient GPU code), a
scheduling problem (thousands of effect instances competing for a fixed GPU budget), and a
renderer problem (they produce geometry, lights, and decals that the renderer must consume).

Getting the layering right at the start matters more here than in most subsystems, because the
three decisions that are expensive to retrofit — GPU-first execution, compiled graphs, and a
shared simulation world — are all foundational.

## Decisions

### 1. Build, do not integrate

VFX is engine-built. There is no mature permissive library for programmable GPU VFX, and the
leading implementations are engine-internal for a structural reason: the system must be
co-designed with the renderer's instance representation, the frame budget controller, and the
shader pipeline. A bolt-on VFX library would own its own dispatches, its own buffers, and its own
scalability policy, which is precisely what this design is trying to avoid.

### 2. GPU-first, with a CPU path that is not a fallback in disguise

GPU compute simulation is the default. The CPU path exists for two distinct reasons that are
worth separating:

- **Capability fallback** — devices without adequate compute. Documented lower budgets.
- **CPU-visible effects** — the small set of effects whose results gameplay must read
  synchronously. These are a *different use case*, not a degraded one, and are specified as such.

**Alternative rejected — CPU-first with GPU as an optimisation.** This is what most engines grew
into, and it caps particle counts at tens of thousands because the CPU→GPU upload becomes the
bottleneck. Designing for millions means the data must live on the GPU and stay there.

### 3. Compile graphs; never interpret them

An effect graph compiles through a typed IR to Slang, then through the engine's existing shader
pipeline to SPIR-V and backend code.

```
VFX Graph  →  VFX IR  →  optimisation  →  Slang  →  SPIR-V  →  backend
                          ├─ attribute analysis
                          ├─ dead-code elimination
                          ├─ constant folding
                          └─ kernel fusion
```

**Rationale.** An interpreter pays per-particle dispatch overhead and cannot eliminate work the
graph does not need. A compiler can prove which attributes are live, fuse spawn and update stages,
and fold artist-set constants into the generated code. This is the difference between a graph
being a convenience and a graph being free.

**Reconciliation with the locked shader decision.** The engine's shading language is Slang; the
VFX compiler therefore emits **Slang**, not HLSL or MSL directly, and reuses the existing
compilation, reflection, caching, and hot-reload machinery. Emitting HLSL and adding a DXC path
would create a second shader toolchain and a second cache, and would break the Metal backend's
translation path.

### 4. Attribute layout is derived, not declared

Particle storage is structure-of-arrays, and the *set* of arrays is computed by the compiler from
the graph's live attributes.

```
Graph reads/writes:  Position, Velocity, Age, Lifetime, Colour
Graph never touches: Rotation, Mass, Custom0..3

Allocated storage:   Position[]  Velocity[]  Age[]  Lifetime[]  Colour[]
```

**Rationale.** A fixed particle struct wastes bandwidth on every effect that does not use every
field, and bandwidth is the binding constraint at millions of particles. Deriving the layout means
a simple effect is genuinely cheap rather than merely simple to author.

**Trade-off accepted.** Layout depends on the graph, so a graph edit changes the buffer layout and
invalidates in-flight simulation state. Hot reload therefore restarts affected effect instances
rather than migrating their particles — acceptable for authoring, and specified explicitly.

### 5. One simulation world, one scheduler

Effect instances do not each own a dispatch. A global scheduler collects all active emitters,
groups those sharing a compiled kernel and compatible parameters, and issues merged dispatches
over a shared particle pool.

```
   Explosion×40   Sparks×300   Dust×1200   Smoke×80
        └──────────────┴────────────┴──────────┘
                          │
                  VFX Scheduler
                          │
          group by kernel + compatible state
                          │
              a handful of indirect dispatches
```

**Rationale.** At RTS scale the dominant cost of a naive design is thousands of tiny dispatches
and their state changes, not the simulation arithmetic. Merging is only possible if it is
foundational: retrofitting it means changing how every effect allocates and addresses its
particles.

**Trade-off accepted.** A shared pool means one effect's overspend is another's starvation, which
is why the budget controller and per-importance-class reservations are specified alongside it.

### 6. Data interfaces are the extensibility mechanism

Graphs read engine data through typed **data interfaces** rather than through bespoke nodes wired
into the compiler. A data interface declares its readable fields and the GPU resources it binds;
the compiler treats it as an opaque typed source.

**Rationale.** This is the single best idea in Niagara's design. It means adding "particles can
sample the wind field" is a data interface implementation, not a compiler change, and third-party
modules can add their own without touching engine code.

### 7. VFX publishes into the GPU scene

Mesh particles publish `{transform, mesh, material, flags}` into the renderer's GPU-side instance
representation, participating in GPU-driven culling and indirect drawing like any other instance.

**Rationale.** A million mesh particles must not become a million ECS entities or a million CPU
draw submissions. If VFX and the renderer share one instance representation, mesh particles are
free of special handling.

**Prerequisite surfaced.** The GPU scene is currently implicit in the specs — GPU-driven culling
and instanced meshes both assume it without naming it. This change names it in
`rendering-architecture` so VFX has a defined contract to publish into.

### 8. Events stay on the GPU

Collisions and lifetime events write into GPU event buffers consumed by other emitters' spawn
stages in the same or next frame, with no CPU round trip.

```
bullet impact → sparks → spark hits ground → dust
     all on the GPU; the CPU sees none of it
```

**Guard.** Event-driven spawning is a feedback loop and can diverge. Every event channel has a
hard per-frame spawn budget, and chains have a maximum depth. Exceeding either drops events
deterministically-by-rank and reports it, rather than compounding.

**CPU events are a separate, bounded path.** Where gameplay genuinely must know (a particle
triggered a gameplay event), a readback path exists with explicitly documented latency of at least
one frame. It is not the default and it is budgeted.

### 9. VFX must not influence gameplay state

GPU simulation is non-deterministic: atomics, floating-point ordering, async compute overlap, and
variable simulation rate all make bit-exact reproduction impossible across runs and devices.

The engine already requires deterministic physics and deterministic re-simulation for network
reconciliation. VFX is therefore **firewalled**: it may read gameplay state, and it may not write
it. Anything gameplay-affecting is a gameplay system's job, computed deterministically on the CPU.

**Rationale.** Without this boundary stated as a requirement, someone will eventually spawn damage
from a GPU collision event, and network reconciliation will diverge in a way that is extremely
hard to diagnose. The boundary costs nothing to state now.

### 10. Budget, not presets

Effects declare an importance class (`Critical`, `Important`, `Ambient`, `Decorative`). A
controller measures GPU VFX time and adjusts spawn rates, simulation frequency, collision quality,
LOD, and renderer features to hold a configured target.

```
target 2.0 ms   measured 3.4 ms
        │
        ├─ reduce Decorative spawn rate
        ├─ halve Ambient simulation frequency
        ├─ drop distant collision to analytic
        └─ merge compatible emitters more aggressively
        │
   measured 2.1 ms
```

`Critical` effects are reserved capacity and are degraded last, so gameplay-legible effects
survive.

**Rationale.** Low/Medium/High presets do not survive contact with an RTS scene where the effect
count varies by two orders of magnitude between a quiet base and a large battle. A controller
adapts; a preset either wastes headroom or blows the frame.

**Trade-off accepted.** Adaptive quality means visuals vary with load, which is inappropriate for
cinematics and for capture. A pinned mode disables adaptation.

### 11. Simulation rate is per effect, not global

Effects simulate at a rate chosen from importance and distance (60, 30, 15, or 8 Hz), while
rendering interpolates at frame rate.

**Rationale.** A distant smoke plume simulated at 8 Hz and interpolated is visually
indistinguishable from one simulated at 60 Hz, and costs an eighth as much. Across a battlefield
this dominates.

## Risks

- **Compiler complexity.** The IR and its optimisations are the hardest part of this system and
  the easiest to under-scope. Mitigation: the IR is specified as a requirement with defined
  passes, and the first milestone targets correctness with only attribute analysis and dead-code
  elimination; fusion comes later.
- **Shared pool contention.** One effect can starve others. Mitigation: per-importance-class
  reservations and the budget controller.
- **Debuggability.** Compiled GPU graphs are hard to debug. Mitigation: the diagnostics
  requirement mandates IR and generated-source inspection, per-effect GPU timing, and a particle
  inspector.
- **GPU scene coupling.** VFX depends on a renderer concept being named and stable. Mitigation:
  this change names it; if the renderer later restructures it, VFX has one defined contract to
  update rather than scattered assumptions.
- **Cross-backend behaviour.** Generated kernels must behave identically on Vulkan and Metal.
  Mitigation: golden-image and statistical tests across backends, specified in the validation
  requirement.

## Open questions

- **Fluids.** Deferred. The seams reserved are: a grid data interface, a volume renderer, and the
  scheduler's ability to host non-particle simulation stages. Whether the eventual solver is
  written or integrated is deliberately not decided here.
- **Offline simulation import** (Houdini caches). Deferred; the attribute-array model would
  accommodate a cache playback emitter without restructuring.
- **Whether the budget controller should be shared with the renderer's LOD system** rather than
  VFX-specific. Likely yes eventually; specified as VFX-local for now to avoid coupling two
  unbuilt systems.
