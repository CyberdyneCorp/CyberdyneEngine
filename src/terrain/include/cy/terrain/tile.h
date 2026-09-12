#pragma once
// CyberTerrain's vocabulary: what a tile IS, where it sits, and what it carries. M10 task 2.1.
//
// `terrain` — "Tiled hierarchical storage": terrain "SHALL be stored as TILES in a hierarchy of
// levels, never as one global grid", carrying "height or volumetric data, material layer data,
// biome assignment, holes, and runtime metadata, at multiple resolutions", with an identity that is
// "stable and derived from a tile coordinate and level, so saves, patches, and streaming caches key
// on it".
//
// ================================================================================================
// THE COORDINATE SCHEME IS A FIELD, AND NOTHING BELOW IT NAMES ONE
// ================================================================================================
//
// "The coordinate system SHALL be replaceable — a planar grid initially, a cube-face quadtree for
// planetary worlds later — without changing consumers", and its scenario asks that introducing
// planetary terrain change the scheme "WITHOUT ALTERING the terrain query, collision, or rendering
// interfaces".
//
// So `TileLayout::scheme` is the only place a scheme is named. `TileCoord` is (x, z, level) under
// whatever scheme the layout declares, every consumer in this module takes a `TileCoord` or a world
// position, and no function in surface.h, collision.h, meshing.h or nav.h mentions a scheme at all.
// The scheme contributes to `TileLayout::signature()`, so changing it changes every tile identity —
// which is what makes a streaming cache or a save keyed on the old scheme report a mismatch rather
// than silently answer for a different piece of the world.
//
// ================================================================================================
// WHY HEIGHTS ARE QUANTISED AGAINST THE LAYOUT AND NOT AGAINST THE TILE
// ================================================================================================
//
// A per-tile height range is the obvious encoding and it breaks the one invariant this module is
// judged on. "Meshing ... SHALL produce watertight boundaries between adjacent tiles and between
// levels": two tiles sharing an edge must produce BIT-IDENTICAL positions along it, and two tiles
// with different ranges quantise the same metre to two different numbers. One range, declared on
// the layout, makes the shared edge exact by construction rather than by a tolerance.
//
// The same choice is what makes `derive_coarse()` exact: a coarse sample is a DECIMATION of a fine
// one — the same stored u16 — so a level transition has no seam either. `terrain`: "Macro
// representations SHALL be derived at cook time, not authored."

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash.h>
#include <cy/core/memory/hash_map.h>
#include <cy/world/coordinates.h>

namespace cy::terrain {

/// The seed every identity hash in CyberTerrain uses. A constant and never `hash_seed()`, for the
/// reason `world::kWorldHashSeed` is one: an identifier that changes between two runs of a cooker
/// is not an identifier, and `cy::hash_bytes()` is randomised per process in development builds.
inline constexpr u64 kTerrainHashSeed = 0x6379'6265'7274'726eULL;  // "cybe" "rtrn"

/// Height samples along a tile edge. The last row and column are SHARED with the next tile — a tile
/// owns 64 quads and 65 sample lines — which is what lets two neighbours agree about their boundary
/// without a stitching rule for the common case.
inline constexpr u32 kTileVerts = 65;
inline constexpr u32 kTileQuads = kTileVerts - 1;
inline constexpr u32 kTileHeights = kTileVerts * kTileVerts;

/// Material and biome texels along a tile edge. One per quad rather than one per sample: a material
/// is a property of the surface between samples, and a texel grid one larger than the quad grid
/// would have a row nothing covers.
inline constexpr u32 kTileTexels = kTileQuads;
inline constexpr u32 kTileTexelCount = kTileTexels * kTileTexels;

/// Layers blended at one texel. `terrain` — "The number of layers blended at a texel SHALL be
/// bounded and configurable": bounded here, configurable through `MaterialBound` in material.h,
/// which may not exceed this. Four is the storage bound; a project asking for two pays for two in
/// shading and for four in storage, and the cooker reports what it dropped either way.
inline constexpr u32 kMaxTexelLayers = 4;

/// The coarsest level a layout may declare. Eight levels at a ratio of two span a factor of 128 in
/// tile size, which is the 256 m tile and the 32 km macro tile the HLOD requirement reaches for.
inline constexpr u8 kMaxTerrainLevels = 8;

/// How tile coordinates are laid over the world. See the header note: this enumerator is named in
/// this file and in no other.
enum class CoordinateScheme : u8 {
    /// A planar grid of square tiles. What a bounded world uses.
    PlanarGrid = 0,
    /// A quadtree over one face of a cube projected onto a sphere. Planetary worlds; the face is
    /// `TileLayout::face` and the tile arithmetic is face-local, so a consumer sees the same
    /// `TileCoord` it always did.
    CubeFaceQuadtree,
};

[[nodiscard]] const char* coordinate_scheme_name(CoordinateScheme scheme) noexcept;

/// A tile's place in the hierarchy. Level 0 is the FINEST, matching `world::CellCoord`.
struct TileCoord {
    i32 x = 0;
    i32 z = 0;
    u8 level = 0;

