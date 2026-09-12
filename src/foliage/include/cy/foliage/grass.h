#pragma once
// Ground cover: blades expanded on the GPU from patch descriptions. M10 task 2.4.
//
// `foliage` — "Grass is generated on the GPU": "Dense ground cover SHALL be generated on the GPU
// from PATCH DESCRIPTIONS — density, species, height, orientation, and seed — rather than stored as
// individual instances. Blades SHALL be expanded ONLY WHERE VISIBLE and within a configured
// distance, and the expansion SHALL be BOUNDED BY A BUDGET. Ground cover SHALL RESPOND TO THE WIND
// FIELD AND THE INTERACTION FIELD like other foliage. Persistent storage for ground cover SHALL be
// PROPORTIONAL TO PATCH DESCRIPTIONS, not to blade count."
//
// ================================================================================================
// THE RATIO IS THE REQUIREMENT, AND IT IS MEASURED
// ================================================================================================
//
// A `GrassPatch` is 32 bytes and describes a square of ground. `expand()` derives every blade in it
// from the patch's own seed and the blade's index — so the stored size is `patches * 32` and the
// rendered size is whatever the budget allows, and `GrassReport::bytes_stored` against
// `GrassReport::blades_expanded` is the specification's "tens of millions of blades, tiny storage"
// as a number a test asserts on rather than a claim a reader takes on trust.
//
// ================================================================================================
// WHY THE EXPANSION IS WRITTEN ON THE CPU
// ================================================================================================
//
// For the reason `environment::sample_field_image()` is: this is the algorithm a compute shader
// runs, and an algorithm that exists only in a shader is an algorithm nothing measures. Determinism
// is the property that matters — a blade's position, height, lean and phase must be the same on
// every machine and in every frame, because a trail flattened into the interaction field has to
// land on the same blades — so the derivation is `determinism::RandomStream`, substream by patch
// and draw by blade index, which is the same derivation instance identity uses and for the same
// reason. **The `.slang` module is NOT in this milestone**; see README.md.
//
// ================================================================================================
// A PATCH IS NOT A CLUSTER
// ================================================================================================
//
// A cluster holds instances that exist. A patch holds a rule for blades that do not, until a frame
// asks for them and the budget allows it. They stream together — a patch belongs to a cluster's
// region — but a patch has no slots, no identities and no exceptions, because there is nothing
// stable to anchor one to: `foliage` puts ground cover outside the promotion and exception model
// entirely, and this file is where that shows.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/random.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/foliage/instance.h>
#include <cy/foliage/interaction.h>
#include <cy/foliage/species.h>

namespace cy::foliage {

/// One square of ground cover. The whole of what is stored.
///
/// Thirty-two bytes, and the `static_assert` below is the "proportional to patch descriptions"
/// requirement made checkable: a patch that grew to hold a blade list would still compile.
struct GrassPatch {
    /// The patch's own seed contribution. Derived from the world seed and the patch's position by
    /// the cooker, so two machines expand the same blades.
    u64 seed = 0;
    /// Cluster-relative origin, quantised exactly as `FoliageInstance` is — a patch belongs to a
    /// cluster region and is addressed against the same bounds.
    u16 qx = 0;
    u16 qz = 0;
    /// Edge of the square, in decimetres. 25.5 m at most, which is larger than any patch worth
    /// having: a patch is the granularity the budget reduces at.
    u8 edge_decimetres = 40;
    /// Blades per square metre at full density.
    u8 density = 64;
    /// Blade height in centimetres, and how much it varies.
    u8 height_cm = 30;
    u8 height_variance_cm = 8;
    /// Which ground-cover species. An index into the cluster's species table, as
    /// `FoliageInstance::species_slot` is.
    u8 species_slot = 0;
    /// Base orientation of the blades, as a `u8` turn. Blades fan out around it.
    u8 orientation = 0;
    /// How much the blades fan, in degrees.
    u8 spread_degrees = 90;
    /// Reserved so the record is a round 32 bytes and a later field costs no layout change.
    u8 reserved[1] = {};
    /// Terrain height at the patch's corner, in metres. Stored rather than queried so expansion
    /// does not need a terrain query per blade; the slope is what makes the rest of the square.
    f32 base_height = 0.0F;
    f32 height_dx = 0.0F;
    f32 height_dz = 0.0F;

