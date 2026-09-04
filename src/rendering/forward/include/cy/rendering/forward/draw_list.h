#pragma once
// The draw list: keys, a radix sort over them, and the per-draw instance data. Task 4.3.2.
//
// `rendering-forward-clustered` — "Draw sorting": a 64-bit sort key per draw, opaque draws grouped
// by pipeline then material then mesh, transparent draws ordered back to front, and "Sorting SHALL
// use a radix sort over the packed keys".
//
// ================================================================================================
// THE KEY IS THE RENDER SERVER'S; THE SORT IS THIS MODULE'S. WHY THE SPLIT
// ================================================================================================
//
// `render::make_sort_key` (layer 2) packs the key and `render::sort_draws` orders a list with
// `std::ranges::sort` over the total order `(key, stable_id, surface)`. That function is the
// REFERENCE: it is short, obviously correct, and it is what the determinism argument in sort.h is
// written about.
//
// `radix_sort_draws` below is the same order, produced by a stable least-significant-digit radix
// sort. It exists because the requirement names a radix sort and because a comparison sort over a
// hundred thousand draws costs a frame's budget in cache misses. It is the fast path and the
// reference is the check: `unit.forward_frame` sorts one list both ways and asserts the results are
// byte-identical, which is a stronger statement than either implementation alone.
//
// THE RADIX PASSES ARE OVER THE WHOLE TRIPLE, not over the key. A radix sort over the key alone is
// stable, which means equal keys keep their INPUT order — and the input order is the cull's, which
// is the thing design.md §6 says must not decide anything. Sorting least-significant field first —
// surface, then stable id, then key — makes the result the total order rather than a stable
// approximation of it. Twenty byte-passes instead of eight; still linear, and still deterministic
// with no tie-break left to chance.
//
// ================================================================================================
// THE INSTANCE DATA, AND WHY IT IS NOT `render::GpuInstance`
// ================================================================================================
//
// The requirement lists what per-draw data must reach the shader: "the world transform (3×4), the
// previous world transform, a compact bounds representation, the material index, per-instance
// parameter offsets, a layer mask, LOD and fade factors, and GI/lightmap addressing".
//
// The first four are already in `render::GpuInstance` — the GPU scene, published once per instance
// and read by everything. The last four are per-DRAW rather than per-instance: one instance drawn
// at two LODs during a cross-fade is two draws with two fade factors, and one instance with three
// material slots is three draws with three material indices.
//
// So `GpuDrawInstance` below is the indirection the draw call actually supplies: `first_instance`
// indexes THIS buffer, and this buffer names the GPU scene slot. That is one 32-byte record per
// draw instead of a second copy of a 160-byte one, and it is what lets an instanced batch of five
// hundred draws share one mesh, one material and five hundred fade factors.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/culling/cull.h>
#include <cy/servers/render/mesh.h>
#include <cy/servers/render/sort.h>
#include <cy/servers/render/types.h>

#include <cstddef>
#include <type_traits>

namespace cy::rendering {

/// One drawable surface of one visible instance: what the sort key is packed from.
///
/// Supplied by the caller through `SurfaceQueryFn` rather than looked up here, because the mesh and
/// material tables are the render server's and this module holds no copy of them.
struct DrawSurface {
    /// The compiled program's identity. Whatever the pipeline cache calls it — never a pointer.
    u32 pipeline = 0;
    u32 material = 0;
    u32 mesh = 0;
    /// Which surface of the mesh this is. The field that distinguishes the several draws one
    /// instance produces, and half of the sort's tie-break.
    u32 surface = 0;
    render::BlendMode blend = render::BlendMode::Opaque;
    /// Byte offset of this draw's per-instance parameters in the material parameter buffer.
    u32 parameter_offset = 0;
    /// Lightmap or GI volume addressing, opaque to this module.
    u32 gi_address = 0;
};

/// Where a visible instance's surfaces come from. Returns a span the caller owns for the duration
/// of the build — commonly a slice of a mesh table it already holds.
using SurfaceQueryFn = Span<const DrawSurface> (*)(const VisibleInstance& instance, void* user);

/// PER-DRAW DATA, IN THE LAYOUT THE SHADER SEES. 32 bytes.
///
/// `first_instance` in the draw call indexes an array of these, and `instance_slot` is what it
/// names in the GPU scene. See the header comment for why the two are separate records.
struct alignas(16) GpuDrawInstance {
    /// Into `render::GpuScene`'s flat array. The transform, bounds and previous transform are
    /// there.
    u32 instance_slot = 0;
    /// Into the material table. Duplicated from the surface rather than read through the instance,
    /// because one instance's surfaces have different materials.
    u32 material = 0;
    u32 parameter_offset = 0;
    u32 gi_address = 0;