    friend constexpr bool operator==(const TileCoord&, const TileCoord&) noexcept = default;
};

/// An opaque, stable tile identifier. Consumers compare, hash and store it; nothing else.
///
/// Opaque for the reason `world::CellId` is: the moment a consumer can read x out of it, the layout
/// is no longer free to change shape, and this identifier is exactly the thing a save, a patch and
/// a streaming cache key on.
struct TileId {
    u64 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(TileId, TileId) noexcept = default;
    /// Ordered so a cook manifest and a diagnostic can be sorted into one canonical order.
    friend constexpr bool operator<(TileId a, TileId b) noexcept { return a.value < b.value; }
};

/// A position in absolute world space, in the f64 form `world::WorldVec3d` uses. Terrain queries
/// take absolute positions because a terrain spans cells and a cell-relative position would have to
/// name which cell before it could name a point.
struct TerrainPoint {
    f64 x = 0.0;
    f64 z = 0.0;
};

/// A rectangle of world, in absolute metres. The unit every invalidation in this module is
/// expressed in, so that "only the affected region" is a number a test can check rather than a
/// promise.
struct TerrainBounds {
    f64 min_x = 0.0;
    f64 min_z = 0.0;
    f64 max_x = 0.0;
    f64 max_z = 0.0;

    [[nodiscard]] bool contains(f64 x, f64 z) const noexcept {
        return x >= min_x && x <= max_x && z >= min_z && z <= max_z;
    }
    [[nodiscard]] bool overlaps(const TerrainBounds& other) const noexcept {
        return min_x <= other.max_x && max_x >= other.min_x && min_z <= other.max_z &&
               max_z >= other.min_z;
    }
    [[nodiscard]] TerrainBounds expanded(f64 radius) const noexcept {
        return TerrainBounds{min_x - radius, min_z - radius, max_x + radius, max_z + radius};
    }
};

/// Everything the tiling declares. Its `signature()` contributes to every tile identity, so two
/// layouts differing in any field name different tiles for the same piece of world.
struct TileLayout {
    CoordinateScheme scheme = CoordinateScheme::PlanarGrid;
    /// Distinguishes two terrains of one world — a runtime terrain and an editor preview, say — so
    /// their identifiers cannot collide. `world::PartitionConfig::partition` is the same field for
    /// the same reason.
    u32 terrain = 0;
    /// Which cube face, when the scheme is one. Zero for a planar grid.
    u8 face = 0;
    /// Where the level-0 grid's origin sits in absolute space.
    f64 origin_x = 0.0;
    f64 origin_z = 0.0;
    /// The edge of a LEVEL-0 tile, in metres.
    f32 tile_metres = 256.0F;
    u8 levels = 4;
    /// The factor by which a level's tile size exceeds the level below it. Two, so that a coarse
    /// sample is a decimation of a fine one — see the header note on watertightness.
    u32 level_ratio = 2;
    /// The declared height range every stored sample is quantised against. Widening it changes what
    /// every stored u16 means, which is why it is part of the layout and part of its signature.
    f32 height_min = -1024.0F;
    f32 height_max = 8192.0F;

