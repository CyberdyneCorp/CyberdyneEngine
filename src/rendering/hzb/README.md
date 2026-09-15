# `src/rendering/hzb/` — the hierarchical depth buffer, on the device

Layer 4. `cy::rendering-hzb`. M11.c task 4.1, capability `rendering-culling-and-lod` reaching
**Complete** and `virtual-geometry`'s cluster-granular occlusion reaching an implementation.

## One piece of work, two rows, and why it is not in either of them

M11.c's design named this before it was built: five rows converge on two pieces of device work, and
this is the first of them.

| Row | What it was waiting for |
|---|---|
| `rendering-culling-and-lod` | `GpuCullPass::upload` refused `kGpuCullOcclusion` by name — *"no hierarchical depth buffer exists on the device yet; a dispatch that ignored the flag would report 'nothing was occluded' and be indistinguishable from a working occlusion cull"* |
| `virtual-geometry` | Cluster-granular occlusion, whose seam was already cut: the traversal takes the cull tests in the order the requirement fixes, and `TraversalStatistics::nodes_pruned_by_occlusion` read zero |

Building it inside either module would have put the other's dependency somewhere it has no reason
to be. Building it twice would let **one piece of work be recorded as two satisfied requirements**,
which is the failure mode M11.c's specification delta names in as many words:

> One hierarchical depth buffer SHALL serve both this capability's occlusion culling and
> `virtual-geometry`'s cluster-granular occlusion; two implementations of the same structure would
> let one piece of work be recorded as two satisfied requirements.

So it is a module, both consumers link it, and **"one buffer, two consumers" is a link-graph fact**
rather than a sentence — on the C++ side through `cy::rendering-hzb`, and on the shader side through
`shaders/hzb_sample.slang`, which `gpu_cull.slang` and `vg_traversal.slang` both `#include`.

## The CPU model is the expected value

`cy::render::culling::Hzb` — `src/servers/render/culling/include/cy/servers/render/culling/hzb.h` —
has been a tested model of this structure since M6. It was written to be a model: layer 2, no
texture, no device, `level()` handing out storage a copy writes into so that *"a headless test fills
them itself"*.

That is exactly what makes this module checkable. `hzb_reduce.slang` is `Hzb::reduce()` transcribed
expression by expression, including the fold that takes an odd dimension's extra row and column into
the same parent texel; `hzb_sample.slang` is `project_sphere`, `hzb_level_for` and `Hzb::occludes`
transcribed the same way. `HzbPass::read_back()` fills a model object from the device pyramid, so a
test compares **texels** rather than counters.

**A model passing its own tests is not a pass.** That sentence is the requirement this rung adds,
and it is why `read_back` exists at all: without it, "occlusion culling works" would be a claim about
`unit.rendering-culling`, which never touched a device.

## The pyramid is a buffer

`width * height + ...` floats, levels concatenated coarsest last, one `StructuredBuffer<float>`
binding, no sampler, no image layout, no per-mip view.

That is this engine's shape rather than a shortcut: `vg_visbuffer.slang` already settles depth and
payload in one 64-bit atomic over an `RWStructuredBuffer`, and the compute rasteriser is the path
virtual geometry ships. Level 0 is seeded by a **copy** from whatever buffer the depth was written
into; levels above it by one compute dispatch each, with the barrier between two of them derived by
the render graph, because nothing else in this engine may emit one.

**What it costs, recorded rather than discovered.** A texture pyramid would sample with hardware
filtering and would be laid out for two-dimensional locality. This one is read with the same four
explicit loads `Hzb::occludes` makes, at a level chosen so the footprint spans at most a 2x2 — so
the **answer** is identical and the **cache behaviour** is not. Moving to an image pyramid is a
change to this module and to two `#include`s; it changes no interface and no answer.

## What is here

| File | What it holds |
|---|---|
| `hzb_pass.h` | `HzbPass`: create, declare the chain into a graph, the parameters a consumer pushes, read back into the CPU model |
| `shaders/hzb_sample.slang` | The **test**, included by every consumer. `project_sphere`, `hzb_level_for`, `Hzb::occludes` |
| `shaders/hzb_reduce.slang` | The reduction, one dispatch per level, and the slangc invocation that regenerates the header |
| `src/hzb_spirv.h` | The compiled SPIR-V, checked in. Generated — `shaders/embed_spirv.py` writes it |

## Two things worth knowing before changing anything here

**The pyramid holds the FURTHEST depth of each footprint, which under reversed Z is the SMALLEST
value.** An instance is occluded when its nearest depth is further than the furthest thing already
drawn over its whole footprint. Getting this inequality backwards produces a renderer that culls
what is visible and draws what is not, and it looks like a broken pyramid rather than a flipped
comparison. `kReversedZ` in `hzb.h` asserts the convention; this module obeys it.

**False occlusion is a correctness bug, not a quality one.** `HzbParams::enabled` is zero until
something has built the pyramid and called `mark_valid()`, and a consumer with no pyramid attached
sends zero — so every test answers "not occluded". A camera cut calls `invalidate()`. Every case
where the answer is *not known* has to read as *draw it*, and there are three of them: the pyramid
is invalid, the rectangle is invalid, the rectangle leaves the buffer.

## Tests

The device pass is compared against the CPU model in `integration.rendering_culling`
(`tests/integration/test_rendering_culling.cpp`), which is where the comparison belongs: it is a
claim about a seam between this module, `cy::servers-render-culling` and `cy::rendering-gpu-culling`,
and no one of the three owns it.

    CY_BUILD_DIR=build/<label> just build-engine --profile dev
    build/<label>/cy_test_integration_rendering_culling

Needs a Vulkan device; skips loudly, naming the backend that was selected instead, on a machine
without one.

## Regenerating the shader

    cd shaders
    slangc hzb_reduce.slang -target spirv -profile spirv_1_5 \
           -entry hzb_reduce -stage compute -o hzb_reduce.spv
    python3 embed_spirv.py ../src/hzb_spirv.h kHzbReduceSpirv=hzb_reduce.spv
    just quality-format

`hzb_sample.slang` has no entry point and compiles to nothing on its own; it is included by its
consumers, whose own invocations carry `-I ../../hzb/shaders`.
