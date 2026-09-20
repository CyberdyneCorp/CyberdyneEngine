# `src/backends/rhi-metal/` — layer 3

The native Metal backend, under active implementation for M11.d.5. **`src/device.mm` first compiled
on an Apple M3 Pro with AppleClang 21 and the macOS 27 SDK on 2026-09-19**; every transcribed Metal
enumerator below passed its compile-time assertion. The platform-neutral mapping and gap records
continue to build and run on every host through `just test-all`.

**Governed by**: `rhi-and-render-graph`. `delivery-roadmap` seeds Metal at **M7** and delivers it at
**M11**. Task 10.5.

## What the seed is for, and therefore what shape it has

M7's task says it outright: the seed exists "to expose Vulkan-specific assumptions in
`rhi-and-render-graph` **while they are still cheap**… That finding is the point of the seed, not the
backend."

**M11.d section 1 spent that finding.** All eight gaps were settled as interface changes, on Vulkan
and the null backend, before either new backend exists — and three of the seed's own proposed
remedies turned out to be wrong when somebody finally applied them, which is the strongest argument
this module could have made for existing. The table below carries what each became.

So the module is shaped around the finding rather than around the device. `mapping.h`, `mapping.cpp`
and `backend.cpp` name no Metal type and compile everywhere, which means the eight gaps below are
checked by the ordinary test run on the machine the work is being done on. A finding that can only
be read on hardware nobody in this project has is a finding nobody reads.

    just test-unit -R unit.rhi_metal_seed

    formats: 30 map directly, 2 have no Metal equivalent and are substituted explicitly
    gaps: 8 total, 0 still open, 3 where Metal has no equivalent at all, 0 open with no workaround

## The eight gaps, and what M11.d did with each

`metal_gaps()` returns these as data — including where each stands — so a diagnostic, a test and
this table cannot drift apart. **`metal_open_gap_count()` is the number that shrinks**, and
`unit.rhi_metal_seed` prints it: eight open at M7, **none open after M11.d section 1**. The rows
are never deleted when they close, because the finding, the remedy that was argued for and what was
actually done are one row a reviewer reads together.

M11.d settled all eight **on Vulkan and the null backend, before either new backend exists** —
which is the whole reason this seed was written four milestones early. `delivery-roadmap` moved the
backends themselves to M11.d.5; the interface did not move, because changing
`reserve_transient_memory`'s contract after two more backends are written is a migration across
every pass in the engine.

| # | The interface said | Metal has | Status | What it is now |
|---|---|---|---|---|
| 1 | `ShaderModuleDescription::spirv` is `Span<const u32>`, and there is no second field | MSL source, or a `.metallib` | **closed** | `Span<const u8> native` plus a `ShaderFormat native_format` beside the SPIR-V, and `DeviceCapabilities::native_shader_format()`. Additive: **zero** call sites moved |
| 2 | `reserve_transient_memory(bytes, memory_type_bits)` | **nothing**: a `MTLHeap` picks one storage mode and there is no bitmask of types | **closed** | `MemoryPoolClass`, an opaque token the graph **meets** and tests for empty. The seed said *equality* and that was measured wrong — see below |
| 3 | `ImageBarrier::old_layout` / `new_layout` | **nothing**: a tracked heap needs no transition, an untracked one a `MTLFence` | **closed** | `ImageUse` — engine vocabulary — and `VkImageLayout` only in `vulkan_translate.cpp`. The seed's remedy is **not implementable**; see below |
| 4 | `queue_family(QueueKind) -> u32`, `kQueueFamilyIgnored` | **nothing**: `MTLCommandQueue` has no family and no ownership transfer | **closed** | `needs_queue_ownership_transfer()` and an opaque `queue_ownership_domain()`; barriers carry `QueueKind`s and an `ownership_transfer` flag |
| 5 | `execute_secondary(primary, …)` — a secondary recorded before its pass instance exists | `MTLParallelRenderCommandEncoder`, whose sub-encoders exist only inside a live encoder | **closed** | `Capability::ParallelPassRecording`, one term in `executor.cpp`. The seed proposed a precondition; it does not address the mismatch — see below |
| 6 | `save_pipeline_cache(Span<u8> out)` | `MTLBinaryArchive`, serialised to a URL | **closed** | both calls take a path; an absent file is a cold start. **And nothing in the tree calls either** |
| 7 | `Format::D24UnormS8Uint` | `MTLPixelFormatDepth24Unorm_Stencil8`, unsupported on every Apple GPU | **closed** | never an interface change: the query existed and had **no consumer**. `select_depth_stencil_format()` is the engine picking, and `validate_texture` refuses rather than letting a backend substitute |
| 8 | `PushConstantRange::offset` with a multi-stage mask | `setBytes:` binds a whole block to one stage's table | **no change needed** | measured again and still nothing to do. Recorded, not skipped: that is the difference between a gap somebody closed and a gap somebody forgot |

