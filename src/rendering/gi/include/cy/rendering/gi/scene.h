#pragma once
// The GI scene: what the world looks like to an illumination query. Task 9.1.
//
// `rendering-global-illumination` — "The GI scene", "Incremental invalidation", and the vocabulary
// ("GI strategy layers", "Sample confidence") every other file in this module is written in.
//
// ================================================================================================
// THE GI SCENE IS COARSER THAN PRIMARY VISIBILITY, ON PURPOSE, AND BY A DECLARED AMOUNT
// ================================================================================================
//
// Primary visibility wants sub-pixel geometric error. Near-field illumination wants centimetres and
// far-field wants metres, and that is three orders of magnitude of geometry nobody has to trace.
// `ErrorTargets` is those numbers and `select_detail_level` is how they are spent: where an asset
// has a virtual geometry hierarchy, the GI scene asks it for a COARSER LEVEL rather than cooking a
// second simplified mesh. Two representations of one asset that are simplified by two different
// tools disagree at exactly the silhouettes an illumination query is most sensitive to.
//
// ================================================================================================
// CELL-SCOPED, WHICH IS WHY THERE IS NO GLOBAL STRUCTURE HERE
// ================================================================================================
//
// Payloads arrive and leave with world cells. The index is a `cy::DynamicBvh` — incremental by
// construction — so ingesting a cell inserts its surfels and evicting one removes them, and neither
// touches a surfel of any other cell. `GiScene::last_touched_surfels()` is what makes that
// checkable rather than asserted: a global rebuild would report the whole scene.
//
// ================================================================================================
// A SURFEL IS A SURFACE CARD, NOT A TRIANGLE
// ================================================================================================
//
// The GI scene's primitive is a shaded card: position, normal, albedo, emission, roughness and an
// area. That is the representation the surface cache is keyed by and the software tracer resolves
// against, and it is why a secondary hit is a lookup rather than a material evaluation. Triangles
// still exist — the hardware tier traces them through `ray-tracing-infrastructure` — but what a hit
// RESOLVES TO is a card either way, which is what keeps the two tiers agreeing.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/bvh.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>

namespace cy::rendering::gi {

/// `rendering-global-illumination` — "GI strategy layers". Selectable per project and per platform
/// profile; `Baked` is a first-class mode and not a compatibility path.
enum class GiMode : u8 {
    None = 0,
    Baked,
    Probe,
    Dynamic,
    Hybrid,
    Count,
};

[[nodiscard]] const char* gi_mode_name(GiMode mode) noexcept;

/// Where one radiance sample came from. Every source in the specification's table, plus the two
/// world tracing tiers and `None` for a sample nothing answered.
enum class RadianceSource : u8 {
    None = 0,
    Lightmap,
    IrradianceVolume,
    RadianceCache,
    SurfaceCache,
    ScreenTrace,
    SoftwareTrace,
    HardwareTrace,
    ReflectionProbe,
    Sky,
    Count,
};

inline constexpr u32 kRadianceSourceCount = static_cast<u32>(RadianceSource::Count);

[[nodiscard]] const char* radiance_source_name(RadianceSource source) noexcept;

/// A bit per source, for the "no double counting" exclusion mask.
[[nodiscard]] constexpr u32 source_bit(RadianceSource source) noexcept {
    return 1U << static_cast<u32>(source);
}

/// One answer, and how much the resolve should trust it.
///
/// `rendering-global-illumination` — "Sample confidence": every sample carries a confidence
/// "derived from its source, hit validity, cache age, invalidation state, and temporal history
/// validity", and the resolve COMBINES by it rather than selecting on it. A sample with confidence
/// zero is not an error; it is an answer the resolve will weight to nothing.
struct RadianceSample {
    Vec3 radiance{0.0F, 0.0F, 0.0F};
    f32 confidence = 0.0F;
    RadianceSource source = RadianceSource::None;
    /// Distance to what answered, in metres. Negative where nothing was hit — the ray escaped, and
    /// the sky answered. The denoiser's specular reprojection needs this and so does the resolve's
    /// far-field transition.
    f32 hit_distance = -1.0F;
};

/// A surface card. See the header comment: this, not a triangle, is what a hit resolves to.
struct Surfel {
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    Vec3 albedo{0.5F, 0.5F, 0.5F};
    Vec3 emission{0.0F, 0.0F, 0.0F};
    f32 roughness = 1.0F;
    /// The world area the card stands for, in square metres. Drives the surface cache's page size
    /// and the emissive surface's contribution.
    f32 area = 1.0F;
    u32 instance_id = 0;
    u32 material_id = 0;
};

/// `rendering-global-illumination`'s error table, as numbers.
struct ErrorTargets {
    /// Centimetres of world-space error for near-field illumination.
    f32 near_field_metres = 0.02F;
    /// Metres of it for the far field.
    f32 far_field_metres = 1.0F;
    /// Beyond this distance from the camera a query is far-field.
    f32 far_field_begins_metres = 60.0F;
};

/// One level of an asset's virtual geometry hierarchy, as the GI scene sees it.
struct DetailLevel {
    u32 level = 0;
    f32 error_metres = 0.0F;
    u32 surfel_count = 0;
};

/// The coarsest level whose declared error is within `target_error_metres`, or the finest level
/// when even that is too coarse.
///
/// This is the whole of "one hierarchy, two error targets": the GI scene asks the hierarchy it
/// already has for a level, and never cooks a second simplification.
[[nodiscard]] u32 select_detail_level(Span<const DetailLevel> levels,
                                      f32 target_error_metres) noexcept;

/// Why a region of the illumination state stopped being valid.
enum class InvalidationCause : u8 {
    GeometryMoved = 0,
    LightChanged,
    MaterialChanged,
    CellIngested,
    CellEvicted,
    Count,
};

[[nodiscard]] const char* invalidation_cause_name(InvalidationCause cause) noexcept;

/// One invalidation, kept so that "what did this change cause" is answerable.
///
/// `rendering-global-illumination` — "Invalidation is attributable": "WHEN GI cost spikes THEN the
/// system SHALL report which changes caused which invalidations." That is why `source_id` is here:
/// the light, instance or cell that did it.
struct InvalidationRecord {
    Aabb region{};
    InvalidationCause cause = InvalidationCause::GeometryMoved;
    u64 source_id = 0;
    u64 frame = 0;
    /// Filled in by each consuming subsystem as it services the record, so one record accounts for
    /// the whole cost of one change.
    u32 probes = 0;
    u32 surface_pages = 0;
    u32 field_bricks = 0;
};

/// The representation of the world used to answer illumination queries.
///
/// Not thread-safe. Ingestion and eviction are the streaming thread's through the frame's commit
/// boundary; queries are read-only and run on workers between commits.
class GiScene {
public:
    GiScene() noexcept;

