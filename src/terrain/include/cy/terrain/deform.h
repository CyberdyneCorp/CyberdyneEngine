#pragma once
// Runtime terrain change: the three classes, the delta over immutable cooked data, what each class
// invalidates, and the persistence and replication of both. M10 task 2.2.
//
// `terrain` — "Deformation classes" and "Terrain deltas use the persistence overlay":
//
//     cooked terrain + terrain delta = current terrain
//
// ================================================================================================
// THE CLASSES ARE NOT A LABEL ON ONE MECHANISM
// ================================================================================================
//
// "Terrain deformation SHALL be classified, because the classes COST DIFFERENT AMOUNTS AND MUST NOT
// SHARE ONE MECHANISM", and its first scenario is the one that decides the data structure: a
// footprint "SHALL be visual deformation and SHALL NOT trigger collision or navigation rebuilds".
//
// A single delta array with a flag beside it would satisfy the invalidation half and fail the query
// half: a gameplay query that reads the same array a footprint wrote returns a surface that is
// half a centimetre lower where somebody walked, on one machine and not on another, and no amount
// of careful invalidation fixes that. So there are TWO delta channels here and they are read by
// different callers:
//
//   * `height_delta()` — the GAMEPLAY surface. `Gameplay` and `Structural` write it; queries,
//     collision and navigation read it. This is the `+ terrain delta` of the equation above.
//   * `visual_delta()` — displacement for rendering only. `Visual` writes it; meshing reads it and
//     nothing else does. `terrain`'s own table calls this "Displacement and decal fields".
//
// `HeightDeltaSource`, the interface surface.h queries through, exposes only the first. That is
// what makes "a footprint does not move the ground gameplay stands on" a property of the type
// rather than of a rule someone has to remember.
//
// ================================================================================================
// STRUCTURAL DEFORMATION IS REFUSED, NAMING WHAT IS MISSING
// ================================================================================================
//
// The table says a structural edit "requires the SDF representation", and this engine does not have
// one — `terrain`'s own representation table marks it Planned. So `apply()` REFUSES a structural
// deformation with `NotImplemented` and a message naming the representation, and changes nothing.
// A silent success that carved a height delta instead would be an engine that reports a tunnel and
// renders a dent.
//
// ================================================================================================
// ONE PERSISTENCE MECHANISM, AND IT IS THE WORLD'S
// ================================================================================================
//
// "Deltas SHALL be recorded in the world persistence overlay ... so that saves, dedicated server
// persistence, replays, and editor play-mode changes handle terrain through the existing mechanism
// RATHER THAN A SECOND ONE." `record_into()` writes into `world::PersistenceOverlay` and
// `restore_from()` reads back out of it; this file opens no file and defines no format of its own
// beyond the bytes of one tile's delta, which is the same blob the replication path sends.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/terrain/surface.h>
#include <cy/terrain/tile.h>
#include <cy/world/coordinates.h>
#include <cy/world/overlay.h>

namespace cy::terrain {

/// `terrain`'s three classes, in the order of its table.
enum class DeformationClass : u8 {
    /// Tyre ruts, footprints. Displacement and decal fields; invalidates RENDERING ONLY.
    Visual = 0,
    /// Craters, trenches, levelled build sites. A height delta over cooked terrain; invalidates
    /// rendering, collision and navigation.
    Gameplay,
    /// Tunnels, overhangs carved at runtime. A volumetric edit; requires the SDF representation.
    Structural,
};

[[nodiscard]] const char* deformation_class_name(DeformationClass klass) noexcept;

/// The shape a deformation stamps. Two, because the cost of a third is a parameter and the cost of
/// a callback is a runtime change that cannot be replicated as a bounded message.
enum class DeformationShape : u8 {
    /// Radial, falling to zero at the rectangle's inscribed radius. A crater, a footprint.
    Radial = 0,
    /// Constant inside the rectangle. A levelled build site, a trench segment.
    Box,
};

/// One edit.
struct Deformation {
    DeformationClass klass = DeformationClass::Gameplay;
    DeformationShape shape = DeformationShape::Radial;
    TerrainBounds bounds;
    /// Metres added at the centre. Negative digs.
    f32 amount = 0.0F;
    /// Open a hole rather than move the surface. Structural only, and therefore refused here.
    bool opens_hole = false;
    /// What caused it, for the diagnostics view. A literal the caller owns.
    const char* cause = "";
};

/// What one deformation invalidated. `terrain` — "Only the affected region SHALL be invalidated, at
/// tile granularity or finer."
struct InvalidationSet {
    bool rendering = false;
    bool collision = false;
    bool navigation = false;
    /// The tiles whose data changed, every level.
    Array<TileCoord> tiles;
    /// The rectangles navigation must rebuild. Empty for a `Visual` deformation — which is the
    /// "a footprint is cheap" scenario, as a count rather than as a promise.
    Array<TerrainBounds> navigation_dirty;

    explicit InvalidationSet(Allocator& allocator) noexcept
        : tiles(allocator), navigation_dirty(allocator) {}