    [[nodiscard]] f64 tile_size(u8 level) const noexcept;
    /// Metres between height samples at a level.
    [[nodiscard]] f32 sample_metres(u8 level) const noexcept;
    /// Deterministic over every field above, fixed-seed, and therefore the same on every machine.
    [[nodiscard]] u64 signature() const noexcept;
    /// Rejects a layout that cannot name tiles: no levels, a non-positive tile size, a ratio below
    /// two, a hierarchy taller than `kMaxTerrainLevels`, or an empty height range.
    [[nodiscard]] bool is_valid() const noexcept;
};

/// The tile identifier for a coordinate under a layout. Deterministic and opaque.
[[nodiscard]] TileId tile_id_of(const TileLayout& layout, const TileCoord& coord) noexcept;

/// Which tile of a level holds a position.
[[nodiscard]] TileCoord tile_at(const TileLayout& layout, f64 x, f64 z, u8 level) noexcept;

/// A tile's extent in absolute metres.
[[nodiscard]] TerrainBounds tile_bounds(const TileLayout& layout, const TileCoord& coord) noexcept;

/// The tile one level coarser that covers `coord`.
[[nodiscard]] TileCoord parent_of(const TileLayout& layout, const TileCoord& coord) noexcept;

/// Absolute position of one height sample. `i` runs along x and `j` along z, both in
/// [0, kTileVerts).
[[nodiscard]] TerrainPoint sample_position(const TileLayout& layout, const TileCoord& coord, u32 i,
                                           u32 j) noexcept;

// --- Quantisation
// ---------------------------------------------------------------------------------

/// Metres to the stored u16, against the layout's declared range. Saturating rather than wrapping:
/// a generator that overshoots the declared range produces a clamped mountain, not a pit.
[[nodiscard]] u16 quantise_height(const TileLayout& layout, f32 metres) noexcept;
[[nodiscard]] f32 dequantise_height(const TileLayout& layout, u16 stored) noexcept;

// --- What a tile carries
// --------------------------------------------------------------------------

/// The dominant layers at one texel, with their weights. `terrain` — "a small number of dominant
/// layer indices with weights — rather than one weight map per layer across the world".
///
/// Weights are u8 and sum to 255 after `normalise()`, so shading divides by a constant and a texel
/// costs eight bytes whatever the world's layer count is.
struct MaterialTexel {
    u8 layer[kMaxTexelLayers] = {};
    u8 weight[kMaxTexelLayers] = {};

    [[nodiscard]] u32 used() const noexcept;
    [[nodiscard]] u8 dominant() const noexcept { return layer[0]; }
    friend constexpr bool operator==(const MaterialTexel&, const MaterialTexel&) noexcept = default;
};

/// One cooked tile. Immutable once inserted: runtime change is a delta over this (deform.h), and
/// "cooked terrain data SHALL never be rewritten at runtime".
struct TerrainTile {
    TileCoord coord;
    TileId id;
    /// `kTileHeights` stored samples, row-major in j then i.
    Array<u16> heights;
    /// `kTileTexelCount` texels, row-major in j then i.
    Array<MaterialTexel> material;
    /// One biome index per texel. Separate from the material layers because a biome is a category
    /// and a material is a blend, and averaging a category produces an index naming nothing.
    Array<u8> biome;
    /// One bit per quad, set where the surface is absent. `terrain` — "regions excluded from the
    /// surface, consistently in rendering, collision, navigation, and queries".
    Array<u64> holes;
    f32 min_height = 0.0F;
    f32 max_height = 0.0F;
    /// What produced this tile: the modifier stack's derivation key (stack.h). A cook cache and an
    /// incremental re-cook both key on it, and a tile whose key differs from the stack's is stale.
    u64 derivation_key = 0;

    explicit TerrainTile(Allocator& allocator) noexcept
        : heights(allocator), material(allocator), biome(allocator), holes(allocator) {}