    void set_error_targets(const ErrorTargets& targets) noexcept { targets_ = targets; }
    [[nodiscard]] const ErrorTargets& error_targets() const noexcept { return targets_; }

    /// Ingest one world cell's illumination payload. Incremental: only these surfels are inserted,
    /// and `last_touched_surfels()` is that number.
    [[nodiscard]] Status ingest_cell(u64 cell, const Aabb& bounds, Span<const Surfel> surfels,
                                     u64 frame) noexcept;

    /// Evict it again. The affected region is invalidated and nothing else is.
    void evict_cell(u64 cell, u64 frame) noexcept;

    [[nodiscard]] bool cell_resident(u64 cell) const noexcept;
    [[nodiscard]] u32 resident_cell_count() const noexcept;
    [[nodiscard]] usize surfel_count() const noexcept { return live_surfels_; }
    /// How many surfels the last ingest or eviction touched. The number that says an update was
    /// local rather than global.
    [[nodiscard]] usize last_touched_surfels() const noexcept { return last_touched_; }

    /// Whether any resident cell covers `point`. A query outside coverage falls back to the far
    /// field rather than returning black, and this is how the resolve knows which it is.
    [[nodiscard]] bool has_coverage(Vec3 point) const noexcept;

    [[nodiscard]] const Surfel& surfel(u32 index) const noexcept { return surfels_[index]; }

    /// Visit every surfel whose position is inside `box`. `fn(const Surfel&, u32 index)`.
    template <class Fn>
    void query_box(const Aabb& box, Fn&& fn) const {
        index_.query_aabb(box, [&](u32 /*proxy*/, u64 user_data) {
            const u32 slot = static_cast<u32>(user_data);
            if (slot < surfels_.size() && alive_[slot] != 0 &&
                box.contains(surfels_[slot].position)) {
                fn(surfels_[slot], slot);
            }
            return true;
        });
    }

    /// The nearest surfel to `point` within `radius` whose normal agrees with `normal`, or
    /// `kInvalidSurfel`. The lookup a traced hit resolves through.
    [[nodiscard]] u32 nearest_surfel(Vec3 point, Vec3 normal, f32 radius) const noexcept;

    // --- Invalidation ---------------------------------------------------------------------------

    void invalidate(const Aabb& region, InvalidationCause cause, u64 source_id, u64 frame) noexcept;
    [[nodiscard]] Span<const InvalidationRecord> invalidations() const noexcept {
        return invalidations_.span();
    }
    [[nodiscard]] Span<InvalidationRecord> invalidations_mutable() noexcept {
        return {invalidations_.data(), invalidations_.size()};
    }
    void clear_invalidations() noexcept { invalidations_.clear(); }
    /// Invalidations recorded since construction, by cause. The attribution the diagnostics need.
    [[nodiscard]] u32 invalidation_count(InvalidationCause cause) const noexcept;

    static constexpr u32 kInvalidSurfel = ~0U;

private:
    struct Cell {
        u64 id = 0;
        Aabb bounds{};
        Array<u32> slots;
        bool resident = false;
    };

    [[nodiscard]] u32 find_cell(u64 id) const noexcept;
    [[nodiscard]] Expected<u32, Error> allocate_slot(const Surfel& surfel) noexcept;
    void free_slot(u32 slot) noexcept;

    ErrorTargets targets_{};
    Array<Surfel> surfels_;
    Array<u8> alive_;
    Array<u32> proxies_;
    Array<u32> free_slots_;
    Array<Cell> cells_;
    Array<InvalidationRecord> invalidations_;
    DynamicBvh index_;
    usize live_surfels_ = 0;
    usize last_touched_ = 0;
    u32 invalidation_counts_[static_cast<u32>(InvalidationCause::Count)] = {};
};

}  // namespace cy::rendering::gi
