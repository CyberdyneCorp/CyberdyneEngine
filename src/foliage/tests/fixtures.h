#pragma once
// The world CyberFoliage's suites are written against.
//
// Four species over three classes, because a fixture with one species would let a suite pass while
// the per-species blocks, the budget's reduction order and the promotion refusal were all wrong.
// One of them is protected cover and one is ground cover, which are the two the specification's own
// scenarios single out.
//
// The TERRAIN is a real `terrain::TerrainQuery` over a real `HeightfieldSource` with a real
// `TerrainStore` — not a stub — because M10's brief for this row is to place against what terrain
// actually produced, and a fixture that answered "height 0, slope 0" everywhere would make every
// placement test pass for a reason that has nothing to do with terrain.

#include <cy/core/memory/system_allocator.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/foliage/placement.h>
#include <cy/foliage/species.h>
#include <cy/foliage/system.h>
#include <cy/terrain/surface.h>
#include <cy/terrain/tile.h>

#include <cmath>

namespace cy::foliage::test {

[[nodiscard]] inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// A partition with 128 m level-0 cells at the origin — the shape src/world/'s, src/environment/'s
/// and src/water/'s own fixtures use, so a cell footprint computed here is the one those modules
/// would compute.
///
/// Returned BY REFERENCE to a function-local constant, not by value. `environment::FieldStore`
/// holds the configuration by reference, so a fixture returning a temporary would hand every suite
/// a dangling reference the moment the constructor returned — src/water/tests/fixtures.h records
/// the same trap for the same reason.
[[nodiscard]] inline const world::PartitionConfig& partition() noexcept {
    static const world::PartitionConfig config = [] {
        world::PartitionConfig value;
        value.partition = 1;
        value.base_cell_size = 128.0F;
        value.levels = 3;
        value.level_ratio = 4;
        return value;
    }();
    return config;
}

inline constexpr const char* kPine = "test.pine";
inline constexpr const char* kFern = "test.fern";
inline constexpr const char* kGrass = "test.grass";
inline constexpr const char* kBoulder = "test.boulder";

/// A canopy tree: promotable, tactically important, the full wind hierarchy, and a tier ladder that
/// ends in an aggregate rather than an impostor.
[[nodiscard]] inline SpeciesDeclaration pine() noexcept {
    SpeciesDeclaration species;
    species.name = kPine;
    species.klass = SpeciesClass::Canopy;
    species.surface = FoliageSurface::Foliage;
    species.importance = GameplayImportance::Cover;
    species.tiers = TierMask::of(DetailTier::Detailed) | TierMask::of(DetailTier::Simplified) |
                    TierMask::of(DetailTier::Aggregate) | TierMask::of(DetailTier::Macro);
    species.tier_pixels[0] = 220.0F;
    species.tier_pixels[1] = 40.0F;
    species.tier_pixels[2] = 10.0F;
    species.tier_pixels[3] = 2.0F;
    species.wind = WindDetail::Leaf;
    species.wind_amplitude = 0.09F;
    species.stiffness = 0.65F;
    species.scale_min = 0.8F;
    species.scale_max = 1.6F;
    species.variations = 4;
    species.footprint_metres = 2.5F;
    species.promotable = true;
    return species;
}

/// An understory shrub: decorative, not promotable, branch-level wind.
[[nodiscard]] inline SpeciesDeclaration fern() noexcept {
    SpeciesDeclaration species;
    species.name = kFern;
    species.klass = SpeciesClass::Understory;
    species.surface = FoliageSurface::Thin;
    species.importance = GameplayImportance::Decorative;
    species.tiers = TierMask::of(DetailTier::Detailed) | TierMask::of(DetailTier::Simplified);
    species.tier_pixels[0] = 90.0F;
    species.tier_pixels[1] = 6.0F;
    species.wind = WindDetail::Branch;
    species.wind_amplitude = 0.04F;
    species.stiffness = 0.15F;
    species.scale_min = 0.7F;
    species.scale_max = 1.3F;
    species.variations = 3;
    species.footprint_metres = 0.6F;
    return species;
}

/// Ground cover: expanded on the GPU from patches, so not promotable by construction.
[[nodiscard]] inline SpeciesDeclaration grass() noexcept {
    SpeciesDeclaration species;
    species.name = kGrass;
    species.klass = SpeciesClass::GroundCover;
    species.surface = FoliageSurface::Aggregate;
    species.importance = GameplayImportance::Decorative;
    species.tiers = TierMask::of(DetailTier::Detailed);
    species.tier_pixels[0] = 1.0F;
    species.wind = WindDetail::Sway;
    species.wind_amplitude = 0.02F;
    species.stiffness = 0.05F;
    species.scale_min = 0.9F;
    species.scale_max = 1.1F;
    species.variations = 2;
    species.footprint_metres = 0.1F;
    species.ground_cover = true;
    return species;
}

/// A rock placed by foliage rules: no wind at all, and a `Scatter` class.
[[nodiscard]] inline SpeciesDeclaration boulder() noexcept {
    SpeciesDeclaration species;
    species.name = kBoulder;
    species.klass = SpeciesClass::Scatter;
    species.surface = FoliageSurface::Aggregate;
    species.importance = GameplayImportance::Relevant;
    species.tiers = TierMask::of(DetailTier::Detailed) | TierMask::of(DetailTier::Simplified);
    species.tier_pixels[0] = 120.0F;
    species.tier_pixels[1] = 4.0F;
    species.wind = WindDetail::None;
    species.scale_min = 0.5F;
    species.scale_max = 2.0F;
    species.variations = 5;
    species.footprint_metres = 1.5F;
    species.promotable = true;
    return species;
}

[[nodiscard]] inline Status declare_all(SpeciesLibrary& library) noexcept {
    if (Status declared = library.declare(pine()); !declared) {
        return declared;
    }
    if (Status declared = library.declare(fern()); !declared) {
        return declared;
    }
    if (Status declared = library.declare(grass()); !declared) {
        return declared;
    }
    return library.declare(boulder());
}

/// A terrain layout whose level-0 tiles are 64 m — the same edge as the foliage cluster policy
/// below, so a region and a tile line up and a test reading "the slope at this candidate" is
/// reading a number the terrain actually stores.
[[nodiscard]] inline terrain::TileLayout terrain_layout() noexcept {
    terrain::TileLayout layout;
    layout.scheme = terrain::CoordinateScheme::PlanarGrid;
    layout.tile_metres = 64.0F;
    layout.levels = 3;
    layout.level_ratio = 2;
    layout.height_min = -16.0F;
    layout.height_max = 240.0F;
    return layout;
}

/// The cluster policy the suites use. 64 m regions, 2 048 target instances.
[[nodiscard]] inline ClusterPolicy policy() noexcept {
    ClusterPolicy value;
    value.edge_metres = 64.0F;
    value.target_instances = 2048;
    value.max_instances = 8192;
    return value;
}

/// A hillside: height rises to the east and the south, so slope and altitude both vary across one
/// region and a slope test excludes part of it rather than all or none of it.
[[nodiscard]] inline f32 hillside_height(f64 x, f64 z) noexcept {
    return static_cast<f32>(6.0 + (x * 0.18) + (z * 0.05) +
                            (8.0 * std::sin(x * 0.02) * std::cos(z * 0.017)));
}

/// Fill a rectangle of terrain tiles with the hillside. Returns how many tiles were written.
[[nodiscard]] inline Expected<u32, Error> build_hillside(terrain::TerrainStore& store, i32 min_x,
                                                         i32 min_z, i32 max_x, i32 max_z) noexcept {
    u32 written = 0;
    const terrain::TileLayout& layout = store.layout();
    for (i32 tz = min_z; tz <= max_z; ++tz) {
        for (i32 tx = min_x; tx <= max_x; ++tx) {
            const terrain::TileCoord coord{tx, tz, 0};
            Expected<terrain::TerrainTile, Error> tile =
                terrain::TerrainTile::create(allocator(), layout, coord, 0.0F);
            if (!tile) {
                return make_unexpected(tile.error());
            }
            for (u32 j = 0; j < terrain::kTileVerts; ++j) {
                for (u32 i = 0; i < terrain::kTileVerts; ++i) {
                    const terrain::TerrainPoint at = terrain::sample_position(layout, coord, i, j);
                    tile.value().set_stored(
                        i, j, terrain::quantise_height(layout, hillside_height(at.x, at.z)));
                }
            }
            tile.value().refresh_extent(layout);
            if (Status inserted = store.insert(static_cast<terrain::TerrainTile&&>(tile.value()));
                !inserted) {
                return make_unexpected(inserted.error());
            }
            ++written;
        }
    }
    return written;
}

/// Punch a hole into one tile's quad grid. `terrain` treats a hole as the absence of a surface, and
/// placement must refuse to plant on one — which is only measurable against a real hole.
[[nodiscard]] inline Status punch_hole(terrain::TerrainStore& store,
                                       const terrain::TileCoord& coord, u32 min_q,
                                       u32 max_q) noexcept {
    const terrain::TerrainTile* resident = store.find(coord);
    if (resident == nullptr) {
        return fail(ErrorCode::NotFound, "no such terrain tile");
    }
    // `TerrainStore::find()` is const and a cooked tile is immutable once inserted, so the hole is
    // punched by rebuilding the tile — which is what a cooker does too.
    Expected<terrain::TerrainTile, Error> rebuilt =
        terrain::TerrainTile::create(allocator(), store.layout(), coord, 0.0F);
    if (!rebuilt) {
        return make_unexpected(rebuilt.error());
    }
    for (u32 j = 0; j < terrain::kTileVerts; ++j) {
        for (u32 i = 0; i < terrain::kTileVerts; ++i) {
            rebuilt.value().set_stored(i, j, resident->stored(i, j));
        }
    }
    for (u32 qj = min_q; qj <= max_q && qj < terrain::kTileQuads; ++qj) {
        for (u32 qi = min_q; qi <= max_q && qi < terrain::kTileQuads; ++qi) {
            rebuilt.value().set_hole(qi, qj, true);
        }
    }
    rebuilt.value().refresh_extent(store.layout());
    if (Status evicted = store.evict(coord); !evicted) {
        return evicted;
    }
    return store.insert(static_cast<terrain::TerrainTile&&>(rebuilt.value()));
}

/// A rule set: pines on gentle slopes, ferns anywhere that is not too steep, boulders on the steep
/// ground the pines refuse. Three rules, so a spacing conflict between two priorities is real.
[[nodiscard]] inline Status author_rules(PlacementRuleSet& rules) noexcept {
    PlacementRule pines;
    pines.species = species_id(kPine);
    pines.density_per_hectare = 320.0F;
    pines.spacing_metres = 3.0F;
    pines.priority = 10;
    pines.orientation = OrientationMode::AlignToSlope;
    pines.tests[0] = RuleTest{RuleInput::Slope, 0.0F, 0.0F, 18.0F, 30.0F, 0, false};
    pines.tests[1] = RuleTest{RuleInput::Altitude, 0.0F, 2.0F, 400.0F, 600.0F, 0, false};
    pines.test_count = 2;
    if (Status pushed = rules.rules.push_back(pines); !pushed) {
        return pushed;
    }

    PlacementRule ferns;
    ferns.species = species_id(kFern);
    ferns.density_per_hectare = 900.0F;
    ferns.spacing_metres = 1.0F;
    ferns.priority = 1;
    ferns.tests[0] = RuleTest{RuleInput::Slope, 0.0F, 0.0F, 34.0F, 45.0F, 0, false};
    ferns.test_count = 1;
    if (Status pushed = rules.rules.push_back(ferns); !pushed) {
        return pushed;
    }

    PlacementRule boulders;
    boulders.species = species_id(kBoulder);
    boulders.density_per_hectare = 40.0F;
    boulders.spacing_metres = 4.0F;
    boulders.priority = 20;
    boulders.orientation = OrientationMode::RandomLean;
    boulders.tests[0] = RuleTest{RuleInput::Slope, 12.0F, 20.0F, 90.0F, 90.0F, 0, false};
    boulders.test_count = 1;
    return rules.rules.push_back(boulders);
}

/// A real terrain, a real field store, and the sampler over both.
///
/// Constructed in place and never copied or moved: `HeightfieldSource`, `TerrainQuery` and
/// `FieldStore` all hold their inputs BY REFERENCE, so a fixture that returned one by value would
/// hand every suite dangling references — the same trap src/water/tests/fixtures.h records.
struct TestWorld {
    terrain::TileLayout layout;
    terrain::TerrainStore terrain_store;
    terrain::HeightfieldSource heights;
    terrain::TerrainQuery query;
    environment::FieldRegistry registry;
    environment::FieldStore fields;