### Three of the seed's own remedies were wrong, and that is the most valuable thing on this list

A remedy nobody applied is a guess. M11.d applied all eight.

**Gap 2 — the seed said "equality" and the word matters.** The proposal was an opaque
`MemoryPoolClass` *"the graph only compares for equality"*. Measured on this project's own devices:
an NVIDIA RTX 5060 answers `0x03` for transient images and `0x1F` for transient buffers — **they
differ** — while an Intel UHD 770 answers `0x07` for everything and llvmpipe `0x01`. An equality
would have refused to place images and buffers in one pool on the NVIDIA device and **split the
transient heap in two**, losing exactly the aliasing `heap_bytes` against `naive_bytes` exists to
report. A **meet** (`a & b`, empty when zero) keeps the proof, loses the Vulkan spelling, needs no
device — so `compile()`'s "the derivation touches no device" invariant and `plan_hash`'s determinism
both survive — and costs nothing extra. D3D12 wants the same shape: on Resource Heap Tier 1 a heap
holds buffers *or* textures and never a mix, which is the same partition Vulkan spells as a bitmask.

**Gap 3 — the seed's remedy is not implementable, and that is the finding.** It said the engine
"already has the information without them" because `access.h` carries the masks the layouts were
derived from, so a backend could derive the layout itself. It cannot: `compile.cpp` deliberately
puts only the **write** access into a barrier's `src_access` — a write-after-read needs an execution
dependency and not a memory one, and naming the read's access would ask the implementation to flush
caches nothing wrote — so a barrier's source mask **is not the resource's current state**. An image
last read as sampled and next written as a colour attachment produces a barrier whose `src_access`
is a colour-attachment write from two passes ago; a backend deriving "what it was" from that would
transition from the wrong state. And `Access::Present` carries no access bits and no stage at all,
so a derivation could not answer for the one intent whose entire content is the state it leaves the
image in.

What was done instead is the half that *is* true: the **vocabulary** was Vulkan's and did not have
to be. `ImageLayout` became `ImageUse` — `Storage`, `SampledRead`, `Presentable` rather than
`General`, `ShaderReadOnly`, `Present` — and the mapping to `VkImageLayout` moved entirely inside
`vulkan/src/vulkan_translate.cpp`. A Metal backend maps `ImageUse` to **nothing**, which is a
mapping it writes rather than a field it silently drops. The engine keeps the value because it
genuinely needs it: two **reads** of one image — sampled and storage — need a barrier between them
even though neither contributes a source access mask, and that difference is visible only here.

**Gap 5 — the proposed precondition does not address the mismatch.** The seed proposed stating
*"the pass is begun before its secondaries are recorded"* as an interface precondition. Read against
the tree, that is not the shape of the problem:

* `executor.cpp` records **every** secondary for a submit, one per pass, on job workers, **before
  the primary loop reaches any of them**;
* `frame_recorder.cpp`'s pass callback itself calls `begin_rendering`/`end_rendering`, so **each
  secondary contains a whole render pass**;
* the barriers are recorded into the **primary**, between passes.

`MTLParallelRenderCommandEncoder` is parallelism **within** one render pass. This engine's is
parallelism **across** passes. They are different axes, and no ordering precondition converts one
into the other; Metal's actual equivalent is one `MTLCommandBuffer` per pass with `-enqueue`
establishing order, which is a different allocation strategy rather than a reordering. So the honest
fix is a **capability** — `Capability::ParallelPassRecording`, one term in `executor.cpp` — and a
device that answers false records sequentially and produces the **identical** command stream.
`ExecuteOptions::parallel_recording` already defaults to false, so this is opt-in either way.

### And two gaps were not interface work at all

