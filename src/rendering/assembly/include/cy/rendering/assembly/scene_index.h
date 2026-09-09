#pragma once
// The snapshot, as the index a cull reads. M8.b task 11.2, and the other half of task 11.3.
//
// ================================================================================================
// THE LINK THAT WAS MISSING
// ================================================================================================
//
// `cy::rendering::SnapshotExtractor` turns an ECS world into a `render::RenderSnapshot` at the
// commit boundary, and `cy::rendering::SpatialIndex` is what `cull_view()` culls. Nothing joined
// them. So an authored `MeshRenderer` reached a snapshot and stopped there, and every consumer that
// wanted to draw one built its own index by hand — which at M8.a was nobody, which is why the
// artefact's sphere was drawn through M3's fixed slots as a box.
//
// This is that join, and it is fifty lines because the two data models were designed to meet:
// a snapshot is a DIFF (changed instances and removed stable ids) and a spatial index is a
// long-lived slot allocator, which is exactly the pair "apply a diff to a store" fits.
//
// ================================================================================================
// WHY THE STABLE ID IS THE KEY AND THE SLOT IS NOT
// ================================================================================================
//
// A snapshot addresses everything by `stable_id`, and says why: "everything in the snapshot is
// keyed by it, so a snapshot is meaningful without knowing which slot anything landed in". A
// spatial slot is an allocation and is reused. So this holds the map from one to the other, and it
// is the only place the two numbering schemes meet — the same discipline `Instance::mapping()`
// applies to authoring ids.
//
// ================================================================================================
// THE INTERPOLATION ALPHA IS APPLIED HERE, ONCE
// ================================================================================================
//
// A snapshot carries BOTH placements, because "the alpha is not known at the commit". `apply()`
// takes it and calls `render::resolve_transform`, which is the engine's one blend — including its
// teleport rule, which returns the current placement rather than smearing across the gap.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/culling/spatial.h>
#include <cy/servers/render/snapshot.h>

namespace cy::rendering::assembly {

/// What applying one snapshot did. Counters rather than a bool: "fifty of a hundred thousand
/// instances moved" is the claim the extract stage makes, and a caller has to be able to see it
/// survive this step.
struct SceneIndexReport {
    u32 inserted = 0;
    u32 updated = 0;
    u32 removed = 0;
    /// Removals naming a stable id this index never held. Not an error — a snapshot may remove an
    /// instance a later-started index never saw — and counted so it cannot hide a mismatched key.
    u32 unknown_removals = 0;
};

/// A spatial index kept in step with a stream of render snapshots.
class SceneIndex {
public:
    explicit SceneIndex(Allocator& allocator) noexcept;

    SceneIndex(const SceneIndex&) = delete;
    SceneIndex& operator=(const SceneIndex&) = delete;

    /// Apply one snapshot at one interpolation alpha.
    ///
    /// `SpatialEntry::gpu_slot` IS LEFT AT ZERO, and that is a statement rather than an omission:
    /// the GPU scene is `render::GpuScene`'s and this class does not own one, so it has no number
    /// to put there and inventing one would be a draw pointed at the wrong instance record. What it
    /// publishes instead is `surface_of(slot)` — the mesh and material the snapshot carried — which
    /// is what a surface query resolves a draw through. A caller that does own a GPU scene writes
    /// `gpu_slot` into the index itself, and this map is then the reverse lookup it needs anyway.
    [[nodiscard]] Status apply(const render::RenderSnapshot& snapshot, f32 alpha,
                               SceneIndexReport& out) noexcept;

    [[nodiscard]] const SpatialIndex& index() const noexcept { return index_; }
    [[nodiscard]] SpatialIndex& index() noexcept { return index_; }

    /// The spatial slot an instance's stable id holds, or `kNoSlot`.
    [[nodiscard]] u32 slot_of(u64 stable_id) const noexcept;
    /// The stable id in a spatial slot, or zero. What a draw list's `stable_id` is checked against.
    [[nodiscard]] u64 id_of(u32 slot) const noexcept;

    /// The mesh and material an instance was published with. The renderer's own record of what a
    /// node referenced, which is what a surface query resolves a draw's mesh through.
    struct Surface {
        render::MeshHandle mesh;
        render::MaterialHandle material;
    };
    [[nodiscard]] Surface surface_of(u32 slot) const noexcept;

    [[nodiscard]] u32 live() const noexcept { return live_; }

    static constexpr u32 kNoSlot = ~0U;

private:
    struct Entry {
        u64 stable_id = 0;
        u32 slot = kNoSlot;
        Surface surface;
    };

    [[nodiscard]] Entry* find(u64 stable_id) noexcept;

    SpatialIndex index_;
    /// One entry per live instance, in insertion order. Linear lookup, deliberately: an index
    /// holding tens of thousands would want a map, and putting one here before a profile asked for
    /// it would be a second structure to keep in step for no measured gain. `live()` is what a
    /// caller watches to know when that stops being true.
    Array<Entry> entries_;
    /// Indexed by spatial slot, so a draw list's slot resolves back to its instance in one read.
    Array<u32> slot_to_entry_;
    u32 live_ = 0;
};

}  // namespace cy::rendering::assembly
