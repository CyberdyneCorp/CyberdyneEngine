# Design: CyberRenderer and CyberMaterial

## 1. One arbiter measures; subsystems receive allocations

This is the decision with the widest reach, and it corrects something the existing specifications
got wrong by accretion.

Today `vfx-system`, `virtual-geometry`, and `rendering-post-processing` each describe a controller
that measures GPU time and reduces quality to hold a target. Each was reasonable in isolation.
Together they form three feedback loops reading one shared, noisy signal — total GPU frame time —
with no coordination. The failure is predictable:

- A heavy geometry frame raises total GPU time. VFX sees the frame is slow and cuts particles,
  even though particles were not the cause.
- Dynamic resolution drops internal resolution, which reduces geometry *and* VFX cost, so both
  controllers now see headroom and increase quality, which pushes the frame over again.
- The result is visible pumping in three subsystems at once, with no single component responsible
  and nothing in the profiler that says why.

The resolution: **exactly one component measures the frame and allocates it.** The renderer budget
arbiter holds a per-subsystem allocation — geometry, shadows, GI, materials, VFX, post — and each
subsystem's controller holds *its own allocation* using its own levers. Subsystems report their
measured cost to the arbiter; they do not read frame time.

That keeps what the subsystem controllers are genuinely good at — only the VFX system knows that
spawn rate is a cheaper lever than simulation frequency — while removing what they were bad at,
which was inferring global cause from a global symptom.

Two properties follow and are specified as requirements rather than left to implementation:

- **Allocations move slower than the frame.** The arbiter adjusts on a longer time constant than
  the subsystem controllers, so a subsystem is never chasing a moving target. A fast inner loop
  under a slow outer loop is stable; two fast loops are not.
- **Reserved floors are honoured before reallocation.** A subsystem that has hit its declared
  minimum reports so, and the arbiter reallocates from elsewhere rather than continuing to squeeze
  a subsystem that has nothing left to give.

Pinned mode becomes global: capture, cinematics, and deterministic tests disable the arbiter, and
every subsystem controller is pinned with it. Half-pinned is not a state that should exist.

## 2. Renderer profiles are presets, not renderers

`Mobile`, `Standard`, `HighEnd`, `Cinematic` could easily become four codebases. They must not.

A profile is a **named configuration** — pipeline, feature set, quality tiers, budget
allocations — over one renderer. The constraint that keeps it honest is stated as a requirement:
the same content SHALL render under every profile it targets, differing in fidelity and
performance, not in whether it appears. A profile that requires content changes is a porting
problem wearing a preset's clothing.

Where a profile genuinely cannot express something — the mobile pipeline's per-object light lists
versus a cluster grid — the difference is in the pipeline, which is already a pluggable component,
not in the profile.

## 3. Bindless is the default, and the fallback's cost is stated

The current descriptor requirement says bindless is "preferred where available", with a
non-bindless fallback. That was written when the renderer was less GPU-driven than it has since
become. Indirect draws generated on the GPU from a GPU scene cannot bind a descriptor set per
draw — there is no CPU in the loop to do it.

So bindless becomes the **default architecture**, and the non-bindless path becomes a
**compatibility path** whose limitations are documented: it cannot execute fully GPU-generated
draw workloads and therefore constrains virtual geometry and GPU-driven culling to a
CPU-submitted approximation. This is a real limitation and it is better written down than
discovered on the one device that needs it.

## 4. Closures are the authoring model; shading models are the lowered form

The existing specification fixes eight shading models, each behind a specialization constant. The
proposed direction is a general closure model — diffuse, specular, coat, transmission, emission,
composable into layered surfaces.

Both are right, at different levels, and the resolution is a lowering rule rather than a choice:

- The material graph and the material IR speak in **closures**. An artist layers a coat over a
  metallic base, or adds transmission to a leaf, without picking from a menu of eight.
- The compiler **matches the closure set against the known shading models**. A closure set that
  is exactly the standard model compiles to the standard model's code path, with identical cost to
  a material authored against it directly.
- Only a closure set with no matching model compiles to the **generic layered evaluator**, which
  is more expensive and is reported as such at cook time.

This is the point that matters: generality is available and costs what it costs, while the common
case — which is most materials — pays nothing for the generality it does not use. A system where
every material pays layered-evaluation cost because layering exists would be a regression, and
that is the trap the requirement is written to avoid.

The eight shading models remain in `rendering-materials-and-shading` as the specified lowering
targets. They are not deleted; they change status from *the* material model to the fast paths.

## 5. Static parameters change code; runtime parameters change data

A parameter that changes the shader's structure (`UseClearCoat`) and a parameter that changes a
number (`Roughness = 0.42`) are not the same thing, and treating them the same is how permutation
counts explode.

The compiler classifies every parameter as **static** or **runtime**, and the classification is
derived, not declared: a parameter is static only if it feeds a control-flow or resource decision
that cannot be expressed as data. Authors may not promote a parameter to static to make it faster.

The permutation budget already specified in `shader-system` then has something concrete to bound,
because the set of static parameters is exactly the permutation axis set.

## 6. Materials compile against attribute semantics, not vertex layouts