    [[nodiscard]] f32 edge_metres() const noexcept {
        return static_cast<f32>(edge_decimetres) * 0.1F;
    }
    /// Blades this patch would expand at full density and full area.
    [[nodiscard]] u32 full_blade_count() const noexcept;
};

static_assert(sizeof(GrassPatch) == 32,
              "`foliage` requires stored ground cover to be proportional to PATCH DESCRIPTIONS: a "
              "patch that grew a blade list would satisfy the type system and not the requirement");

/// One expanded blade. Produced by `expand()` and consumed by the renderer; NEVER stored.
struct GrassBlade {
    world::WorldVec3d position;
    f32 height_metres = 0.0F;
    /// Yaw in radians.
    f32 yaw = 0.0F;
    /// Lean from vertical in radians, before wind and interaction.
    f32 lean = 0.0F;
    /// A per-blade phase in [0,1), so a meadow does not move as one sheet.
    f32 phase = 0.0F;
    /// The flattening the interaction field applies here, 0..1.
    f32 flatten = 0.0F;
};

/// The levers the budget moves, and the visibility the expansion respects. `foliage` — "Blades
/// SHALL be expanded ONLY WHERE VISIBLE and within a configured distance, and the expansion SHALL
/// BE BOUNDED BY A BUDGET."
struct GrassBudget {
    /// Beyond this, no blades. The distance lever.
    f32 distance_metres = 40.0F;
    /// Multiplier on every patch's declared density, 0..1. The density lever.
    f32 density_scale = 1.0F;
    /// The hard ceiling on blades expanded in one call. Reaching it is REPORTED, and the reduction
    /// is by patch — the far patches first — rather than by truncating the array, because a
    /// truncated array is a meadow with a straight edge across it.
    u32 max_blades = 500'000;
    /// A patch is skipped entirely when its expanded count would fall below this. Expanding four
    /// blades costs a draw's worth of overhead for four blades.
    u32 min_blades_per_patch = 4;

    [[nodiscard]] bool is_valid() const noexcept {
        return distance_metres > 0.0F && density_scale >= 0.0F && max_blades > 0;
    }
};

/// What one expansion cost and what it gave up.
struct GrassReport {
    u32 patches_considered = 0;
    u32 patches_expanded = 0;
    u32 patches_beyond_distance = 0;
    u32 patches_outside_view = 0;
    u32 patches_below_minimum = 0;
    /// Patches the ceiling stopped. Non-zero means the budget bit.
    u32 patches_over_budget = 0;
    u64 blades_expanded = 0;
    /// What the same patches would have expanded at full density and no ceiling.
    u64 blades_at_full_density = 0;
    /// Bytes the patch descriptions occupy. The storage the requirement is about.
    u64 bytes_stored = 0;

    /// Blades per stored byte. The "tens of millions of blades, tiny storage" ratio.
    [[nodiscard]] f64 blades_per_stored_byte() const noexcept {
        return bytes_stored == 0
                   ? 0.0
                   : static_cast<f64>(blades_expanded) / static_cast<f64>(bytes_stored);
    }
};

/// A cluster's ground cover: the patches, and the expansion over them.
///
/// It holds patches and no blades, which is the whole design. `expand()` writes into a caller's
/// array — the renderer's own staging buffer — so this class never owns a blade either.
class GrassField {
public:
    explicit GrassField(Allocator& allocator) noexcept;

    GrassField(const GrassField&) = delete;
    GrassField& operator=(const GrassField&) = delete;
    GrassField(GrassField&&) noexcept = default;
    GrassField& operator=(GrassField&&) noexcept = default;
    ~GrassField() = default;

    [[nodiscard]] Status add(ClusterId cluster, const ClusterBounds& bounds,
                             const GrassPatch& patch) noexcept;
    [[nodiscard]] usize size() const noexcept { return patches_.size(); }
    [[nodiscard]] u64 bytes() const noexcept;

    /// Drop every patch of one cluster. Ground cover is evicted with the cluster it belongs to.
    [[nodiscard]] u32 evict(ClusterId cluster) noexcept;

    /// Expand the visible patches into `out`.
    ///
    /// `interaction` may be null. When it is not, each blade's `flatten` is sampled from it — which
    /// is "Ground cover SHALL respond to ... the interaction field like other foliage", and it is
    /// the same `InteractionField::sample()` an instanced plant calls, not a second model.
    [[nodiscard]] Expected<GrassReport, Error> expand(const GrassBudget& budget,
                                                      const CullView& view,
                                                      const InteractionField* interaction,
                                                      Array<GrassBlade>& out) const noexcept;

    /// One patch's blades, deterministically. Public because the determinism claim is about THIS
    /// function and a test should be able to call it twice without driving a view.
    [[nodiscard]] static u32 expand_patch(const GrassPatch& patch, const ClusterBounds& bounds,
                                          f32 density_scale, u32 max_blades,
                                          const InteractionField* interaction,
                                          Array<GrassBlade>& out) noexcept;

private:
    struct Entry {
        ClusterId cluster;
        ClusterBounds bounds;
        GrassPatch patch;
    };

    Array<Entry> patches_;
};

/// The stream every grass draw descends from. Separate from the placement stream so that changing
/// the number of draws per blade cannot renumber a single placed tree — `simulation-and-
/// determinism`'s "consuming randomness in one does not shift another's sequence", used rather than
/// hoped for.
inline constexpr const char* kGrassStream = "foliage.grass";

}  // namespace cy::foliage