    TestWorld() noexcept
        : layout(terrain_layout()),
          terrain_store(allocator(), layout),
          heights(terrain_store, nullptr),
          query(allocator()),
          registry(allocator()),
          fields(allocator(), registry, partition()) {}

    TestWorld(const TestWorld&) = delete;
    TestWorld& operator=(const TestWorld&) = delete;

    /// Cook a rectangle of hillside and make it the query's surface source.
    [[nodiscard]] Status build(i32 min_x, i32 min_z, i32 max_x, i32 max_z) noexcept {
        if (Expected<u32, Error> built = build_hillside(terrain_store, min_x, min_z, max_x, max_z);
            !built) {
            return make_unexpected(built.error());
        }
        return query.add_source(heights);
    }
};

/// The sun the suites evaluate exposure against. Declared rather than sampled, so placement
/// produces the same forest at midnight as at noon.
[[nodiscard]] inline Vec3 test_sun() noexcept {
    return Vec3{0.3F, -0.9F, 0.31F};
}

/// A generation context over a world and a rule set.
[[nodiscard]] inline GenerationContext context_for(const SpeciesLibrary& library,
                                                   const PlacementRuleSet& rules,
                                                   const PlacementSampler& sampler,
                                                   u64 seed) noexcept {
    GenerationContext context;
    context.seed = seed;
    context.library = &library;
    context.rules = &rules;
    context.sampler = &sampler;
    context.policy = policy();
    return context;
}

}  // namespace cy::foliage::test
