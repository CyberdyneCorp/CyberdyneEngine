# Design: CyberAnimation

## Context

Animation touches more subsystems than anything else in the engine: it consumes assets, feeds
skinning and the renderer, drives and is driven by physics, is queried by VFX for attachment
points, is triggered by AI, emits events to audio and gameplay, and produces root motion that is
gameplay state. Designing it late means designing it around decisions already made elsewhere.

The existing specification is adequate for a few hundred characters and inadequate for the scale
the rest of the engine targets.

## Decisions

### 1. Skeleton and rig are different assets

A **skeleton** is the runtime deformation hierarchy: joints, bind poses, parent indices, bone LOD
levels. It is small, flat, and evaluated in bulk.

A **rig** is the artist-facing control system: controls, IK chains, constraints, space switching,
pose drivers, and the maths connecting them. It is a program.

```
  Rig (authoring)                     Skeleton (runtime)
  HandControl → TwoBoneIK      →      UpperArm · Forearm · Hand
```

**Rationale.** Conflating them is what makes engine rigging weak. If controls live in the skeleton,
runtime data carries authoring concepts it never uses, and rigging is limited to what the runtime
hierarchy can express. Separating them lets the skeleton stay lean for 50,000 instances while the
rig stays expressive for the twenty characters that need it.

**Consequence.** Two asset types, two editors, and a defined compilation from rig to a program that
runs against a skeleton.

### 2. Graphs and rigs compile to shared programs

Animation graphs and control rigs both compile: graph → typed IR → optimisation → compact program.
All instances using a graph share one program; per-instance state is a small block.

**Rationale.** This is the third time this pattern appears (VFX, AI, now animation), for the same
reason each time: interpreting a node graph per instance per frame is what caps instance counts,
and a compiler can eliminate work the graph does not need — dead branches, constant-folded
parameters, and pose dependencies that are never read.

**Pose dependency analysis is the animation-specific optimisation.** If a graph's output is masked
to the upper body, the lower-body pose of a contributing clip need not be sampled at all. Knowing
that statically is worth more than any micro-optimisation of the sampler.

### 3. Pose evaluation is separate from skinning, and poses live on the GPU

```
Anim graph → local pose → global pose → bone matrices → GPU Pose World
                                                              │
                                          ┌───────────────────┼──────────────┐
                                          ▼                   ▼              ▼
                                      skinning          VFX attachment   rendering
```

The **GPU pose world** is to animation what the GPU scene is to rendering: one shared GPU-side
representation with multiple producers and consumers, holding current and previous bone matrices.

**Rationale.** Without it, every consumer needs its own copy and its own upload, and VFX attachment
to a bone requires CPU readback. With it, a sword trail reads the hand bone on the GPU where the
data already is.

**Trade-off accepted.** Anything the CPU needs from the pose — root motion, IK targets resolved
against the world, gameplay queries like "where is the muzzle" — must either be computed CPU-side
or read back with a frame of latency. The root motion decision below resolves the important case;
the rest is documented.

### 4. Root motion is gameplay, and therefore deterministic

Root motion drives the character controller. That makes it gameplay state, subject to the same
determinism contract as physics and AI.

**Requirement.** Root motion SHALL be computed on a deterministic CPU path, regardless of where the
rest of the pose is evaluated or at what LOD tier the instance is running.

**Why this is not obvious.** Two mechanisms in this design would otherwise break it:

- **Animation LOD.** If a distant character evaluates at 10 Hz, its root motion per tick differs
  from full-rate evaluation. That is acceptable *only* if the tier is a deterministic function of
  simulation state — the same rule the AI system already establishes — and if root motion is
  integrated correctly across the reduced rate rather than sampled naively.
- **GPU pose evaluation.** GPU results are not bit-reproducible across devices. Root motion derived
  from a GPU-evaluated pose would desync.

So: poses may be evaluated anywhere, root motion is computed on the CPU from the clip's root track,
deterministically, at a rate the determinism mode permits.

**Alternative rejected — root motion from the evaluated pose.** Simpler and more general (it would
compose through arbitrary graph topology), but it couples gameplay state to GPU evaluation and to
LOD scheduling. Extracting from the root track, with blend weights composed on the CPU, keeps the
authoritative path narrow and cheap.

### 5. Animation LOD reduces fidelity, not only rate

| Tier | Evaluation | Skeleton | Rate |
|---|---|---|---|
| `Full` | Full graph, all layers, IK, control rig | Full bone set | Up to frame rate |
| `Simplified` | Reduced graph: locomotion only, no IK, no control rig | Reduced bone set | 30 Hz |
| `Cached` | Shared pose cache sample, no per-instance evaluation | Reduced bone set | 10–15 Hz |
| `Baked` | Pose texture or vertex animation; no skeleton evaluation | None | Sampled |

