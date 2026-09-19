# `src/backends/rhi/` — layer 3

The Rendering Hardware Interface: an explicit, Vulkan-shaped abstraction over a graphics device, a
**null backend** that implements all of it without a GPU, and a **Vulkan backend** behind the same
interface.

**Governed by**: `rhi-and-render-graph`. Landed at M3, section 2.1 and 2.3 of that milestone's tasks.

## The three targets, and the order they were written in

| Target | Built when | What it is |
|---|---|---|
| `cy::rhi` | always | the interface, the hard limits, the capability model, the backend registry, and the access table |
| `cy::rhi-null` | always | the whole interface without a device: real handles, real validation, a comparable command log |
| `cy::rhi-vulkan` | `CY_RENDERER_VULKAN` | Vulkan, over volk and VMA |

**The null backend was written before the Vulkan one** (design.md §1). With no Vulkan to lean on, the
interface above it had to be an interface; written afterwards it would have been a set of empty
functions shaped by decisions Vulkan had already made, and it would have stopped being a reference
for what the RHI requires. It is also what lets a rendering test run in continuous integration on a
machine with no GPU — which is most machines.

## The two rules this directory exists to keep

**No barrier in the recording API.** `cy::rhi::CommandBuffer` — what a render pass is handed — has no
barrier method, no layout transition and no queue-ownership transfer. Those live on
`cy::rhi::BarrierRecorder`, reachable only with a passkey that `cy::rendering::GraphExecutor` alone
can construct, and `tools/layercheck/layercheck.py --check barriers` fails the build if a barrier
symbol appears outside `src/backends/rhi/` and `src/rendering/graph/`. See `include/cy/backends/rhi/barrier.h`.

**No Vulkan type above this layer.** The engine's synchronisation vocabulary — `Stage`,
`AccessFlags`, `ImageUse` — is engine-owned and Vulkan-shaped, in `include/cy/backends/rhi/types.h`.
Exactly one file translates it (`vulkan/src/vulkan_translate.cpp`), and
`tools/layercheck/layercheck.py --check gpuapi` fails the build on a Vulkan, Slang or SPIR-V header
outside `src/backends/`. That is the same rule that keeps SDL inside `platform/`, for the same
reason: Metal is a directory rather than a rewrite.

**And SHAPED BY VULKAN IS NOT SPELT IN VULKAN — four places where it had been, closed at M11.d.**
The Metal seed (`src/backends/rhi-metal/`) exists to name every place `cy::rhi` says something a
Metal device cannot be asked, *while that is still cheap to change*. M11.d section 1 spent that
finding, on Vulkan and the null backend, before either new backend exists:

| Was | Is | Why |
|---|---|---|
| `MemoryRequirements::memory_type_bits` (`u32`, a Vulkan memory-type mask the graph intersected) | `MemoryPoolClass`, an opaque token the graph MEETS and tests for empty | an `MTLHeap` picks one storage mode and has no bitmask. A MEET rather than the equality the seed proposed, because an RTX 5060 answers 0x03 for transient images and 0x1F for transient buffers and an equality would split the heap in two |
| `ImageLayout` (nine enumerators named after `VkImageLayout`'s, on every `ImageBarrier`) | `ImageUse`, engine vocabulary; `VkImageLayout` lives only in `vulkan_translate.cpp` | Metal has no image layouts. The seed said to derive the layout in the backend from the access masks; that is **not implementable** — a barrier's `src_access` carries only the write access, so it is not the resource's current state |
| `Device::queue_family() -> u32` and `kQueueFamilyIgnored` | `DeviceCapabilities::needs_queue_ownership_transfer()` and an opaque `queue_ownership_domain()`; barriers carry `QueueKind`s and a flag | `MTLCommandQueue` has no family index and no ownership transfer at all |
| `save_pipeline_cache(Span<u8>)` | `save_pipeline_cache(const char* path)` | `MTLBinaryArchive` serialises to a URL. **Nothing in the tree calls either**, which is the finding beside the signature |

Plus two additions and one non-change: `ShaderModuleDescription::native` with
`DeviceCapabilities::native_shader_format()` (SPIR-V stays the interchange form; zero call sites
moved), `Capability::ParallelPassRecording` (the engine records one secondary per pass *across*
passes, which `MTLParallelRenderCommandEncoder` does not do *within* one), and a multi-stage
`PushConstantRange`, which is genuinely fine and is recorded as such rather than skipped.
`src/backends/rhi-metal/README.md` carries the argument for each at length, and `metal_gaps()`
carries where each stands as data.

## Where the interesting decisions are written down

- `include/cy/backends/rhi/access.h` — the closed enum of access intents. A pass's entire
  synchronisation vocabulary; every barrier in every frame is two rows of the table beside it.
- `include/cy/backends/rhi/barrier.h` — the invariant, expressed as a passkey.
- `include/cy/backends/rhi/device.h` — resources, frames in flight, submission, the transient pool
  the render graph places into, and the GPU memory report that feeds the engine's own budget tree.
- `null/include/cy/backends/rhi/null/null_device.h` — why the null backend is not a set of empty
  functions, and what its command log is for.
- `vulkan/src/vulkan_instance.cpp` — queue selection by capability, the 1.3 baseline, and why
  synchronisation validation has to be asked for explicitly.

## What does not belong here

A render graph (`src/rendering/graph/`, layer 4 — it consumes this and must never be consumable by
it). A window (`platform/` — `create_swapchain` is handed a surface `DisplayServer` produced and does
not ask how). A shader compiler (`src/backends/shader/` — this module consumes SPIR-V as bytes).
