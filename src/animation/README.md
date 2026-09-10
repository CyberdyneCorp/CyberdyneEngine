# `src/animation/` — CyberAnimation

The animation runtime: skeletons and bone level of detail, clips and their codec, the runtime of the
compiled pose program, root motion, the constraint framework, retargeting, the GPU pose world and
pose sharing. M8.b section 5, `animation-and-skinning` → **Working**.

## The one sentence that decides where everything lives

**The compiler is not here.** `animation-and-skinning` asks for *"graph → typed IR → optimisation →
compact program"*, and that is M8.b task 2.4's `cy/graph/lower_pose.h` — the animation IR with poses
as values, a lazily-evaluated state machine and pose dependency analysis. This module holds the
**runtime that program drives** and the **assets it names**, which is the split the specification
draws itself: *"Compilation SHALL occur at cook time; the runtime SHALL contain no graph compiler."*
Nothing under `src/animation/` parses or lowers a graph.

```
   an authored graph                     cy::graph::pose::compile_pose      (cook time)
        │
        ▼
   PoseProgram ── instructions, states, transitions, masks, clip table
        │
        │  cy::animation::AnimationRig::bind(skeleton, program, clips)
        ▼
   advance(rig, instance, dt, events)     THE DETERMINISTIC HALF
        │                                 clip clocks, the state machine, ROOT MOTION, events
        ▼
   evaluate(rig, instance, bone_lod, scratch, out_local, stats)
        │                                 poses blended, layers masked, IK solved
        ▼
   publish_pose(skeleton, local, lod, world, handle, …)
        │                                 local → model → skinning matrices → the pose world
        ▼
   PoseWorld ── current and previous matrices, a dirty range for the renderer to transfer
                                          → cy::render::geometry::SkinningDescriptor
```

## What each header is

| Header | What it owns |
|---|---|
| `skeleton.h` | The deformation hierarchy: joints ordered parent-before-child, bind poses, bone LOD levels as nested joint masks, per-joint bounds, the humanoid profile, and the one-pass local → model → skinning-matrix path |
| `clip.h` | The clip asset and its codec: tracks, quantised keys with per-track ranges, curve fitting to a tolerance **in millimetres and degrees**, constant collapsing, a per-track cursor, markers, events and the root motion track. Property tracks bind by `TypeId`/`FieldId`, so a rename does not break a clip |
| `evaluate.h` | The runtime: `AnimationRig` (skeleton + program + clips), `AnimationInstance` (the small state block), `PoseScratch` (caller-owned pose buffers), `advance()`, `evaluate()` and `AnimationBatch` |
| `lod.h` | Animation LOD tiers, the policy that chooses one **from simulation state alone**, hysteresis, and the pose cache pose sharing runs through |
| `ik.h` | The constraint framework: declared reads and writes, conflict detection at setup, two-bone IK and look-at, each weighted |
| `retarget.h` | Retargeting by semantic chains, at runtime and as an offline bake |
| `pose_world.h` | The **GPU pose world**: current and previous bone matrices per instance, add and remove without a rebuild, derived velocities, and the upload range |

## Four decisions worth knowing before changing anything here

### 1. Root motion is computed in `advance()`, and that is the determinism contract

`animation-and-skinning` — *"Root motion SHALL be computed on a deterministic CPU path from clips'
root tracks with blend weights composed on the CPU, regardless of where the rest of the pose is
evaluated and regardless of the instance's animation LOD tier."*

`evaluate()` is the part a tier may skip, a pose cache may satisfy and a GPU may one day perform.
`advance()` is the part that may not. So root motion is integrated in `advance()` from the clips'
root tracks over the whole interval, weighted by the same blend weights the pose tree uses. An
instance at `Baked` tier — whose pose is never evaluated at all — travels exactly as far as one at
`Full`, and `test_evaluate.cpp` asserts it in both directions, along with 60 Hz against 10 Hz.

### 2. The runtime evaluator is this module's, not `cy::graph`'s

`cy::graph::pose::evaluate()` exists and is correct for what it is, and the frame does not call it,
for two reasons that are properties of where it lives:

* `cy::graph` depends on `cy::core-base`, `-memory` and `-values` and **not** on `cy::core-math`. Its
  pose value is eight floats per joint blended componentwise, so it cannot slerp — and a rotation
  blended componentwise is not a rotation.
* it allocates a scratch array per call. At *"50,000 characters visible"* a heap allocation per
  instance per frame is the cost.

