# `src/servers/render/geometry/` — layer 2

Skinning and blend shapes, GPU resource residency and lifetime, mip streaming, ring-buffered dynamic
geometry, and the diagnostics over all of it.

**Governed by**: `rendering-geometry-and-resources`. It stays at **Working** at M6 and completes at
**M8** — M6 task 10.8, decided at M6's gate on the evidence this module put in code rather than in
a note (see "One requirement is not met" below): the tier moved rather than the requirement being
scoped away. Task 8.4.

Mesh representation, vertex compression, LOD chains and instancing are `../mesh.h` and `../model.h`
and landed at M3. What is here is the remainder of the capability.

## The files

| File | What it holds |
|---|---|
| `skinning.h` | `SkinningDescriptor` and its validation, the double-buffered output's sizing, skinned bounds from bone transforms, and the sparse blend-shape set |
| `resources.h` | `ResourceLedger` (reference counting with deferred release, per-category budgets, eviction candidates), `MipChain`, `GeometryRing`, and `GeometryStatistics` |

## One requirement is not met, and it is reported rather than quietly satisfied

`rendering-geometry-and-resources` requires skinning to read bone matrices from the **GPU pose
world**, which belongs to `animation-and-skinning` at **M8**. There is no pose world at M6 and this
module must not invent a second one — inventing one is exactly the failure that requirement exists to
prevent.

So the seam is named instead of worked around: `PoseSource::GpuPoseWorld` is declared,
`SkinningDescriptor::pose_offset` is the offset into it, and `SkinningDescriptor::validate()`
**refuses** that value with `NotImplemented`, naming the milestone. `test_skinning.cpp` asserts the
refusal, so the gap is a fact in the test log rather than a note in a header.

The capability matrix's own rule — a capability may not reach Complete before its prerequisites reach
Working — is what this is evidence for. **M6 task 10.8** asked the milestone gate to decide whether
the requirement is scoped or the tier is moved. **The gate moved the tier.** Scoping the requirement
away would delete the one sentence that stops a second per-consumer pose upload being invented, which
is the failure it exists to prevent; so `rendering-geometry-and-resources` stays at Working at M6 and
its Complete cell moves to M8, where `animation-and-skinning` reaches Working and the GPU pose world
exists. The reasoning is in `docs/roadmap/capability-matrix.md#where-m6s-tiers-are-thin` and in
`tools/roadmap/milestones/m6.toml` beside `[criterion.expect_tiers]`.

## Four things worth knowing before changing anything here

**Deferred release is the point, and it is one counter.** `release()` never frees; it records the
frame in which the count reached zero, and `retire(frame)` reclaims what the device has finished
with. A caller that never retires leaks visibly in the report, which is a better failure than a
use-after-free that appears once in a thousand frames.

**A budget reports, it does not refuse.** Refusing an allocation mid-frame produces a missing object.
The requirement's answer to pressure is eviction, and `evict_candidates` produces an ordered list for
the shared `residency` policy to choose from — an ordering, not a decision, because this module does
not know an instance's importance.

**The mip tail is a guarantee.** `configure` refuses a tail of zero and `commit_resident` never
evicts into it, so a frame is never missing a texture — only sampling a coarser one.

**The geometry ring refuses rather than wraps.** Wrapping into the next slice is how a frame
overwrites the vertices the device is still reading, and the symptom is geometry that flickers on one
machine and not another.

## No device, no buffer, no upload

Layer 2 may not name one. Everything here is a description a module above turns into GPU work: a
`SkinningDescriptor` says what a compute pass must do and `ResourceLedger` says what memory costs,
and neither holds a handle. That is what lets `tests/` run in every profile on a machine with no GPU.