**Gaps 6 and 7 are M8.c's firewall finding in a third module.** `capabilities.h` has defined
`FormatFeature` and `DeviceCapabilities::format_features()` since M3, and both backends populate it
for **every** format — and **nothing outside `src/backends/rhi/` called it**. Every depth path in
the engine defaults to `D32Sfloat`, which Metal supports, so gap 7 was inert. The pipeline cache is
the same shape and worse: two interface methods, four implementations, **zero callers**, and a
requirement — *"the cache is persisted across runs, so a warm start compiles nothing"* — that is
therefore unimplemented above the RHI. **Noticing that is worth more than the signature change**, and
changing the signature does not implement it. Gap 7 got its consumer at M11.d; gap 6 did not, and
that is recorded rather than quietly claimed.

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
| `src/device.mm` | not built | built when `CY_RENDERER_METAL` is on, which is its **default there**; first verified on an M3 Pro with the macOS 27 SDK |

`CY_RENDERER_METAL`'s default column in `cmake/features.cmake` is `APPLE`, a keyword added for this
module: on where the platform can build it, off where it cannot. The alternative — leaving a seeded
backend OFF everywhere — makes "a delivered capability is on by default" unsatisfiable for every
platform backend this engine will ever add.

`register_metal_backend()` on a build without Metal **refuses** with `ErrorCode::Unsupported` rather
than registering a factory that would fail at the first call. A registration that exists and cannot
work turns "asked for metal, ran null" into a runtime surprise instead of a configuration answer.

## What `src/device.mm` contains today

The seed's four free functions remain as transcription evidence, and a native `cy::rhi::Device`
now owns the real Metal path being implemented for M11.d.5. The hardware suite on the M3 Pro has
exercised:

* private and shared buffers, textures and texture views;
* Apple-family memoryless attachments;
* explicit-placement `MTLHeap` aliasing between a texture and a buffer at the same offset;
* sampler states, MSL compilation, graphics and compute pipeline creation, and
  `MTLFunctionConstantValues`;
* Tier 2 argument buffers, including the device-owned 16,384-entry global texture table, its
  single sampler, descriptor-set pipeline layouts and a compute shader that samples through the
  table and returns the observed colour to shared memory;
* render, blit and readback command encoding, including a pixel read back from a native draw; and
* `MTLSharedEvent` queue timelines, fences and binary submission dependencies;
* `CAMetalLayer` acquisition and presentation from a surface supplied through the platform seam,
  with the acquired drawable cleared, read back and presented headlessly;
* timestamp counter sampling on Apple GPUs through stage-boundary blit passes; and
* `MTLBinaryArchive` persistence with verified warm hits for graphics and compute pipelines.

The first-light render golden also runs through this backend using committed MSL artefacts from the
same Slang module as Vulkan's SPIR-V. Descriptor sets are emitted as `ParameterBlock` argument
buffers, vertex streams use native slots 16–30, and the push-constant buffer follows the pipeline
layout's set buffers. On the Apple M3 Pro the Metal capture matches the committed Vulkan reference
with zero differing texels; `docs/design/images/m11d5-three-backends-metal.manifest` names the
backend and device beside the captured PNG.

The shader probe also found a boundary the earlier M11.c check did not exercise. Slang 2026.9.2
emits the existing runtime-sized global texture array as a direct MSL entry-point parameter; Apple's
compiler rejects that output because the flexible texture array is neither the last struct member
nor valid in that address space. A fixed-capacity `ParameterBlock<T>` emits one argument buffer and
compiles through both `metal` and `metallib`. The fixture in `tests/fixtures/` preserves that result;
the runtime test uses the same ABI at the engine's full 16,384-slot capacity.

The native conformance suite is registered as `unit.rhi_metal`; the driver compiler, GPU execution
and presentation cases remain in `integration.rhi_metal_shader` and
`integration.rhi_metal_surface` so their unavoidable driver waits are charged to the integration
budget. The M11.d.5 criterion runs all three on Apple hosts.

## One change outside this directory

`tools/layercheck/layercheck.py`'s `GPU_API_DIRECTORIES` gained `Metal` and `QuartzCore`. The rule
is the same one it already applied to Vulkan and Slang and the reason is the same: a
`#import <Metal/Metal.h>` above `src/backends/` is exactly as wrong as a
`#include <vulkan/vulkan.h>` there, and a rule that named only Vulkan would have accepted it.
