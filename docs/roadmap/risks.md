# Risks and Deferrals

What is most likely to be wrong in [the roadmap](../ROADMAP.md), what is deliberately not being
built, and how each is kept honest.

---

## The principle

> Each milestone names the work within it that carries the most uncertainty, and that work is
> attempted **first**, as a time-boxed spike whose only deliverable is a decision.

A spike may violate the ladder — it may prototype a capability from a later milestone — provided the
prototype is **not merged** and the decision it produces is recorded. Where a spike's outcome would
change the roadmap, the roadmap change is proposed before the rest of the milestone proceeds.

The alternative is the ordinary failure: the hard part is left until the end of the milestone, and
by the time it is understood, the easy 80% has been built on an assumption that turns out to be
false.

---

## The register

Ordered by expected cost if the assumption is wrong, not by likelihood.

### 1 — Determinism holds across the whole simulation

**Milestone**: hooks at M2, validated at M9. **Owner**: `simulation-and-determinism`.

The hooks are cheap. The discipline is not: determinism is a property of every line of simulation
code, and a single unordered iteration, unseeded random draw or floating-point difference is
invisible until it is a desync months later.

*Why it is scheduled the way it is*: the commit boundary, seeded streams and hash hooks land at M2
so that M3 to M8 are written under the constraint rather than audited against it afterwards. The
validator at M9 can then **find** violations — it could never have **prevented** them.

*If the assumption fails*: `CrossPlatform` and `Lockstep` become unachievable profiles, and the
specification's promise that a session declaring an unachievable profile is rejected at
configuration becomes the whole of the value. That is a specification change, and the M9 spike is
where it would be discovered.

### 2 — The material IR expresses every graph consumer

**Milestone**: M7, consumed from M8. **Owner**: `material-compiler`, `visual-scripting`.

Seven subsystems lower through one shared graph infrastructure — materials, VFX, AI, animation,
camera rigs, PCG, abilities, visual scripts and sequences. The specifications commit to graphs
compiling rather than interpreting, which makes the IR the shared contract for all of them.

*If the assumption fails*: either a consumer gets its own compiler — losing the shared tooling,
debugging and merge behaviour — or the IR grows a general-purpose escape hatch that becomes an
interpreter by another name. Both are worth knowing at M7 with one consumer built rather than at M8
with six.

*Mitigation*: the M7 spike lowers **two** deliberately dissimilar graphs — a material and an ability
— before the compiler's shape is fixed.

### 3 — The budget arbiter's control loop is stable

**Milestone**: M7. **Owner**: `rendering-architecture`.

The specification requires exactly one component to measure GPU cost and allocate it, precisely
because several subsystems each measuring one shared signal would correct for costs they did not
cause and oscillate together. Whether the specified single-arbiter loop is itself stable under step
loads is an empirical question no specification can settle.

*If the assumption fails*: allocations oscillate visibly — detail popping in and out at a beat —
which is worse than a naive fixed budget. The fix is in the control law, not the architecture, but
finding it late means every paged system has been tuned against an unstable signal.

*Mitigation*: spike the arbiter against synthetic load traces before the paged systems consume it.

### 4 — Virtual geometry's cluster pipeline

**Milestone**: M7. **Owner**: `virtual-geometry`.

Crack-free hierarchical simplification, cluster grouping, GPU hierarchy traversal and the visibility
buffer are each individually hard, and the visibility buffer couples geometry to material resolve —
so a mistake in the geometry representation reaches the material compiler.

*If the assumption fails*: the fallback is conventional LOD chains with GPU-driven culling, which
the roadmap can absorb — but it changes the authoring experience the specifications promise
("artists stop authoring LOD chains") and would be a specification change.

*Mitigation*: prototype hierarchy construction on real assets before the runtime consumes it.

### 5 — The live bridge is fast enough to feel local

**Milestone**: M5. **Owner**: `live-editing`, `editor-rust-application`.

An out-of-process editor is the right decision — a runtime crash costs a restart, not a session, and
editing on a console becomes the same code path as editing locally. It is also the expensive one:
every selection, every gizmo drag and every property edit crosses a process boundary.

*If the assumption fails*: the in-process hosting mode becomes the default for local editing and the
out-of-process mode becomes the remote path. The specification already permits both, so this is a
default change rather than an architecture change — but it should be a measured decision, not a
discovery.

*Mitigation*: measure gizmo-drag round-trip latency in the M5 spike, before panels are built.

### 6 — The derivation key model is precise

**Milestone**: M6. **Owner**: `build-and-packaging`.

