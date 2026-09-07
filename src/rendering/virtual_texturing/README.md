# `src/rendering/virtual_texturing/` — CyberTexture on the device

Layer 4. `cy::rendering-virtual-texturing`. M7 tasks 4.1, 4.2 and 4.3; capabilities
`virtual-texturing` and `residency` reaching **Complete**.

`src/servers/render/virtual_texturing/README.md` named this module in advance and said what it owes:
"The Vulkan-side upload, the page table image, the sampling shaders and the analytic derivative
reconstruction under a visibility buffer are `src/rendering/` work and arrive with M7."

| File | What it holds |
|---|---|
| `frame.h` | `VirtualTextureFrame`: the feedback dispatch, the GPU resolve, the page-table upload and the sampling dispatch |
| `frame_residency.h` | `FrameResidency`: two subsystems registered against one residency server, fed by one frame's two dispatches |
| `shaders/vt_frame.slang` | `record_feedback`, `resolve_feedback`, `sample_pages`, and the three slangc invocations |
| `src/vt_frame_spirv.h` | the compiled SPIR-V, checked in. Generated — `shaders/embed_spirv.py` writes it |

## The three claims, and how each is measured

### A feedback buffer written by a shader, resolved without a per-pixel stream reaching the CPU

The feedback buffer is **a count per page, not a list of pixels**. That single decision is what makes
"a per-pixel request stream SHALL NOT reach the CPU" a structural fact rather than a promise about
buffer sizes: `record_feedback` runs one thread per pixel and does one `InterlockedAdd` into a word
indexed by the page, so a million pixels asking for one page is one word holding a million.
Deduplication is not a pass; it is the shape of the buffer. `resolve_feedback` then compacts the
non-zero words into `(address, samples)` pairs, and the compact list is the only thing the CPU maps.

Measured, over a 256x256 grid at density 1:

| | |
|---|---|
| pixels that reported | 65,536 |
| pages compacted on the device | 53 |
| bytes the CPU mapped | **860** |
| bytes a per-pixel stream would have been | 262,144 |
| ratio | **305x smaller** |

`VirtualTextureFrameReadback::bytes_read` is that number, and the suite asserts it rather than
describing it. The density lever moves it: at one sample per sixteen pixels the same grid reports
4,096 samples over 45 pages.

The compacted list is also compared **entry for entry and in order** against a CPU mirror built from
`address_of_pixel`, which is `record_feedback`'s arithmetic line for line. The compaction is an
ordered prefix scan rather than an atomic append for exactly that reason: an unordered compaction
would pass a set comparison and fail this one.

### A page table sampled by a shader, and the mip tail's guarantee held on the device

`upload_page_table` writes each entry where `PageTable::linear_index` says it goes — which is why
that function was made public at M7, rather than the arithmetic being reimplemented here. A second
implementation of `mip_offsets + layer * tile_count + y * tiles_x + x` is a sample resolving to the
*wrong* page rather than to no page, which is a picture that is subtly incorrect rather than a
failure.

`sample_pages` walks from the level asked for towards the coarsest, stopping at the first resident
entry — `VirtualTextureSystem::sample()`'s walk, on the device. The suite dispatches it over the
**whole address space** and compares every entry against the CPU's answer:

| | before the tail is resident | after |
|---|---|---|
| pages sampled | 341 | 341 |
| `missing` | **341** | **0** |
| resolved to a coarser level | 0 | 336 |

The first column is why the second means anything. An assertion that nothing is missing, over a table
where nothing could be missing, is an assertion about an empty set.

### More than one subsystem registered against the residency policy

Until M7 exactly one thing in this tree registered a subsystem: `samples/06-open-world` registers
`Subsystem::Texture`, and its own comment says "One subsystem, because this sample pages one kind of
thing; the point of `residency` is that a second one would join the same policy." A capability whose
entire subject is arbitration, exercised by a single claimant, is a capability nothing has tested.

`FrameResidency` is the second claimant, and both come from dispatches that ran:

* `Subsystem::Texture` — the compacted page requests the resolve pass produced.
* `Subsystem::Geometry` — the level each instance was actually drawn at, which is what the culling
  dispatch's payloads say. A mesh LOD is a paged resource with a ladder exactly as a texture mip is.

Both budgets are set below what the frame asked for, so the two actually meet. Measured: texture
425,984 of 425,984 bytes over 53 requests, geometry 1,048,576 of 1,048,576 over 32, 42 admissions —
both won something, neither exceeded its own budget. Under `PressureLevel::Critical` the coordinated
reduction is one plan over both subsystems, walked in the declared order:

    reduce texture  texture-mip-bias          0.00 -> 2.00  (order 0)
    reduce texture  texture-prefetch-radius   3.00 -> 1.00  (order 0)
    reduce geometry geometry-error-threshold  1.00 -> 4.00  (order 1)

Texture declares `reduction_order` 0 and geometry 1, so texture gives way first. That is a decision
the policy **declared**, not one that emerged from the order the two happened to register in.

## What `LeverSchedule` gained, and why

M7's budget-arbiter spike (design.md §2.10) found the residency policy was missing exactly one thing:
a subsystem must declare **what each ladder position costs relative to position 0**. Every mechanism
the arbiter uses is expressed in terms of that number — the deadband is half the coarsest reachable
lever quantum, the actuator forces one step per subsystem until `gain * error` of the deficit is
covered, and a restore is granted all-or-nothing only when the measured headroom covers the step's
predicted increase. `LeverSchedule::relative_cost` is that fourth field, and the two policies here
are the first to fill it in.

## What layer 4 costs, and what it does not

This module names a device, a buffer, a shader and a render graph — all four of which
`src/servers/render/virtual_texturing/` may not. What it does **not** name is a policy: `residency`
still owns scoring, budgets and eviction, and there is no method here that chooses which page to
drop. `FrameResidency` turns feedback into requests and reports what the server decided.

## The limit, stated

The feedback count array and the page-table buffer are both one entry per addressable page, which is
the flat page table's cost and the flat page table's ceiling. `create()` refuses a texture whose
pyramid exceeds `PageTable::kFlatEntryLimit` (65,536 entries), naming the limit, rather than
allocating 128 MB of mostly-zero words. A hash on the device is what a 512k-texel virtual space needs
and it is not here.

## Tests

    just test-render -R render.virtual_texturing_gpu

`-R`, not a bare name: `just test-render <word>` appends the word to ctest's argument list, where it
filters nothing and the whole render label runs instead. Every command in this file was executed as
written.

Needs a Vulkan device; skips loudly, naming the backend that was selected instead, on a machine
without one. The CPU-side suites are still `unit.virtual_texturing` and are not replaced by these:
the whole design argument of the layer-2 module is that its questions can be answered on a machine
with no GPU.
