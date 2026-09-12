#pragma once
// The collision representation, and the navigation surface derived from it. M10 task 2.2.
//
// `terrain` — "Terrain collision": terrain "SHALL produce a collision representation INDEPENDENT OF
// ITS RENDERING REPRESENTATION", whose resolution is "independently configurable and typically
// coarser than rendering", streamable "at its own granularity", registered "with physics IN BULK on
// cell activation", and invalidated "regionally on gameplay and structural deformation". And
// "Terrain navigation contribution": terrain "SHALL contribute a navigation surface to
// `navigation`, DERIVED FROM ITS COLLISION REPRESENTATION and its material and slope data".
//
// ================================================================================================
// WHY BOTH LIVE IN ONE FILE
// ================================================================================================
//
// Because the second is derived from the first, and the specification says so in as many words. A
// navigation surface built from the RENDER mesh would disagree with what an agent walks on wherever
// the two resolutions differ — which is everywhere, because collision is deliberately coarser.
// Keeping them together makes that derivation a call rather than a convention.
//
// ================================================================================================
// WHAT THIS FILE DOES NOT DO
// ================================================================================================
//
// It creates no physics body and builds no navmesh. `build_collision()` produces a
// `physics::HeightFieldDescription` — the description `physics::create_shape()` takes — and
// `contribute()` produces a `navigation::NavSourceGeometry` — the geometry
// `navigation::build_tile()` takes. Terrain hands both systems the shape they already accept from
// everything else, which is what "registered with physics" and "contributes a navigation surface"
// mean when they are link facts rather than sentences. Neither system is called from here, because
// a subsystem that reached into physics during streaming is the re-entrancy
// `world-partition-and-streaming` forbids.
//
// HOLES CROSS BOTH BOUNDARIES UNCHANGED. A hole becomes `physics::kHeightFieldHole` in the
// collision samples — the sentinel that specification already defines for exactly this — and a quad
// navigation is not given a triangle for. "WHEN a terrain hole is authored for a cave entrance,
// THEN collision SHALL have the hole", and an agent cannot path across one either.

#include <cy/core/base/expected.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/environment/store.h>
#include <cy/navigation/build.h>
#include <cy/servers/physics/shapes.h>
#include <cy/terrain/deform.h>
#include <cy/terrain/surface.h>
#include <cy/terrain/tile.h>

namespace cy::terrain {

/// How collision is derived from the surface. Independently configurable, and typically coarser.
struct CollisionConfig {
    /// Fine samples between collision samples. Two is one collision sample per two metres on a
    /// one-metre terrain; it must divide the tile's quad count.
    u32 decimation = 2;
};

/// One tile's collision heightfield, in the form physics takes.
struct CollisionTile {
    TileCoord coord;
    TerrainBounds bounds;
    /// `edge * edge` samples in metres, row-major in z then x, with `physics::kHeightFieldHole`
    /// where the surface is absent.
    Array<f32> samples;
    u32 edge = 0;
    f32 spacing = 0.0F;
    u32 holes = 0;
    f32 min_height = 0.0F;
    f32 max_height = 0.0F;

    explicit CollisionTile(Allocator& allocator) noexcept : samples(allocator) {}

    CollisionTile(const CollisionTile&) = delete;
    CollisionTile& operator=(const CollisionTile&) = delete;
    CollisionTile(CollisionTile&&) noexcept = default;
    CollisionTile& operator=(CollisionTile&&) noexcept = default;
    ~CollisionTile() = default;
};

/// Build one tile's collision. The VISUAL delta is deliberately not read: `heights` carries the
/// cooked surface plus the gameplay delta and nothing else, which is why a footprint cannot move
/// what a body rests on.
[[nodiscard]] Expected<CollisionTile, Error> build_collision(
    Allocator& allocator, const TerrainStore& store, const HeightfieldSource& heights,
    const TileCoord& coord, const CollisionConfig& config) noexcept;

/// The description `physics::create_shape()` takes. Non-owning: the samples stay in the tile.
[[nodiscard]] physics::HeightFieldDescription describe_collision(
    const CollisionTile& tile) noexcept;

/// What a bulk registration did. `terrain` — collision "SHALL be registered with physics in bulk on
/// cell activation", so the unit of the call is a set of tiles rather than a tile.
struct CollisionRegistration {
    u32 tiles = 0;
    u64 samples = 0;
    u32 holes = 0;
};

/// The resident collision tiles, at their own granularity.
class TerrainCollision {
public:
    TerrainCollision(Allocator& allocator, const TerrainStore& store,
                     const CollisionConfig& config) noexcept;

    /// Build and hold collision for a set of tiles. One call per cell activation.
    [[nodiscard]] Expected<CollisionRegistration, Error> activate(
        const HeightfieldSource& heights, Span<const TileCoord> tiles) noexcept;
    /// Drop them again. One call per cell deactivation.
    [[nodiscard]] u32 deactivate(Span<const TileCoord> tiles) noexcept;