    InvalidationSet(const InvalidationSet&) = delete;
    InvalidationSet& operator=(const InvalidationSet&) = delete;
    InvalidationSet(InvalidationSet&&) noexcept = default;
    InvalidationSet& operator=(InvalidationSet&&) noexcept = default;
    ~InvalidationSet() = default;
};

/// The overlay channel terrain deltas are recorded under. A subsystem channel rather than an entity
/// override, because a terrain delta belongs to a piece of WORLD and not to an entity — and
/// `world::PersistentId` is required never to be derived from a position, which is exactly what
/// inventing an identifier per tile would have done.
inline constexpr u32 kOverlayChannelTerrain = 1;

/// The deltas, their persistence, and their replication.
///
/// It implements `HeightDeltaSource`, which is how the query path picks up runtime change without
/// knowing any of this exists.
///
/// **DELTAS ARE STORED AT LEVEL 0 AND READ AT EVERY LEVEL.** A coarse tile's sample stands at the
/// same place in the world as one of the fine lattice's samples — that is what `derive_coarse()`
/// makes true — so a query answered from a macro tile resolves its delta by walking down to the
/// level-0 sample at the same position. Writing the stamp into every level instead would cost four
/// thirds of the memory, would make a replicated message carry a coarse tile that spans the whole
/// map, and would have two answers to keep in step for one edit.
class TerrainDeltaStore final : public HeightDeltaSource {
public:
    TerrainDeltaStore(Allocator& allocator, const TileLayout& layout) noexcept;

    // --- HeightDeltaSource: the gameplay surface, and only it.
    [[nodiscard]] f32 height_delta(const TileCoord& coord, u32 i, u32 j) const noexcept override;
    [[nodiscard]] bool hole_delta(const TileCoord& coord, u32 qi, u32 qj) const noexcept override;

    /// Displacement for rendering only. Meshing reads this; queries, collision and navigation do
    /// not. See the header note.
    [[nodiscard]] f32 visual_delta(const TileCoord& coord, u32 i, u32 j) const noexcept;

    /// Apply one deformation. The returned set says what has to be rebuilt and nothing more.
    [[nodiscard]] Expected<InvalidationSet, Error> apply(const Deformation& deformation) noexcept;

    [[nodiscard]] usize deformed_tiles() const noexcept { return tiles_.size(); }
    [[nodiscard]] bool has_delta(const TileCoord& coord) const noexcept;
    void clear() noexcept;

    // --- Persistence
    // ------------------------------------------------------------------------------

    /// Write every deformed tile into the world's persistence overlay, keyed by the cell that
    /// contains the tile's centre. Cooked terrain is untouched, here and everywhere.
    [[nodiscard]] Status record_into(world::PersistenceOverlay& overlay,
                                     const world::PartitionConfig& partition) const noexcept;

    /// Read them back. The store is cleared first, so a restore is a restore and not a merge.
    [[nodiscard]] Status restore_from(const world::PersistenceOverlay& overlay,
                                      const world::PartitionConfig& partition) noexcept;

    // --- Replication
    // ------------------------------------------------------------------------------

    /// Encode every delta the rectangle touches. "Deltas SHALL be spatially scoped and replicable,
    /// so a networked terrain change is a BOUNDED MESSAGE rather than a resend of terrain data" —
    /// the message is proportional to the rectangle, and a test measures exactly that.
    [[nodiscard]] Status encode_region(const TerrainBounds& bounds, Array<u8>& out) const noexcept;
    /// Apply an encoded region. Returns how many tiles it carried.
    [[nodiscard]] Expected<u32, Error> apply_encoded(Span<const u8> message) noexcept;

private:
    /// One tile's runtime change. Dense over the tile rather than sparse over its samples: a tile
    /// that has been deformed at all has usually been deformed over a patch, and a hash map per
    /// sample would cost more than the array it replaces.
    struct TileDelta {
        TileCoord coord;
        Array<f32> gameplay;
        Array<f32> visual;
        Array<u64> holes;

        explicit TileDelta(Allocator& allocator) noexcept
            : gameplay(allocator), visual(allocator), holes(allocator) {}
    };

    /// A coarse tile's sample, resolved to the level-0 sample standing at the same world position.
    struct FineSample {
        TileCoord coord;
        u32 i = 0;
        u32 j = 0;
    };

    [[nodiscard]] FineSample fine_sample_of(const TileCoord& coord, u32 i, u32 j) const noexcept;
    [[nodiscard]] TileDelta* find_delta(const TileCoord& coord) noexcept;
    [[nodiscard]] const TileDelta* find_delta(const TileCoord& coord) const noexcept;
    [[nodiscard]] Expected<TileDelta*, Error> delta_for(const TileCoord& coord) noexcept;
    /// Stamp one tile, and say whether anything moved.
    [[nodiscard]] Expected<bool, Error> stamp_tile(const Deformation& deformation,
                                                   const TileCoord& coord) noexcept;
    [[nodiscard]] static Status encode_tile(const TileDelta& delta, Array<u8>& out) noexcept;

    Allocator* allocator_;
    TileLayout layout_;
    Array<TileDelta> tiles_;
    HashMap<u64, usize> index_;
};

/// The falloff of a stamp at a position, in [0, 1]. A free function so meshing, collision and a
/// test all agree about the shape of a crater without calling into the store.
[[nodiscard]] f32 stamp_weight(const Deformation& deformation, f64 x, f64 z) noexcept;

}  // namespace cy::terrain
