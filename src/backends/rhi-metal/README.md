# `src/backends/rhi-metal/` — layer 3

The Metal seed. **`src/device.mm` has never been compiled, on this machine or anywhere else** —
there is no Apple toolchain here — and everything else in this module is built and tested by
`just test-all` on Linux.

**Governed by**: `rhi-and-render-graph`. `delivery-roadmap` seeds Metal at **M7** and delivers it at
**M11**. Task 10.5.

## What the seed is for, and therefore what shape it has

M7's task says it outright: the seed exists "to expose Vulkan-specific assumptions in
`rhi-and-render-graph` **while they are still cheap**… That finding is the point of the seed, not the
backend."

So the module is shaped around the finding rather than around the device. `mapping.h`, `mapping.cpp`
and `backend.cpp` name no Metal type and compile everywhere, which means the eight gaps below are
checked by the ordinary test run on the machine the work is being done on. A finding that can only
be read on hardware nobody in this project has is a finding nobody reads.

    just test-unit -R unit.rhi_metal_seed

    formats: 30 map directly, 2 have no Metal equivalent and are substituted explicitly
    gaps: 8 total, 3 where Metal has no equivalent at all, 2 with no workaround

## The eight gaps, in the order of what each costs to fix later

`metal_gaps()` returns these as data, so a diagnostic, a test and this table cannot drift apart.

| # | The interface says | Metal has | Fix now | Workaround? |
|---|---|---|---|---|
| 1 | `ShaderModuleDescription::spirv` is `Span<const u32>`, and there is no second field | MSL source, or a `.metallib` | one optional `Span<const u8> native` and a `native_shader_format()` capability | yes, at a cost |
| 2 | `reserve_transient_memory(bytes, memory_type_bits)` | **nothing**: a `MTLHeap` picks one storage mode and there is no bitmask of types | an opaque `MemoryPoolClass` the graph only compares for equality | **no** |
| 3 | `ImageBarrier::old_layout` / `new_layout` | **nothing**: a tracked heap needs no transition, an untracked one a `MTLFence` | derive the layout inside the Vulkan backend from the access masks `access.h` already carries | yes |
| 4 | `queue_family(QueueKind) -> u32`, `kQueueFamilyIgnored` | **nothing**: `MTLCommandQueue` has no family and no ownership transfer | `bool needs_queue_ownership_transfer()` on the capabilities | yes |
| 5 | `execute_secondary(primary, …)` — a secondary recorded before its pass instance exists | `MTLParallelRenderCommandEncoder`, whose sub-encoders exist only inside a live encoder | state "the pass is begun before its secondaries are recorded" as a precondition | **no** |
| 6 | `save_pipeline_cache(Span<u8> out)` | `MTLBinaryArchive`, serialised to a URL | take a path, or an opaque backend-defined token | yes |
| 7 | `Format::D24UnormS8Uint` | `MTLPixelFormatDepth24Unorm_Stencil8`, unsupported on every Apple GPU | a per-format support query, so the *engine* picks the substitute | yes |
| 8 | `PushConstantRange::offset` with a multi-stage mask | `setBytes:` binds a whole block to one stage's table | **nothing** — it is genuinely fine, and it is listed so the next reader does not re-derive that | yes |

**Gap 3 is the one worth arguing about.** `ImageLayout` is a Vulkan object sitting in an interface
that is otherwise engine-owned — `types.h` says as much about the rest of its vocabulary — and the
engine already has everything it needs without it, because `access.h` carries the access masks the
layouts were derived *from*. A Metal backend drops every `old_layout` and `new_layout` on the floor.

**Gap 2 has no workaround and no equivalent**, which is the expensive combination: `memory_type_bits`
is intersected across every transient in a frame to prove one pool is legal for all of them, and a
Metal backend can only answer `~0u` and hope nobody looked.

### And three things that map cleanly

Worth as much to the M11 reader as the gaps, because each is a place not to spend an afternoon:

* **Timeline semaphores.** `timeline_value` / `wait_timeline` is `MTLSharedEvent` almost exactly.
* **Specialization constants.** `SpecializationConstant` is `MTLFunctionConstantValues`.
* **Reversed-Z.** `Viewport::min_depth`/`max_depth` stay [0, 1] and the *projection* inverts, so
  Metal's [0, 1] clip depth needs no adjustment. A design that had inverted the viewport instead
  would have needed one here.

## How the transcribed enum values are kept honest

`mapping.cpp` carries every `MTLPixelFormat` as a plain integer, because it is compiled on machines
with no Metal. That is a transcription, and transcriptions are wrong. `src/device.mm` carries one
`static_assert` per row against the real enumerator — so **on Linux the table is unverified and this
file says so, and on the day somebody builds it on a Mac a wrong number is a compile error naming
the row rather than a wrong picture.**

## What is built where

| | Linux / Windows | Apple |
|---|---|---|
| `mapping.{h,cpp}`, `backend.{h,cpp}` | built, tested | built, tested |
| `src/device.mm` | not built | built when `CY_RENDERER_METAL` is on, which is its **default there** |

`CY_RENDERER_METAL`'s default column in `cmake/features.cmake` is `APPLE`, a keyword added for this
module: on where the platform can build it, off where it cannot. The alternative — leaving a seeded
backend OFF everywhere — makes "a delivered capability is on by default" unsatisfiable for every
platform backend this engine will ever add.

`register_metal_backend()` on a build without Metal **refuses** with `ErrorCode::Unsupported` rather
than registering a factory that would fail at the first call. A registration that exists and cannot
work turns "asked for metal, ran null" into a runtime surprise instead of a configuration answer.

## What `src/device.mm` is and is not

It implements the four things the M3 golden scene needs from Metal directly — a device, a command
queue, a `CAMetalLayer`, and a render pass with a load and store action — as free functions with no
inheritance, and stops.

`cy::rhi::Device` has more than eighty pure virtual members. A skeleton overriding all of them that
has never been compiled is eighty signatures that are probably slightly wrong, and it would look
like progress while being worth less than nothing to whoever picks this up at M11 — by which time
gaps 1 to 5 should have been closed, which will change several of those signatures anyway.

**Nothing in that file has been compiled or run. Treat every line of it as a proposal.**

## One change outside this directory

`tools/layercheck/layercheck.py`'s `GPU_API_DIRECTORIES` gained `Metal` and `QuartzCore`. The rule
is the same one it already applied to Vulkan and Slang and the reason is the same: a
`#import <Metal/Metal.h>` above `src/backends/` is exactly as wrong as a
`#include <vulkan/vulkan.h>` there, and a rule that named only Vulkan would have accepted it.