    /// Rebuild every resident collision tile the rectangle touches, and only those. Returns how
    /// many were rebuilt.
    [[nodiscard]] Expected<u32, Error> invalidate(const HeightfieldSource& heights,
                                                  const TerrainBounds& bounds) noexcept;

    [[nodiscard]] const CollisionTile* find(const TileCoord& coord) const noexcept;
    [[nodiscard]] usize resident() const noexcept { return tiles_.size(); }
    [[nodiscard]] const CollisionConfig& config() const noexcept { return config_; }

private:
    [[nodiscard]] Status build_into(const HeightfieldSource& heights,
                                    const TileCoord& coord) noexcept;

    Allocator* allocator_;
    const TerrainStore* store_;
    CollisionConfig config_;
    Array<CollisionTile> tiles_;
    HashMap<u64, usize> index_;
};

// --- The navigation contribution
// ------------------------------------------------------------

/// A declared mapping from a terrain material layer to a navigation area and its cost.
///
/// `terrain` — "Terrain material and environment fields MAY modify navigation cost — mud slowing
/// movement, water raising cost — through DECLARED MAPPINGS RATHER THAN BESPOKE CODE." This struct
/// and the one below are those declarations; there is no other path from a material to a cost.
struct NavLayerMapping {
    u8 layer = 0;
    navigation::AreaType area = navigation::kAreaGround;
    /// The polygon's own traversal cost per metre, as `navigation::NavPolygon::cost` carries it.
    f32 cost = 1.0F;
};

/// The same, driven by an environment field rather than by a material.
struct NavFieldMapping {
    environment::FieldId field;
    /// The band of the field's value in which this mapping applies, in the field's declared unit.
    f32 low = 0.0F;
    f32 high = 1.0F;
    navigation::AreaType area = navigation::kAreaGround;
    f32 cost = 1.0F;
};

/// One tile's navigation surface: the triangles, and the area each one carries.
struct NavContribution {
    TileCoord coord;
    TerrainBounds bounds;
    Array<Vec3> vertices;
    Array<u32> indices;
    Array<navigation::AreaType> area;
    /// Quads omitted because they are holes. An agent cannot path over a cave mouth.
    u32 holes = 0;

    explicit NavContribution(Allocator& allocator) noexcept
        : vertices(allocator), indices(allocator), area(allocator) {}

    NavContribution(const NavContribution&) = delete;
    NavContribution& operator=(const NavContribution&) = delete;
    NavContribution(NavContribution&&) noexcept = default;
    NavContribution& operator=(NavContribution&&) noexcept = default;
    ~NavContribution() = default;

    /// The geometry `navigation::build_tile()` takes. Non-owning.
    [[nodiscard]] navigation::NavSourceGeometry source() const noexcept;
};

/// The mappings, the derivation, and the dirty regions a deformation raises.
class TerrainNavigation {
public:
    explicit TerrainNavigation(Allocator& allocator) noexcept;

    [[nodiscard]] Status add_layer_mapping(const NavLayerMapping& mapping) noexcept;
    [[nodiscard]] Status add_field_mapping(const NavFieldMapping& mapping) noexcept;

    /// Derive one tile's navigation surface from its COLLISION representation. `fields` may be null
    /// when no field mapping is declared.
    [[nodiscard]] Expected<NavContribution, Error> contribute(
        Allocator& allocator, const TerrainStore& store, const CollisionTile& collision,
        const environment::FieldStore* fields) const noexcept;

    /// The area and cost a position takes, from the declared mappings. A field mapping wins over a
    /// layer mapping, because a field is the runtime state and a layer is the cooked one: ground
    /// that is cooked as grass and is currently flooded should cost what the flood costs.
    void resolve(u8 layer, const environment::FieldStore* fields, f64 x, f32 height, f64 z,
                 navigation::AreaType& area, f32& cost) const noexcept;

    /// "Terrain change of the gameplay or structural class SHALL emit a NAVIGATION DIRTY REGION,
    /// and navigation SHALL rebuild affected tiles incrementally rather than globally."
    [[nodiscard]] Status mark_dirty(Span<const TerrainBounds> regions) noexcept;
    [[nodiscard]] Span<const TerrainBounds> dirty() const noexcept { return dirty_.span(); }
    void clear_dirty() noexcept { dirty_.clear(); }

private:
    /// The collision samples as world-space vertices, and one quad's triangles with the area its
    /// declared mapping gives it. Split out of `contribute()` because a function that both walked a
    /// lattice and resolved a cost was forty of cognitive complexity, and the two halves are things
    /// a reader already has names for.
    [[nodiscard]] static Status emit_vertices(const CollisionTile& collision,
                                              NavContribution& out) noexcept;
    [[nodiscard]] Status emit_quad(const TerrainTile& tile, const CollisionTile& collision,
                                   const environment::FieldStore* fields, u32 i, u32 j,
                                   NavContribution& out) const noexcept;

    Array<NavLayerMapping> layers_;
    Array<NavFieldMapping> fields_;
    Array<TerrainBounds> dirty_;
};

}  // namespace cy::terrain