If keys under-specify their inputs, the cache serves stale artefacts and the failures are
non-reproducible. If they over-specify, the cache never hits and it is decoration. Everything from
M6 onward builds through it.

*Mitigation*: the M6 spike builds the key model against the M5 import pipeline's real derivations,
and proves a cold build and a cache-warm build are byte-identical before anything else in M6 starts.

**M6's outcome, and the half that is still open.** The spike ran and found the failure it was
commissioned to find: `cy::import::import_derivation_key` contributed no compiler, no flags and no
library versions, so two builds of one importer computed the identical key and one was served the
other's artefact with a reported cache hit. `tools/build/` was then written so that the failure is
unrepresentable — `derivation_key` refuses a key without a complete `ToolchainFingerprint`, and that
fingerprint is *generated* from `deps/manifest.toml` and `deps/host-tools.toml` at configure time, so
adding a dependency without adding it to the key is impossible rather than discouraged. M6's gate
verified both exit criteria adversarially: 30 node declarations attacking the key's framing produced
26 keys with no collision between different declarations; cold, cache-warm and a second cold build
are byte-identical; a one-asset change ran exactly its three dependents; a rewrite with identical
content ran nothing; and an edit the producer normalises away ran one node where deep input keys
would have run three.

**The risk is not closed, because the fix did not reach the tool that cooks.**
`tools/import/src/importer.cpp` is unchanged, and M6's gate re-demonstrated the defect on the two
binaries it had just built: a `-O2 -g -DCY_DEVELOPMENT` importer and a `-O2 -g -DNDEBUG` importer,
different files, pointed at one cache — `1 hit, 0 miss`. `asset-import-pipeline` requires "one cache
covering all derived data" and there are still two caches and two key functions. Until they are one,
**a shared import cache can serve an artefact built by a different compiler and report success**, and
M7's material and virtual-geometry cooks are the point at which that stops being theoretical. It is
task 1.1 of `implement-m7-fidelity`.

### 7 — Reflection generation is fast and reproducible

**Milestone**: M1. **Owner**: `core-type-system`.

Every capability after M1 pays the generator's cost on every build. Slow or non-reproducible
generation is a tax on the whole project that compounds silently.

*Mitigation*: measure incremental regeneration in the M1 spike against a synthetic type set an order
of magnitude larger than M1's own.

### 8 — Cook-time flattening produces chunk-shaped blocks

**Milestone**: M2. **Owner**: `serialization-and-prefabs`.

The whole storage argument rests on it: a designer authors hierarchies, the runtime gets flat data,
and activating a streaming cell is a bulk copy. If flattening needs runtime fixup, activation stops
being a bulk copy and the prefab graph becomes load-bearing at runtime.

*Mitigation*: the M2 spike cooks a deliberately awkward prefab — nested variants, cross-references,
exposed parameters — and measures the activation path.

### 9 — Four toolchains, one meaning per profile

**Milestone**: M0. **Owner**: `developer-workflow-and-just`.

Low cost, high nuisance. CMake, Cargo, the Slang toolchain and the engine's own tools must agree on
what `debug`, `dev`, `profile` and `release` mean, or the workflow's central promise fails on the
first day.

*Mitigation*: prove one profile end to end before writing the rest of the recipe surface.

### 10 — The ordering itself

**Milestone**: all of them. **Owner**: `delivery-roadmap`.

The most likely error in this roadmap is not that a milestone is too large — it is that a dependency
was missed and a capability is scheduled before something it needs.

*Mitigation*: the specifications, not the roadmap, are authoritative about dependencies. A milestone
that cannot start because a prerequisite is missing is a roadmap defect, fixed by an OpenSpec change
that states what was learned — not by an ad-hoc exception.

### 11 — Region invalidation regenerates exactly what changed, and no more

**Milestone**: M10. **Owner**: `procedural-content-generation`.

Appended after the register was written and kept in numeric order rather than renumbering the nine
entries above it; by expected cost it belongs beside 1 and 2.

A dependency-driven partial regeneration either reproduces the full regeneration of the same seed or
it does not, and the two ways of being wrong are both project-defining: stale content, where the
cache serves a region the edit should have moved, and full-world regeneration, where an edit is
correct because nothing is ever reused. `docs/ROADMAP.md`'s M10 row names it as that milestone's
risk.

*Mitigation*: the M10 spike measures a partial regeneration against a full one, for output and for
generated identity separately, over a graph carrying the three kinds of edge a real one has.