    u32 layer_mask = 0xFFFFFFFFU;
    /// The LOD level in bits 0..7 and the cross-fade factor as an 8-bit unorm in bits 8..15. Packed
    /// because both are small and a dither test reads them together.
    u32 lod_and_fade = 0;
    u32 surface = 0;
    /// `render::InstanceFlagBits`, copied so a shader that needs the two-sided or skinned bit does
    /// not have to read the whole instance record for one word.
    u32 flags = 0;
};

static_assert(sizeof(GpuDrawInstance) == 32);
static_assert(alignof(GpuDrawInstance) == 16);
static_assert(std::is_trivially_copyable_v<GpuDrawInstance>);

/// Pack a level and a fade into `lod_and_fade`.
[[nodiscard]] u32 pack_lod_and_fade(u32 level, f32 fade) noexcept;
[[nodiscard]] u32 unpack_lod(u32 packed) noexcept;
[[nodiscard]] f32 unpack_fade(u32 packed) noexcept;

/// One view's draw list: the sorted items, the per-draw records they index, and the batches.
struct DrawList {
    explicit DrawList(Allocator& allocator) noexcept;

    DrawList(const DrawList&) = delete;
    DrawList& operator=(const DrawList&) = delete;

    Array<render::DrawItem> items;
    /// Parallel to `items`: entry i is the record draw i's `first_instance` points at. Kept
    /// parallel through the sort, which is why `sort_draw_list()` moves both.
    Array<GpuDrawInstance> instances;
    Array<render::InstancedBatch> batches;

    void clear() noexcept;
};

/// Scratch the sort ping-pongs through. Held by the caller so a frame's sort allocates once, in the
/// same spirit as `CullWorkspace`.
struct DrawSortScratch {
    explicit DrawSortScratch(Allocator& allocator) noexcept
        : order(allocator), alternate(allocator), items(allocator), instances(allocator) {}

    DrawSortScratch(const DrawSortScratch&) = delete;
    DrawSortScratch& operator=(const DrawSortScratch&) = delete;

    Array<u32> order;
    Array<u32> alternate;
    Array<render::DrawItem> items;
    Array<GpuDrawInstance> instances;
};

/// Turn a visible set into unsorted draw items AND their per-draw records, in one pass.
///
/// One pass because the two are built from the same surface query, and because a second pass would
/// need to find each sorted item's visible instance again — a search the sort has already destroyed
/// the information for.
///
/// The sort LAYER comes from the surface's blend mode through `render::sort_layer_for`: a material
/// never chooses its own bucket, which is what keeps two materials with the same blending from
/// landing in different ones.
[[nodiscard]] Status build_draw_list(Span<const VisibleInstance> visible,
                                     SurfaceQueryFn surfaces_of, void* user,
                                     DrawList& out) noexcept;

/// The permutation that puts `draws` into `(key, stable_id, surface)` order, by a stable LSD radix
/// sort over all three fields. `order` receives one index per draw.
[[nodiscard]] Status radix_sort_order(Span<const render::DrawItem> draws, Array<u32>& order,
                                      Array<u32>& scratch) noexcept;

/// Sort a span of items in place, through that permutation. Identical output to
/// `render::sort_draws`, which is what `unit.forward_frame` asserts by sorting one list both ways.
[[nodiscard]] Status radix_sort_draws(Span<render::DrawItem> draws,
                                      DrawSortScratch& scratch) noexcept;

/// Sort a whole draw list: the items and their per-draw records move together, so that draw i's
/// `first_instance` is i afterwards.
[[nodiscard]] Status sort_draw_list(DrawList& list, DrawSortScratch& scratch) noexcept;

}  // namespace cy::rendering