Rendering interpolates between evaluations at full frame rate regardless of tier.

**Rationale.** Reducing only the rate still pays full cost per evaluation. Dropping the control
rig, IK, and two thirds of the joints is where the order of magnitude comes from.

### 6. Pose sharing for crowds

Instances running the same clip at the same phase can share an evaluated pose. A pose cache keyed
on (clip, phase bucket, skeleton LOD) is populated once per frame and sampled by many instances,
which then apply cheap per-instance variation — phase offset, upper-body override, aim offset.

**Rationale.** Ten thousand infantry in the same walk cycle is the common case in the workloads
this engine targets, and it is almost entirely redundant evaluation.

**Trade-off accepted.** Phase quantisation into buckets means shared instances are not perfectly
phase-continuous. Bucket count is a quality setting; at `Cached` tier the artifact is not
observable.

### 7. Motion matching is core, not an add-on

Pose search is built in: an offline pose database with extracted features (root velocity, foot
positions and velocities, trajectory, facing, contacts), and an indexed runtime search returning
the best-matching frame for a query.

**Rationale.** Hand-authored locomotion state machines are where most of the animation authoring
budget goes and where most of the visible quality problems remain — foot sliding, turn popping,
start-stop discontinuity. Motion matching converts that authoring problem into a data problem.
Treating it as core means the pose database, the feature extraction, and the search index are
designed alongside the clip format rather than bolted on.

**Determinism.** The search must be deterministic: an approximate index is acceptable only if it
returns the same result for the same query on the same data, which rules out anything with
non-deterministic tie-breaking or thread-order-dependent accumulation.

### 8. One constraint framework, several solvers

Two-bone IK, FABRIK, CCD, look-at, aim, spline IK, and full-body IK are solvers within one
constraint framework alongside position, rotation, parent, and physics constraints — not a list of
independent modifiers each with its own semantics.

**Full-body IK matters** because independent limb solvers fight: hands to a weapon, feet to the
ground, and head to a target, solved separately, produce a pose that satisfies none of them well.

**Rationale.** A unified framework means one ordering model, one conflict detection mechanism, one
debugging view, and constraints composable in ways their authors did not anticipate.

### 9. Physics animation rather than a ragdoll switch

Powered ragdoll: physics bodies follow an animated target pose through joint motors, with a
per-body blend between animation-driven and simulation-driven. Partial ragdoll applies it to a
subset — an arm goes limp while locomotion continues.

**Rationale.** The instantaneous switch from animation to ragdoll is one of the most recognisable
tells of a cheaply-made game. Blending is not much harder if the architecture anticipates it.

### 10. Integrate compression, own the format

Animation compression is a well-defined problem with a strong existing solution (ACL). The
engine's clip format and streaming are engine-owned; the compression codec is a candidate for
integration.

**Framed as an evaluation, not a decision.** ACL is MIT and well-regarded, but it brings its own
data model and the integration cost is real. The requirement specifies *error-bounded compression
with reported achieved ratios*; whether the codec is ACL or written is left to the implementation
change, with the evaluation criteria stated.

**OpenUSD is flagged separately.** It is not a small parser like ufbx — it is a large dependency
with its own build system and object model. It is specified as optional and tool-time only.

## Risks

- **Two compilers and a search index** is a lot of machinery. Mitigation: the graph compiler comes
  first; control rig and pose search are additive and can follow.
- **GPU pose world coupling.** Animation, skinning, VFX, and rendering all depend on one
  representation. Mitigation: it is specified as a contract with named producers and consumers,
  mirroring the GPU scene.
- **Root motion determinism is easy to lose.** Someone will eventually derive it from the evaluated
  pose because that composes more naturally. Mitigation: it is a requirement with a scenario, and
  the determinism test covers it.
- **Pose sharing artifacts** at close range. Mitigation: sharing is tier-gated, off at `Full`.

## Open questions

- Whether the animation LOD controller and the AI LOD controller should be one system. They score
  the same things (distance, importance, visibility) and a character usually wants consistent tiers
  across both. Likely yes; specified separately for now.
- Whether pose search should run on the GPU for very large crowds. The database is read-only and
  the query is embarrassingly parallel, so it is plausible; deferred pending measurement.
- Whether cloth should be a physics feature or an animation feature. Deferred; the constraint
  framework and spring-bone chains cover the cheap cases.