**M10's outcome: the risk closes, and it closes conditionally.**
`~/cyberdyne-spikes/m10-pcg-spike/` measured twenty-four configurations over twelve trials — a
different seed and a different edit each — and **two of them reproduce the full regeneration
exactly, in every trial, for output and identity both**. So caching and spatial invalidation stay in
`procedural-content-generation`'s scope rather than coming out of it. What the spike changes is that
reproduction is now known to need **four properties at once**, each of which the measurement shows is
load-bearing: order-free conflict resolution between regions, invalidation expanded to a fixed point
rather than to a declared radius, an iterative solve run to convergence rather than to a sweep
budget, and a generated identity derived from stable identifiers rather than assigned by traversal
order. Drop any one and the configuration diverges.

**The most useful number in it is the one that nearly passed.** A statically-closed invalidation —
the declared radius-one edge, applied once, which is what an implementation writes first —
reproduced in **7 of 12 trials**. A spike that had run one edit on one seed would have called it
sound more often than not, which is why this one runs twelve and why the blast radius is reported
beside the verdict. The other surprise is where the cost is: the transitive edge everyone worries
about (water crossing regions) invalidates 40 regions to find the 19 it changes, while a long-range
gather whose declared reach is five regions invalidates 179 of 576 to find **one**. Long-range
gathers, not transitive edges, are what make partial regeneration expensive.

`openspec/changes/implement-m10-worlds/design.md` §1 is the measurement in full, with the figure.

---

## Deferred scope

Deferred scope is neither silently dropped nor silently started. Each entry records the seams that
must stay open, the check that proves they are, and the milestone at which it would be reconsidered.
Bringing any of it forward is an OpenSpec change.

| Deferred | Recorded in | Seams held open | Reconsidered |
|---|---|---|---|
| **XR** — headsets, tracking, stereo rendering | [`xr-support`](../../openspec/specs/xr-support/spec.md) | Multi-view rendering, runtime-driven frame timing, late latching — each a check from M3 | After 1.0 |
| **Fluid simulation** | [`vfx-system`](../../openspec/specs/vfx-system/spec.md) | Attachment points in the VFX simulation world and data interfaces | After 1.0 |
| **Hair and fibre rendering** | `vfx-system` | Geometry classification already distinguishes thin geometry | After 1.0 |
| **Voxel rendering** | `vfx-system` | None required | After 1.0 |
| **Offline simulation import** (Houdini caches) | `vfx-system` | The VFX asset model's data interfaces | After 1.0 |
| **Authority migration** between peers | [`networking-and-replication`](../../openspec/specs/networking-and-replication/spec.md) | The authority model names migration seams | M9 or later, if a design needs it |
| **Distributed build execution** | [`build-and-packaging`](../../openspec/specs/build-and-packaging/spec.md) | The derivation graph is already the unit of distribution | M11 or later |
| **Cloud save backends** | [`save-and-persistence`](../../openspec/specs/save-and-persistence/spec.md) | The storage backend boundary | After 1.0 |
| **Optional audio middleware** (Wwise, FMOD) | [`audio`](../../openspec/specs/audio/spec.md) | The engine-owned audio interfaces are the integration point | On demand |
| **Agent density feeding streaming priority** | [`dependencies.md`](dependencies.md) cycle 3 | None; cutting the edge removes a cycle | M10 or later |
| **D3D12 before Metal** | [`rhi-and-render-graph`](../../openspec/specs/rhi-and-render-graph/spec.md) | The backend capability model | Only through a roadmap change |

**Seam checks are tests, not intentions.** The XR prerequisites are the model: from M3, a render
change that breaks multi-view, runtime-driven timing or late-latching fails a check, and the change
must either preserve the seam or propose closing it explicitly. Deferred scope with no check
protecting it is scope that has been dropped without anyone deciding to.

---

## What is not a risk

Worth stating, because these are the things that usually are:

- **Library choice.** Jolt, miniaudio, HarfBuzz, Slang, Recast and the rest each sit behind an
  engine-owned interface with a retained stub. Replacing one is a bounded change, and the stub
  proves it at every build.
- **Backend portability.** The RHI is Vulkan-shaped by decision, the null backend runs in continuous
  integration from M3, and a Metal seed at M7 exposes Vulkan-specific assumptions while removing
  them is still cheap.
- **Scope discovery.** 73 capabilities and 1,155 requirements are already written down. The usual
  greenfield risk — discovering the work halfway through — has been paid for in advance. What
  remains is ordering and execution, which is what this roadmap is for.
