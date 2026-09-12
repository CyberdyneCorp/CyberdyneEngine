#pragma once
// Terrain as a CITIZEN of the rest of the engine: the environment field it produces, the fields it
// reads, the cooked field tiles it supplies, and the one call that turns a deformation into every
// invalidation it causes. M10 tasks 2.1 and 2.2.
//
// ================================================================================================
// TERRAIN IS A PRODUCER, AND IT IS ONE THROUGH THE REFUSAL RATHER THAN AROUND IT
// ================================================================================================
//
// `environment-fields` puts fields BENEATH terrain: "Putting moisture inside terrain would make
// foliage depend on terrain to know whether the ground is wet." So terrain reads moisture, wetness,
// snow depth, water distance, water depth and flow, and writes exactly ONE standard field — `soil`,
// the ground's own material at a position, which is terrain's to know and nobody else's.
//
// `register_producer()` goes through `environment::FieldRegistry::claim()` and keeps the
// `ProducerToken` it returns. A second claim on `soil` fails naming both producers, and there is no
// path in this module that writes a field without a token. Terrain does not get an exemption from
// the rule it is the first user of.
//
// ================================================================================================
// AND IT IS THE FIRST ROW THAT COOKS FIELD TILES
// ================================================================================================
//
// `environment-fields`: "Field tiles SHALL be cooked as a cell channel and stream with world
// cells." `environment::FieldStreaming::set_loader()` is the seam that was left for whichever
// producer cooked first, and `terrain_soil_loader()` is terrain filling it: a field tile's bytes
// derived from the cooked terrain tiles under it. A field with no loader materialises its declared
// default, which is what `soil` did before this existed.
//
// ================================================================================================
// ONE DEFORMATION, EVERY INVALIDATION IT CAUSES, AND NOTHING MORE
// ================================================================================================
//
// `deform()` is the join: the delta store decides WHAT changed and what class it was, and this
// function propagates that decision to the material pages, the collision tiles and the navigation
// dirty list. It propagates to collision and navigation only for the classes that invalidate them,
// which is `terrain`'s own table — and `TerrainChange` reports each count separately so "a
// footprint is cheap" is three zeroes rather than an assurance.

#include <cy/core/base/expected.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/environment/streaming.h>
#include <cy/terrain/collision.h>
#include <cy/terrain/deform.h>
#include <cy/terrain/material.h>
#include <cy/terrain/surface.h>
#include <cy/terrain/tile.h>

namespace cy::terrain {

/// The declaration of the `soil` field as terrain produces it: the dominant surface material at a
/// position, as an integer category.
///
/// A category and not a scalar, because a soil type interpolated between two indices is an index
/// naming nothing — `environment::validate_declaration()` refuses the other pairing, which is how
/// that mistake is caught rather than debugged. Gameplay-visible, because navigation cost and
/// footstep audio both read it, so its `gameplay_level` is the macro level and the macro level is
/// resident everywhere.
[[nodiscard]] environment::FieldDeclaration soil_field_declaration(f32 fine_cell_metres) noexcept;

/// What one deformation cost, counted rather than claimed.
struct TerrainChange {
    InvalidationSet invalidated;
    u32 pages_invalidated = 0;
    u32 collision_rebuilt = 0;
    u32 navigation_regions = 0;

    explicit TerrainChange(Allocator& allocator) noexcept : invalidated(allocator) {}

    TerrainChange(const TerrainChange&) = delete;
    TerrainChange& operator=(const TerrainChange&) = delete;
    TerrainChange(TerrainChange&&) noexcept = default;
    TerrainChange& operator=(TerrainChange&&) noexcept = default;
    ~TerrainChange() = default;
};

/// Terrain's seat at the table: what it produces, what it reads, and what an edit invalidates.
class TerrainSystem {
public:
    TerrainSystem(Allocator& allocator, const TerrainStore& store,
                  TerrainDeltaStore& deltas) noexcept;

    /// Declare and claim the `soil` field. `producer_name` must outlive the registry — it is the
    /// name a refusal prints.
    [[nodiscard]] Status register_producer(environment::FieldRegistry& registry,
                                           const char* producer_name,
                                           f32 fine_cell_metres) noexcept;

    /// Declare every field terrain READS, so `FieldRegistry::validate()` can check the firewall
    /// over the whole configuration before a frame runs.
    [[nodiscard]] Status declare_consumption(environment::FieldRegistry& registry,
                                             const char* consumer_name,
                                             const MaterialRuleSet& materials) noexcept;

    /// Write the soil field over the tiles named, at one field level. Terrain's own material data
    /// is the source; nothing here invents a value the terrain does not have.
    [[nodiscard]] Expected<u32, Error> publish_soil(environment::FieldStore& fields,
                                                    const HeightfieldSource& heights,
                                                    Span<const TileCoord> tiles,
                                                    environment::FieldResidency level) noexcept;

    /// Apply a deformation and propagate its invalidation. `pages`, `collision` and `navigation`
    /// may each be null when the caller does not carry that half.
    [[nodiscard]] Expected<TerrainChange, Error> deform(const Deformation& deformation,
                                                        const HeightfieldSource& heights,
                                                        MaterialPageCache* pages,
                                                        TerrainCollision* collision,
                                                        TerrainNavigation* navigation) noexcept;

    [[nodiscard]] bool produces_soil() const noexcept { return token_.valid(); }
    [[nodiscard]] environment::FieldId soil() const noexcept { return soil_; }

private:
    /// One field tile's worth of soil, staged into the writer. Split out of `publish_soil()`
    /// because a function that both walked a rectangle of field tiles and filled each one's lattice
    /// was thirty of cognitive complexity for two loops a reader already has names for.
    [[nodiscard]] Status write_soil_tile(environment::FieldWriter& writer,
                                         const environment::TileAddress& address,
                                         f32 cell) const noexcept;

    Allocator* allocator_;
    const TerrainStore* store_;
    TerrainDeltaStore* deltas_;
    environment::ProducerToken token_;
    environment::FieldId soil_;
};

/// The cooked source of a soil field tile, in the shape `environment::TileLoader` takes.
///
/// `user` is a `SoilLoaderContext`. The loader derives the tile's bytes from the cooked terrain
/// under it and returns `NotFound` where no terrain is resident — which the substrate treats as "no
/// cooked data for that tile" and hands the admission back, rather than committing a tile of
/// invented values.
struct SoilLoaderContext {
    const TerrainStore* store = nullptr;
    const environment::FieldRegistry* registry = nullptr;
};

[[nodiscard]] Status terrain_soil_loader(void* user, const environment::TileAddress& address,
                                         Array<u8>& out) noexcept;

}  // namespace cy::terrain
