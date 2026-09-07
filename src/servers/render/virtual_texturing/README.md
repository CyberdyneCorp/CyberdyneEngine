# `src/servers/render/virtual_texturing/` — CyberTexture

Layer 2. `cy::servers-render-virtual-texturing`. M6 tasks 5.1 to 5.4, capability `virtual-texturing`
at **Working**.

Extremely large logical textures backed by only the pages a frame actually needs.

| | |
|---|---|
| `address.h` | Residency models, semantics, format classes, the versioned 56-bit address, and what an asset declares |
| `page_table.h` | Virtual page to physical tile, flat or sparse behind one lookup, updates applied in one batch |
| `physical_cache.h` | Shared caches grouped by format class; refuses when full rather than choosing a victim |
| `feedback.h` | The wait-free request ring, its double bank, compaction and the density lever |
| `producer.h` | The producer interface, its declared cost, and the persistence class of a runtime page |
| `system.h` | `VirtualTextureSystem`: the mip tail, sampling, prefetch, the residency loop, the production pool |

## The two guarantees, and where each lives

M6's exit criterion for this capability is one sentence with two halves.

**Feedback never blocks a frame.** `FeedbackBuffer::record` is wait-free: two atomic increments, a
bounds test and one store, with no lock, no allocation and no retry loop. A full buffer *drops and
counts* — the alternative to dropping a page request is stalling the frame that produced it. The
frame writes one bank while the resolver reads the other, and `swap()` is the only call that waits,
for at most the writers already inside `record`. `tests/test_teardown.cpp` puts eight recording
threads against a churning resolver and measures the worst single `record`.

**The mip tail guarantees a frame is never missing.** `sample()` walks from the level that was asked
for towards the coarsest and stops at the first resident entry; the tail is pinned in its cache and
`PageTable::invalidate` refuses to clear a pinned entry, so the walk cannot fall off the end.
`tests/test_system.cpp` asserts `missing == false` over the **whole address space** of a texture,
not at three sampled points — and asserts it is *true* before the tail is made resident, so the case
is not vacuously green. A cache too small to hold the tail is a configuration error rather than a
system that runs and shows holes.

## Policy is the residency layer's; storage is this module's

`residency` owns scoring, budgets and eviction. So: feedback becomes `residency::Request`s, the
residency server answers with a `residency::Schedule`, and this system does what the schedule says.
There is no method here that chooses which tile to drop, and `PhysicalTileCache::acquire` returns
`Unavailable` rather than evicting so that no such method can grow by accident. An admission this
system cannot act on is handed back with `cancel_admission`, because the policy spent budget on it.

## What layer 2 costs this module, stated plainly

`src/servers/` sits below `src/backends/` (3) and `src/rendering/` (4), so nothing here may name a
device, an image, a buffer, a sampler, a shader or a render graph. What lives here is the model and
the policy-facing half: address encodings, page tables and their staged update lists, the tile
cache's occupancy and its CPU staging bytes, the feedback ring, the producer interface, the
residency requests. **The Vulkan-side upload, the page table image, the sampling shaders and the
analytic derivative reconstruction under a visibility buffer are `src/rendering/` work and arrive
with M7.** `apply_staged()`'s list is exactly the buffer that uploader copies from, which is why the
batching lives here rather than there.

**That module now exists: `src/rendering/virtual_texturing/` (M7 tasks 4.1 and 4.2).** It writes the
feedback buffer from a shader, resolves it on the device without a per-pixel stream reaching the CPU
— 65,536 pixels became 53 compacted requests and 860 mapped bytes — and samples the page table from a
shader over the whole address space, holding the mip tail's guarantee there rather than only here.
`PageTable::linear_index` and `PageTable::entry_count` were made public for it: the uploader has to
write each entry where the shader will look for it, and a second implementation of that arithmetic
would be a sample resolving to the *wrong* page rather than to no page.

## Teardown mid-production

Production runs on worker threads that write into staging bytes the physical caches own. `pool_` is
the **last** member of `VirtualTextureSystem`, so it is the **first** destroyed and every worker is
joined before a cache, a page table or a staging buffer is touched — the same class of defect M5.5's
gate found in Jolt's job bridge. `unregister_texture()`, `invalidate()`, `configure_cache()` and
`reset()` call `pool_.quiesce()` for the same reason. Starting no workers at all is a supported
configuration, not a mode: production then runs on the caller's thread.

## Tests

    just test-unit virtual_texturing                  # address space, page table, cache, feedback, system
    just test-integration virtual_texturing_teardown  # destruction mid-production, feedback under contention

Not gated by any option.
