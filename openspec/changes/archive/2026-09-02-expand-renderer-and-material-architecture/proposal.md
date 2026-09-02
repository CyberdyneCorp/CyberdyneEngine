# Expand the renderer and material architecture

## Why

The specification set already contains a great deal of renderer: an explicit RHI, a render graph
that computes its own barriers and aliasing, a GPU scene, GPU-driven culling, a Forward+ pipeline,
a visibility buffer pipeline, virtual geometry, clustered lighting, and a Slang shader toolchain.

What it does not yet contain is the *architecture that holds those together*. Four things are
missing, and each of them is the kind of gap that is cheap now and structural later:

**1. Nothing owns the frame's cost.** VFX has a budget controller. Virtual geometry has a budget
controller. Post-processing has dynamic resolution scaling. Each measures GPU time and reacts to
it. Three independent feedback loops observing one shared signal is not three budgets — it is one
unstable control system. Under load, each sees time it did not cause and each reduces quality
for it, so the frame overshoots its correction and then oscillates. This has to be resolved at the
architecture level, not discovered in a profiler.

**2. The material system stops at shader generation.** `shader-system` generates a shader from a
material definition, and `rendering-materials-and-shading` fixes the BRDF and eight shading
models. Between them there is no compiler: no intermediate representation, no optimisation, no
cost model, no quality tiers, no way to express a layered surface that is not one of the eight
models, and no answer to how one material compiles once and runs on a static mesh, a skinned
mesh, virtual geometry, terrain, and a mesh particle without multiplying variants by geometry
type — the problem Unreal solves with vertex factories and pays for in compile times.

**3. Every temporal technique reinvents history.** TAA, temporal upscaling, screen-space
reflections, screen-space GI, ambient occlusion, and shadow caching each need history buffers,
reprojection, jitter, camera-cut detection, and disocclusion handling. Specified separately, they
will be implemented separately, disagree about jitter and motion vector conventions, and each
carry their own history-invalidation bug.

**4. Pipeline state object management lives in one pipeline's specification.** It is in
`rendering-forward-clustered`, but the visibility buffer pipeline, the mobile pipeline, and every
custom pipeline need it identically. Hitching on first use of a pipeline is not a Forward+
problem.

The organising principle, stated once so the rest follows from it:

> **CyberRenderer is a GPU-driven architecture built around a declarative render graph, a
> persistent GPU scene, a bindless resource model, a visibility-first geometry path, and a
> compiled material system. Geometry, material evaluation, and lighting are decoupled so that
> millions of instances and heterogeneous material models are scheduled as large GPU workloads
> rather than as individual CPU draw calls.**

The consequence worth stating plainly: there is no longer a one-to-one relationship between an
object, a draw call, and a material shader. Specifications that assume there is will be wrong.

## What changes

**Three new capabilities:**

- **`material-compiler`** — CyberMaterial. Graph → IR → optimisation → closures → surface program.
  Static versus runtime parameters, quality tiers, the semantic geometry attribute interface,
  material classification and binning, the GPU material table, cost analysis attributed to graph
  nodes, and derived-data versioning.
- **`temporal-rendering`** — CyberTemporal. One framework owning jitter, motion vectors, history
  buffers, reprojection, camera cuts, disocclusion, and history invalidation, consumed by every
  temporal technique.
- **`ray-tracing-infrastructure`** — acceleration structure lifecycle, geometry adapters for each
  geometry source, the ray query interface, and the capability gate. Built now because it is
  renderer architecture; consumed by the GI and shadow work that follows.

**Added to `rendering-architecture`:** renderer profiles, render features as the module unit, the
render pipeline configuration asset, and — the load-bearing one — the **renderer budget arbiter**
that owns measurement and allocation, turning the existing subsystem controllers into consumers
of an allocation rather than independent observers of the same clock.

**Modified elsewhere:** bindless becomes the default path rather than the preferred one; shading
models become the lowered form of closures rather than the authoring model; material parameters
gain the static/runtime split and compile-time identifiers; PSO management moves from the Forward+
pipeline to `shader-system` and becomes pipeline-agnostic; anti-aliasing and temporal upscaling
consume the temporal framework; decals become GPU scene residents; ray-traced effects consume the
new infrastructure; and the VFX and virtual geometry budget controllers subscribe to the arbiter.

## Impact

- **New**: `material-compiler`, `temporal-rendering`, `ray-tracing-infrastructure`
- **Modified**: `rendering-architecture`, `rhi-and-render-graph`, `shader-system`,
  `rendering-materials-and-shading`, `rendering-forward-clustered`, `rendering-post-processing`,
  `rendering-lighting-and-shadows`, `rendering-global-illumination`, `vfx-system`,
  `virtual-geometry`, `editor-architecture`, `thirdparty-dependencies`
- **Not in scope, and deliberately next**: virtual shadow maps and the dynamic GI stack. The
  seams are specified here; the systems are not. Specifying them alongside this change would have
  produced a proposal too large to review honestly.
