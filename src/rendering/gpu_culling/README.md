# `src/rendering/gpu_culling/` — the culling dispatch

Layer 4. `cy::rendering-gpu-culling`. M7 task 5.1, capability `rendering-culling-and-lod` reaching
**Complete**.

The compute pass that `cpu_reference_cull` was written to be checked against, and the first module
in this tree to create a compute pipeline.

## What M6 left, and what this is

M6's closing gate recorded it in one sentence: `cy::servers-render-culling` was linked by nothing but
its own test binaries. That module holds the records a dispatch reads and writes, plus a CPU
implementation of exactly the algorithm the shader runs — written that way deliberately, so that the
shader landing here is checked against a reference rather than against a screenshot.

This module is the other half. It owns a device, two compute pipelines and the buffers; it runs the
dispatch; and `tests/test_gpu_cull_pass.cpp` compares its output against the reference's **by
comparing buffers**.

| File | What it holds |
|---|---|
| `cull_pass.h` | `GpuCullPass`: create, upload, declare into a render graph, read back |
| `shaders/gpu_cull.slang` | `cull_instances` and `compact_draws`, and the two slangc invocations that regenerate the header |
| `src/gpu_cull_spirv.h` | the compiled SPIR-V, checked in. Generated — `shaders/embed_spirv.py` writes it |

## Four things worth knowing before changing anything here

**Two entry points, and the second one exists for the test.** A one-pass cull appends survivors with
an atomic counter and the order they land in is whatever order the hardware scheduled the groups in.
That is fine for a frame and useless as an expected value: the reference emits in **ascending slot
order** and says so. `compact_draws` is one workgroup walking the verdict array in ascending order
with a shared-memory prefix scan, so the two arrays are equal element for element — and
`first_instance`, which is the draw's own index, is a number rather than a lottery.

**Every expression in the shader is in the reference's order.** `screen_coverage` is
`(radius * inverse_tan) / depth` and not `radius * (inverse_tan / depth)`; the frustum test is
`!(distance < -radius)` and not `distance >= -radius`, because the two differ for a NaN and the
reference keeps the instance. A "simplification" in the shader is a test failure in the suite.

**What is compared exactly, and what is not.** Every integer field — index counts, materials, levels,
slots, `first_instance`, all sixteen counter words, the LOD hysteresis state — is compared for exact
equality and every one of them holds. The four float fields of `GpuDrawPayload` are compared to a
relative tolerance of 1e-6, because `length()`, `dot()` and `exp2()` may differ in the last place
between a C++ standard library and a SPIR-V driver. The measured worst relative difference on an
RTX 5060 is **4.77e-07**, and the suite prints it so a tolerance quietly absorbing a real divergence
shows up as a number that moved.

**An occlusion view is refused, not ignored.** `hzb.h` is a CPU model of a depth pyramid and no
pyramid exists on a device yet. `upload()` fails naming `kGpuCullOcclusion` rather than dispatching,
because a dispatch that ignored the flag would report "nothing was occluded" and be
indistinguishable from a working occlusion cull over an empty pyramid.

## Why this is a module and not part of `src/rendering/culling/`

That module's README states, as a property of its dependency list rather than as a rule, that it
names no device, no graph and no shader — which is what lets every case in its suite run headless on
a machine with no GPU. This one names all three. What `src/rendering/culling/` gained at M7 is
`gpu_bridge.h`: the seam that publishes the dispatch's input from the spatial index and turns its
output back into the `CullResults` `cy::rendering-forward` consumes.

## The defect this module found in the RHI

`VulkanCommandBuffer::bind_descriptor_sets` chose its bind point from the **queue** — compute if the
buffer was on `AsyncCompute`, graphics otherwise. A compute pass recorded on the graphics queue is
the ordinary case, not an exotic one, so every dispatch here bound its descriptor set to the graphics
bind point and the pipeline saw no set at all: `VUID-vkCmdDispatch-None-08600`, on the first
dispatch. It survived M3 to M6 because nothing in this tree had ever created a compute pipeline. The
bind point is now the last bound pipeline's; `src/backends/rhi/vulkan/src/vulkan_device.h` carries
the note, and this suite is the regression test.

## Tests

    just test-render -R render.gpu_culling

`-R`, not a bare name. `just test-render <word>` appends the word to ctest's argument list, where it
is not a filter at all: the run is green and it ran the whole render label. Every command in this
file was executed as written before it was written down, which is the rule M6's gate exists to
enforce.

Needs a Vulkan device; skips loudly, naming the backend that was selected instead, on a machine
without one. Not gated by any option beyond `CY_RENDERER_VULKAN`.