    TerrainTile(const TerrainTile&) = delete;
    TerrainTile& operator=(const TerrainTile&) = delete;
    TerrainTile(TerrainTile&&) noexcept = default;
    TerrainTile& operator=(TerrainTile&&) noexcept = default;
    ~TerrainTile() = default;

    /// Allocate every channel at its declared size and fill it with the flat default.
    [[nodiscard]] static Expected<TerrainTile, Error> create(Allocator& allocator,
                                                             const TileLayout& layout,
                                                             const TileCoord& coord,
                                                             f32 height) noexcept;

    [[nodiscard]] u16 stored(u32 i, u32 j) const noexcept { return heights[(j * kTileVerts) + i]; }
    void set_stored(u32 i, u32 j, u16 value) noexcept { heights[(j * kTileVerts) + i] = value; }
    [[nodiscard]] const MaterialTexel& texel(u32 i, u32 j) const noexcept {
        return material[(j * kTileTexels) + i];
    }
    [[nodiscard]] MaterialTexel& texel(u32 i, u32 j) noexcept {
        return material[(j * kTileTexels) + i];
    }
    [[nodiscard]] bool hole(u32 qi, u32 qj) const noexcept;
    void set_hole(u32 qi, u32 qj, bool value) noexcept;
    /// Recompute `min_height` and `max_height`. Called by whatever finished writing the heights.
    void refresh_extent(const TileLayout& layout) noexcept;
    [[nodiscard]] u64 bytes() const noexcept;
};

/// Decode one sample in metres.
[[nodiscard]] f32 tile_height(const TileLayout& layout, const TerrainTile& tile, u32 i,
                              u32 j) noexcept;

/// Derive the coarse tile covering `children`, by DECIMATION of the stored samples.
///
/// `terrain` — "Macro representations SHALL be derived at cook time, not authored." Decimation
/// rather than averaging, because an averaged coarse sample is not equal to the fine sample at the
/// same position and a level transition would then have a seam meshing could not close. The
/// children are indexed row-major over a `level_ratio` x `level_ratio` block; a null child leaves
/// its quadrant at the layout's minimum, which is what a partially cooked world looks like.
[[nodiscard]] Expected<TerrainTile, Error> derive_coarse(
    Allocator& allocator, const TileLayout& layout, const TileCoord& coarse,
    Span<const TerrainTile* const> children) noexcept;

// --- The store
// ------------------------------------------------------------------------------------

/// Why a position is being answered at the level it is. `terrain` — "The system SHALL report, per
/// tile, why it is at its current detail and residency", and its scenario names the four causes.
enum class DetailReason : u8 {
    /// The wanted level is resident and was used.
    Resident = 0,
    /// The wanted level is in flight.
    Streaming,
    /// The wanted level was refused by a residency budget.
    Budget,
    /// The wanted level was never asked for, because the viewer is far away.
    Distance,
    /// The wanted level needs a representation this terrain does not have — an SDF, for a position
    /// under an overhang. See surface.h.
    MissingRepresentation,
};

[[nodiscard]] const char* detail_reason_name(DetailReason reason) noexcept;

/// What the store can say about one position.
struct TileDetailReport {
    TileCoord used;
    u8 level_wanted = 0;
    bool resolved = false;
    DetailReason reason = DetailReason::Distance;
    f32 sample_metres = 0.0F;
    u64 bytes = 0;
};

/// Per-terrain diagnostics. "tile bounds, levels and residency ... and per-tile memory and cost."
struct TerrainDiagnostics {
    u32 tiles[kMaxTerrainLevels] = {};
    u64 bytes = 0;
    u32 resident_levels = 0;
    i32 min_tile_x = 0;
    i32 min_tile_z = 0;
    i32 max_tile_x = 0;
    i32 max_tile_z = 0;
    /// Holes across resident tiles, because a hole that did not survive cooking is invisible in
    /// every other report.
    u32 hole_quads = 0;
};

/// The sparse hierarchy of resident tiles. There is no global grid and no dense array of levels:
/// an absent tile costs nothing and a query walks to the next coarser level that has one.
class TerrainStore {
public:
    TerrainStore(Allocator& allocator, const TileLayout& layout) noexcept;

