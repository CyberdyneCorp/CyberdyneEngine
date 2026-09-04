#pragma once
// The scene's spatial indices, and the dense arrays the broad phase actually reads. Task 4.4.3.
//
// `rendering-culling-and-lod` — "Spatial indexing": a scene maintains a `DynamicBvh` over
// renderable geometry, a `DynamicBvh` over volumes (lights, probes, decals, GI volumes, fog
// volumes), and a flat array for always-visible instances that bypass spatial culling. Instance
// bounds live in "a dense, cache-friendly array parallel to a compact per-instance flags-and-mask
// array, so the broad-phase test touches minimal memory".
//
// ================================================================================================
// WHY THE BOUNDS ARE HERE AND NOT IN THE TREE, WHEN THE TREE ALREADY HOLDS BOUNDS
// ================================================================================================
//
// `cy::DynamicBvh` stores FAT bounds — the real box grown by a margin — because that is what makes
// a moving object cost nothing until it leaves its expansion. A cull that read them would reject
// nothing that a tight test would keep, but it would ACCEPT a margin's worth of instances that are
// really outside, every frame, for every view. So the tight bounds live here, in an array parallel
// to the flags and the layer masks, and the tree is used for what a tree is good at: skipping whole
// subtrees. The narrow phase then re-tests the survivors against the tight box it has and the tree
// does not — which is the arrangement `core-math`'s bvh.h documents from the other side.
//
// The dense arrays are also what makes the LINEAR path legitimate. Below a few thousand instances a
// scan over two 32-bit words plus a box beats a tree traversal on a real machine, and above the
// parallel threshold the scan is what partitions cleanly across workers. Both paths read the same
// three arrays, so the tree is an accelerator rather than the representation.
//
// ================================================================================================
// SLOTS ARE STABLE, AND THAT IS THE WHOLE CONTRACT
// ================================================================================================
//
// `insert()` returns a slot that names the same instance until it is removed, and a removed slot is
// reused by a later insert. A caller that keeps slots across removals needs its own generation —
// `cy::Handle` is what that is for — and the render server already holds one, which is why this
// index does not hold a second.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/bvh.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/types.h>

namespace cy::rendering {

/// The per-instance flags word the broad phase reads. One 32-bit load per instance, tested before
/// any geometry — which is why the classification bits (transparent, moved) are here rather than
/// looked up through a material.
enum SpatialFlagBits : u32 {
    /// Set for a live slot. A slot that has been removed has it clear and is skipped without any
    /// other bookkeeping, exactly like `render::kInstanceActive`.
    kSpatialActive = 1U << 0U,
    /// The author's own visibility. Clear means "do not draw", and it is tested with the layer mask
    /// in the same word.
    kSpatialVisible = 1U << 1U,
    /// Bypasses spatial culling entirely: an instance in the flat array rather than in a tree.
    /// Skyboxes, first-person weapons, anything whose bounds are a lie.
    kSpatialAlwaysVisible = 1U << 2U,
    /// Inserted once with tight bounds and never updated. The scenario "static instances are cheap
    /// to maintain" is this bit plus `update()` refusing to touch the tree for one.
    kSpatialStatic = 1U << 3U,
    /// Composites against what is already in the target, so it belongs in the transparent list and
    /// is drawn back to front.
    kSpatialTransparent = 1U << 4U,
    /// Moved since the previous frame: motion vectors are non-zero and a shadow page containing it
    /// must be refreshed. Mirrors `render::kInstanceMoved`.
    kSpatialMoved = 1U << 5U,
    kSpatialCastsShadow = 1U << 6U,
    /// `rendering-culling-and-lod`: "Instances MAY opt out with an `IgnoreOcclusion` flag."
    kSpatialIgnoreOcclusion = 1U << 7U,
};

/// Which index an entry belongs to. A volume is a light, a probe, a decal, a GI volume or a fog
/// volume: things that AFFECT what is drawn rather than being drawn.
enum class SpatialDomain : u8 {
    Renderable = 0,
    Volume,
    Count,
};

/// Which kind of volume an entry in the volume index is. Read only when routing culling results
/// into their typed lists (`rendering-culling-and-lod`: "lights, decals, reflection probes, GI
/// volumes"); the broad phase itself never branches on it.
enum class VolumeKind : u8 {
    None = 0,
    Light,
    Decal,
    ReflectionProbe,
    GiVolume,
    FogVolume,
    Count,
};

/// What a caller supplies to place something in the index.
struct SpatialEntry {
    Aabb bounds = Aabb::empty();
    /// The identity everything deterministic hangs on. Never the slot: see render/sort.h.
    u64 stable_id = 0;
    /// Where the instance's record lives in the GPU scene, carried through culling untouched.
    u32 gpu_slot = 0;
    render::LayerMask layer_mask = render::kDefaultLayer;
    u32 flags = kSpatialActive | kSpatialVisible | kSpatialCastsShadow;
    SpatialDomain domain = SpatialDomain::Renderable;
    /// Meaningful only for `SpatialDomain::Volume`. `None` on a renderable.
    VolumeKind volume_kind = VolumeKind::None;
    /// Metres. Zero means "no limit", which is the common case and the one a zeroed struct gives.
    f32 max_draw_distance = 0.0F;
    /// `residency`'s unified render importance, carried so a budget decision does not have to look
    /// the instance up again.
    f32 importance = 1.0F;
    /// Index into the caller's LOD chain table, and the per-instance LOD bias. The index is the
    /// caller's; this module never dereferences it.
    u32 lod_chain = 0;
    f32 lod_bias = 0.0F;
    /// Radius of the world bounding sphere. Derived from `bounds` at insert, kept because screen
    /// coverage needs it per frame and re-deriving it per view is a square root per instance per
    /// view.
    f32 radius = 0.0F;
};

struct SpatialStatistics {
    u32 renderables = 0;
    u32 volumes = 0;
    u32 always_visible = 0;
    u32 free_slots = 0;
    /// How many `update()` calls actually restructured a tree. The number that says whether the
    /// tree's margin is doing its job: a scene of walking characters should restructure rarely.
    u64 tree_restructures = 0;
    u64 updates = 0;
};

/// The two trees, the flat array, and the dense per-instance arrays they index into.
///
/// Not thread-safe for mutation. Culling READS it from many workers, which is safe because a cull
/// never writes; publication happens on the frame thread between culls.
class SpatialIndex {
public:
    explicit SpatialIndex(Allocator& allocator) noexcept;