The evaluator here walks the same program, honours the same `PoseInstruction::required` masks, is
lazy in the same way, works over `Transform`, and takes one caller-owned `PoseScratch` reused across
a whole batch.

### 3. Laziness is measured, not intended

Three counters in `EvaluationStats` are what the specification's *"they SHALL NOT be sampled"* turns
into: `clips_sampled`, `joints_sampled` and `instructions_skipped`. A state nobody is in is never
walked; a layer at zero weight does not sample its clip; a masked layer samples only the joints its
mask retains; and a bone LOD level that drops the head and the fingers drops them from the sample
count too. `SampleStats::tracks_skipped` says the same thing one level down, inside `Clip::sample`.

### 4. The GPU pose world is the seam M6 declared and refused

`src/servers/render/geometry/skinning.h` declared `PoseSource::GpuPoseWorld` at M6 and had
`SkinningDescriptor::validate()` **refuse** it naming M8, so that a second pose upload path could not
accumulate consumers before the shared world existed. `pose_world.h` is that world. The refusal is
gone; what `validate()` still refuses is a descriptor that cannot mean anything — a `Baked` instance
claiming a place in the world it has no skeleton in, and a per-skin upload carrying an offset that
belongs to the shared world. `test_pose_world.cpp` builds a descriptor from an offset the world
actually issued and validates it, so the join is asserted from both sides.

## What is NOT here, and is not pretended to be

The M8.b row of `docs/ROADMAP.md` scopes this capability to *"Skeletons and bone LOD, clips and
compression, the animation graph, compiled programs, batched evaluation, layers and masks, root
motion, IK, retargeting, the GPU pose world, pose sharing"*. `animation-and-skinning` has thirty
requirements and that row is not all of them. What is deliberately absent, each stated rather than
approximated:

| Requirement | Status |
|---|---|
| **Clip streaming** | Absent. Residency over `core-assets-and-io`; there is no partial residency in `clip.h` and no `sample()` that can report an underrun |
| **Cubic interpolation** | Accepted as an authoring mode and **stored linear**: `add_key` carries no tangent, so a cubic track keeps more keys to hold its tolerance rather than reproducing its tangents. Stated at the enumerator |
| **Motion matching and pose search** | Absent. A pose database, its feature extraction and a deterministic index are their own body of work |
| **Animation warping** | Absent. Motion, stride and orientation warping and distance matching |
| **Control rig** | Absent as an asset. The constraint framework it would compile against is here; the rig graph and its compiler are not |
| **Full-body IK, FABRIK, CCD, spline IK, spring bones, foot placement** | Refused **by name**: `solve_unimplemented()` returns `NotImplemented` for each, and `test_ik.cpp` asserts it. A chain solver answering to `FullBodyIk` would be worse than no answer |
| **Physics animation** (powered/partial/full ragdoll, hit reactions) | Absent. It is a join with `physics` |
| **Facial animation** (blendshapes, visemes, the ML path) | Absent. Blend shapes exist on the renderer's side in `cy::render::geometry::BlendShapeSet` |
| **Tweens** | Absent. Property TRACKS are here; code-driven tweening with easing and sequencing is not |
| **The rigging workspace** | Absent. Editor work |
| **Animation diagnostics as a view** | Partly. Every figure the diagnostics requirement names is *reported* — `EvaluationStats`, `CompressionReport`, `PoseCacheStats`, `PoseWorldStats`, `LodDistribution`, `RetargetReport`, `ConflictReport` — and nothing draws them |

## Building without it

`-D CY_ANIMATION=OFF` excludes this directory and its three suites, and nothing else changes:
*"the animation runtime SHALL be excluded, and static meshes SHALL render unaffected."* The option
does **not** gate `cy::graph`'s pose lowering, which is a compiler and belongs to `visual-scripting`.

## Suites

| Suite | Kind | What it covers |
|---|---|---|
| `unit.animation` | unit | The asset model and the algebra over it: skeleton order and bone LOD, clip compression and cursored sampling, tier selection and the pose cache, constraint conflicts and the two-bone solver, retargeting and its bake |
| `integration.animation_runtime` | integration | Compiling a graph, binding a rig, laziness, the state machine's blend, root motion across tiers and rates, batched evaluation of five hundred instances, events, and the pose world's handoff to skinning |
| `integration.animation_teardown` | integration | Sixty-four rounds of building and tearing down rigs, batches and pose worlds with four spinner threads holding the cores |