    TerrainStore(const TerrainStore&) = delete;
    TerrainStore& operator=(const TerrainStore&) = delete;

    [[nodiscard]] const TileLayout& layout() const noexcept { return layout_; }
    [[nodiscard]] Allocator& allocator() const noexcept { return *allocator_; }

    [[nodiscard]] Status insert(TerrainTile&& tile) noexcept;
    [[nodiscard]] Status evict(const TileCoord& coord) noexcept;
    [[nodiscard]] const TerrainTile* find(const TileCoord& coord) const noexcept;
    [[nodiscard]] bool is_resident(const TileCoord& coord) const noexcept;

    /// The finest resident tile covering a position, starting at `from_level`. Null when no level
    /// has one, which is the answer a query turns into the declared default rather than a fault.
    [[nodiscard]] const TerrainTile* finest_at(f64 x, f64 z, u8 from_level) const noexcept;

    /// Record why a wanted tile is not here. Set by whatever is streaming — the two reasons a store
    /// cannot infer are the two a viewer most wants to be told, and inventing one would be worse
    /// than the report being absent.
    [[nodiscard]] Status note_wanted(const TileCoord& coord, DetailReason reason) noexcept;
    void clear_wanted() noexcept;

    /// Why this position is at the detail it is. `wanted_level` is what the caller asked for.
    [[nodiscard]] TileDetailReport explain(f64 x, f64 z, u8 wanted_level) const noexcept;

    [[nodiscard]] usize tile_count() const noexcept { return tiles_.size(); }
    [[nodiscard]] u64 bytes_resident() const noexcept { return bytes_; }
    [[nodiscard]] Status tiles_at_level(u8 level, Array<TileCoord>& out) const noexcept;
    [[nodiscard]] TerrainDiagnostics diagnostics() const noexcept;

private:
    [[nodiscard]] usize* slot_of(const TileCoord& coord) noexcept;
    [[nodiscard]] const usize* slot_of(const TileCoord& coord) const noexcept;

    Allocator* allocator_;
    TileLayout layout_;
    Array<TerrainTile> tiles_;
    HashMap<u64, usize> index_;
    HashMap<u64, u8> wanted_;
    u64 bytes_ = 0;
};

/// The terrain tiles of one level that a WORLD CELL covers.
///
/// `terrain` — "Tiles SHALL stream as part of world cells (see `world-partition-and-streaming`),
/// with terrain data cooked as a cell channel." `world::Channel::Terrain` is that channel; this is
/// the arithmetic that says which tiles a cell's payload has to carry, and it is a free function
/// taking a `world::CellCoord` rather than a method on a streamer for the reason
/// `environment::cell_tile_footprint()` is one: a COOKER computes the same rectangle offline, with
/// no streaming object in existence.
[[nodiscard]] Status cell_tile_footprint(const world::PartitionConfig& partition,
                                         const world::CellCoord& cell, const TileLayout& layout,
                                         u8 level, Array<TileCoord>& out) noexcept;

/// The level whose sample spacing is finest while still coarser than `metres_per_pixel` asks for.
///
/// `terrain` — "Transitions SHALL be driven by the same streaming and detail machinery as other
/// content": this is the same error-over-distance decision `virtual-geometry`'s cluster selection
/// makes, expressed for a grid, and it is a free function so a cooker, a streamer and a test all
/// ask it the same question.
[[nodiscard]] u8 level_for_detail(const TileLayout& layout, f64 distance_metres,
                                  f32 metres_per_unit_at_one_metre) noexcept;

}  // namespace cy::terrain

namespace cy {

template <>
struct Hash<terrain::TileId> {
    [[nodiscard]] u64 operator()(terrain::TileId id) const noexcept {
        return hash_integer(id.value, hash_seed());
    }
};

}  // namespace cy