    SpatialIndex(const SpatialIndex&) = delete;
    SpatialIndex& operator=(const SpatialIndex&) = delete;

    /// Place an entry. Returns its slot.
    [[nodiscard]] Expected<u32, Error> insert(const SpatialEntry& entry) noexcept;

    /// Move a slot's bounds. Cheap when the new box is still inside the tree's fat bounds, which is
    /// the case the margin exists for; `statistics().tree_restructures` is how a caller sees the
    /// difference rather than assuming it.
    [[nodiscard]] Status update(u32 slot, const Aabb& bounds) noexcept;

    /// Replace a slot's flags-and-mask word without touching the tree. What a visibility toggle and
    /// a moved bit cost.
    [[nodiscard]] Status set_flags(u32 slot, u32 flags) noexcept;
    [[nodiscard]] Status set_layer_mask(u32 slot, render::LayerMask mask) noexcept;

    [[nodiscard]] Status remove(u32 slot) noexcept;

    /// The dense arrays, for the broad phase. Parallel: index them all by slot.
    [[nodiscard]] Span<const Aabb> bounds() const noexcept { return bounds_.span(); }
    [[nodiscard]] Span<const u32> flags() const noexcept { return flags_.span(); }
    [[nodiscard]] Span<const render::LayerMask> layer_masks() const noexcept {
        return masks_.span();
    }
    /// The heavier per-instance data, touched only for survivors.
    [[nodiscard]] Span<const SpatialEntry> entries() const noexcept { return entries_.span(); }

    [[nodiscard]] u32 slot_count() const noexcept { return static_cast<u32>(bounds_.size()); }
    [[nodiscard]] const SpatialEntry& entry(u32 slot) const noexcept;

    /// The slots that bypass spatial culling. Still layer-masked, because "always visible" is a
    /// statement about geometry rather than about which view is looking.
    [[nodiscard]] Span<const u32> always_visible() const noexcept { return always_visible_.span(); }

    [[nodiscard]] const DynamicBvh& tree(SpatialDomain domain) const noexcept;

    /// Visit the slots whose FAT bounds intersect `frustum`. Conservative twice over — see the
    /// header comment — so a caller re-tests with `bounds()[slot]`.
    template <class Fn>
    void query_frustum(SpatialDomain domain, const Frustum& frustum, Fn&& fn) const {
        tree(domain).query_frustum(frustum,
                                   [&fn](u32 /*proxy*/, u64 user) { fn(static_cast<u32>(user)); });
    }

    [[nodiscard]] SpatialStatistics statistics() const noexcept;

    void reset() noexcept;

private:
    [[nodiscard]] bool valid(u32 slot) const noexcept;
    [[nodiscard]] DynamicBvh& tree_for(SpatialDomain domain) noexcept;

    // The three arrays the broad phase reads, in the order it reads them.
    Array<Aabb> bounds_;
    Array<u32> flags_;
    Array<render::LayerMask> masks_;
    Array<SpatialEntry> entries_;
    /// The tree proxy for each slot, or `kNullBvhNode` for a slot that is in no tree.
    Array<u32> proxies_;
    Array<u32> free_slots_;
    Array<u32> always_visible_;
    DynamicBvh renderables_;
    DynamicBvh volumes_;
    SpatialStatistics stats_{};
};

}  // namespace cy::rendering