A material asks for `input.normal` and `input.uv(0)`. It never asks for a buffer slot or an
offset. Each geometry source — static mesh, skinned mesh, virtual geometry, terrain, mesh
particle, procedural — supplies an **attribute decoder** implementing that interface.

Under the visibility buffer pipeline this is close to free: material resolve reconstructs
attributes from an identified instance and primitive, and the decoder is a function of the
instance's geometry kind. One material program serves every geometry source.

Under Forward+ it is not free, and the specification says so: attributes are produced by the
vertex stage, so a material still needs a variant per geometry source. That is the vertex factory
problem, and Forward+ has it. Recording this honestly is worth more than a requirement claiming a
decoupling the pipeline cannot deliver — and it is one of the concrete reasons the visibility
buffer path scales better with material count.

## 7. Quality tiers are generated, and they are a bounded permutation axis

A hero material at 3 metres and at 300 metres should not be the same program, and should not be
three hand-authored materials either. The compiler generates tiers by progressively removing
closures and inputs whose screen-space contribution falls below a threshold.

The obvious risk is that tiers multiply every other permutation axis. So tiers are declared as a
permutation axis with a fixed, small cardinality (default 3), participate in the existing
permutation budget, and use specialization constants wherever the difference is a constant rather
than a structure. A material may opt out and be authored per tier.

## 8. One temporal framework, not one per effect

TAA, upscaling, SSR, screen-space GI, AO, and shadow caching all need the same six things: a
jitter sequence, motion vectors, history storage, reprojection, camera-cut detection, and
disocclusion. Specified per effect, they will be implemented per effect and will disagree.

`temporal-rendering` owns them. It also owns the **invalidation events** — camera cut, resolution
change, projection change, scene reload, teleport — because a history buffer that is not
invalidated on a cut is the single most common temporal artefact, and it should be impossible to
forget rather than a thing each effect remembers.

Motion vectors are the interesting case: the GPU scene already stores current and previous
transforms, and the GPU pose world already stores current and previous poses. Motion vectors are
therefore *derived* from data that already exists, not a separate authoring concern, and dynamic
geometry gets correct vectors without per-system effort.

## 9. PSO management is pipeline-agnostic and moves

The pipeline compilation strategy currently specified in `rendering-forward-clustered` — collect
used permutations, cook a manifest, warm the cache at load, use a fallback pipeline for anything
missing while it compiles asynchronously — is correct and is not specific to Forward+. It moves to
`shader-system`, is broadened to cover every pipeline, and gains the parts that were missing: a
tiered derived-data cache (local, shared read-only, CI-writable), and a compilation service that
can run on a build farm rather than only on the developer's machine.

Nothing is lost in the move; the requirement is removed from the pipeline spec with a supersession
note, and its scenarios are preserved in the new home.

## 10. Ray tracing infrastructure now, GI and virtual shadows next

Acceleration structure management is renderer architecture: BLAS lifecycle and refit budgets, TLAS
rebuild, and — the part that is genuinely engine-specific — a **geometry adapter per geometry
source**, since virtual geometry cannot feed a BLAS directly and must supply a proxy, skinned
meshes must feed from the GPU pose world, and procedural geometry must supply bounds and an
intersection shader.

Specifying that here means the GI and shadow work that follows consumes an interface rather than
inventing one. The GI stack itself, and virtual shadow maps, are deliberately excluded — they are
the announced next change, and folding them in would have made this one unreviewable.

## 11. The material IR is engine-owned; the shader backend is not

Slang remains the shader authoring and target language, and the material compiler emits Slang like
every other engine generator. What the engine owns is the **material IR** and its passes —
constant folding, dead-node elimination, common subexpression elimination, sample deduplication,
closure simplification, uniform and varying analysis — because that is where material cost is
actually decided, and because the IR is what makes cost attribution back to graph nodes possible.

The engine does not own a shader optimiser. It owns a material optimiser that produces good input
to somebody else's shader optimiser.

## 12. Build order

The brief's milestone structure is adopted, because it sequences risk correctly: prove the
foundation before adding the systems that depend on it.

| Milestone | Contents | Proves |
|---|---|---|
| **V1** | RHI, render graph, GPU scene, GPU culling, depth, one pipeline, standard PBR, clustered lights, basic shadows, TAA, tonemap | The foundation renders and the abstractions hold |
| **V2** | Virtual geometry, virtual textures, visibility buffer material resolve, material bins, async compute | Density and material count stop being CPU problems |
| **V3** | Dynamic GI, reflections, hardware ray tracing, advanced transparency, volumetrics, atmosphere, terrain, water | The renderer looks modern |
| **V4** | GPU-driven skinned geometry, hair, path tracer, cinematic rendering, foveated XR, ML upscaling | Reach |

**V1 is the milestone that matters.** Everything after it is an addition to a structure that
already works; if V1's abstractions are wrong, none of the later work will save them.

## 13. Gaps this change does not close

- **Virtual shadow maps** and the **dynamic GI stack** — the announced next change. Seams only.
- **Virtual textures** — still unspecified, still the companion to virtual geometry, and now also
  the residency partner the material system's texture references will need.
- **World partition** — now referenced by networking, virtual geometry, and implicitly by any
  serious use of renderer profiles at open-world scale. It remains the most-referenced missing
  capability in the specification set.
