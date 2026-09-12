#include "world.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/pcg/graph.h>
#include <cy/rendering/sky/atmosphere.h>
#include <cy/terrain/deform.h>
#include <cy/water/body.h>
#include <cy/water/displacement.h>
#include <cy/water/foam.h>
#include <cy/weather/preset.h>
#include <cy/world/overlay.h>

#include <chrono>
#include <cmath>
#include <numbers>
#include <utility>

namespace cy::sample::world {
namespace {

namespace sky = cy::rendering::sky;

/// A region is `pcg::kRegionCells` cells on a side, and this is the size of one cell in metres.
/// Four metres, which is also the level-0 terrain sample spacing below — so a terrain sample stands
/// on a raster cell corner and the interpolation is a two-by-two rather than a resample.
constexpr f64 kRegionMetres = 64.0;
constexpr f64 kCellMetres = kRegionMetres / static_cast<f64>(pcg::kRegionCells);

/// The authored node names. Identity derives from them (`pcg::identity.h`), so they are literals
/// in exactly one place.
constexpr const char* kElevationNode = "world.elevation";
constexpr const char* kLandNode = "world.land";
constexpr const char* kErodeNode = "world.erode";
constexpr const char* kFlowNode = "world.flow";
constexpr const char* kFertilityNode = "world.fertility";
constexpr const char* kScatterNode = "world.scatter";
constexpr const char* kFilterNode = "world.filter";
constexpr const char* kSpacingNode = "world.spacing";
constexpr const char* kOutputNode = "world.output";

/// The elevation noise's amplitude and octave count. Named because the sea-level bias below is
/// DERIVED from them: a hand-tuned bias would silently stop putting the shore in the right place
/// the first time anyone changed the amplitude.
/// Where bare rock and thin alpine ground take over from vegetation, in absolute metres. A
/// property of THIS world's elevation range and not a universal tree line, which is why it is a
/// named constant beside the noise it is derived against rather than a number inside two loops.
constexpr f32 kAlpineMetres = 132.0F;

/// The smallest divisor the auto-exposure will use. See `World::shade_sky()`, which measures the
/// numbers this was chosen against.
constexpr f32 kExposureFloor = 0.0015F;

constexpr f32 kNoiseAmplitude = 210.0F;
constexpr u32 kNoiseOctaves = 5;

constexpr const char* kPine = "world.pine";
constexpr const char* kBirch = "world.birch";
constexpr const char* kScrub = "world.scrub";
constexpr const char* kBoulder = "world.boulder";

/// A monotonic millisecond clock. Every number in `FrameCosts` comes from this and from nothing
/// else, so the budget curve is a measurement and not a model.
[[nodiscard]] f64 now_millis() noexcept {
    const auto at = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<f64, std::milli>(at).count();
}

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    if (value < 0.0F) {
        return 0.0F;
    }
    return value > 1.0F ? 1.0F : value;
}

[[nodiscard]] Vec3 mix(Vec3 a, Vec3 b, f32 t) noexcept {
    const f32 k = clamp01(t);
    return Vec3{a.x + ((b.x - a.x) * k), a.y + ((b.y - a.y) * k), a.z + ((b.z - a.z) * k)};
}

/// The engine's own partition. 128 m level-0 cells at the origin, which is the shape src/world/'s,
/// src/environment/'s, src/water/'s, src/foliage/'s and src/weather/'s fixtures all use — so a cell
/// footprint computed here is the one every one of those modules would compute.
///
/// BY REFERENCE to a function-local constant, because `environment::FieldStore` and
/// `water::WaterSystem` hold the configuration by reference and a temporary would dangle the moment
/// the constructor returned. Three of this milestone's modules record that afternoon in their own
/// fixtures; it is recorded once more here so it costs the next reader nothing.
[[nodiscard]] const cy::world::PartitionConfig& partition() noexcept {
    static const cy::world::PartitionConfig config = [] {
        cy::world::PartitionConfig value;
        value.partition = 1;
        value.base_cell_size = 128.0F;
        value.levels = 3;
        value.level_ratio = 4;
        return value;
    }();
    return config;
}

/// The terrain tiling, as a value the constructor can use before `build()` runs.
///
/// 256 m tiles of 65 samples is a 4 m level-0 spacing, which is exactly the generator's raster cell
/// — so a terrain sample stands on a raster cell and the cook interpolates two neighbours rather
/// than resampling a coarser field. The quantisation range is a property of the LAYOUT and not of a
/// tile, which is what makes two tiles' shared edge one number and a level transition seamless; it
/// has to contain everything the generator can produce, so it is generous rather than fitted.
[[nodiscard]] terrain::TileLayout world_layout() noexcept {
    terrain::TileLayout layout;
    layout.scheme = terrain::CoordinateScheme::PlanarGrid;
    layout.terrain = 1;
    layout.origin_x = 0.0;
    layout.origin_z = 0.0;
    layout.tile_metres = 256.0F;
    layout.levels = 3;
    layout.level_ratio = 2;
    layout.height_min = -512.0F;
    layout.height_max = 1024.0F;
    return layout;
}

/// The cluster policy the generator's own output adapter builds under: one cluster per PCG region,
/// which is what makes "a whole region becomes ONE cluster in ONE call" true here.
[[nodiscard]] foliage::ClusterPolicy boulder_policy() noexcept {
    foliage::ClusterPolicy policy;
    policy.edge_metres = static_cast<f32>(kRegionMetres);
    policy.target_instances = 512;
    policy.max_instances = 4096;
    return policy;
}

[[nodiscard]] pcg::RegionExtent extent_of(const WorldOptions& options) noexcept {
    pcg::RegionExtent extent;
    extent.min_x = 0;
    extent.min_z = 0;
    extent.max_x = options.region_edge - 1;
    extent.max_z = options.region_edge - 1;
    extent.level = 0;
    return extent;
}

/// The generator. Nine nodes carrying the three kinds of edge M10's spike measured, because a
/// world generated by a straight line of pure functions would demonstrate none of what section 4
/// built.
///
///   `world.elevation`  value noise over world coordinates: no edge at all.
///   `world.land`       an affine map that drops the sea level through the noise, so there is a
///                      shore. `Compute` is `in * value + second`.
///   `world.erode`      a BOUNDED GATHER with a declared halo — the control node.
///   `world.flow`       the TRANSITIVE edge and M10's named risk: a region reads only its four
///                      neighbours' boundary outflow, but water crosses the world. Solved to
///                      CONVERGENCE, never to a sweep budget — the spike's third condition, and the
///                      only axis on which no configuration reproduced.
///   `world.fertility`  an elementwise map of what the flow deposited: wet ground grows trees.
///   `world.scatter`    candidate generation, and the only node that mints identity. Derived from
///                      (seed, node, region, slot) through substream-then-draw, which is the
///                      spike's fourth condition and the one that mis-bound 43% of 7 877 overrides
///                      when it was a traversal counter.
///   `world.filter`     a threshold on the fertility a candidate landed in.
///   `world.spacing`    CROSS-REGION CONFLICT RESOLUTION over neighbours' CANDIDATES, never their
///                      accepted output — the spike's first condition, refused by name at compile
///                      time if it were spelled the other way.
struct GeneratorGraph {
    explicit GeneratorGraph(Allocator& allocator) noexcept : graph(allocator) {}

    GeneratorGraph(const GeneratorGraph&) = delete;
    GeneratorGraph& operator=(const GeneratorGraph&) = delete;
    GeneratorGraph(GeneratorGraph&&) noexcept = default;
    GeneratorGraph& operator=(GeneratorGraph&&) noexcept = default;

    pcg::Graph graph;
    pcg::AttributeId elevation;
    pcg::AttributeId land;
    pcg::AttributeId eroded;
    pcg::AttributeId flow;
    pcg::AttributeId fertility;
    /// The node the point half hangs off. Carried because `Graph::find_by_name()` answers with a
    /// node rather than with its identity, and a scatter has to name its input by identity.
    pcg::NodeId fertility_node;
};

/// `Graph::add()` returns an identity or an error at every call, and nine unchecked adds is where
/// the ninth is quietly forgotten. One helper, one check.
[[nodiscard]] Status add_node(pcg::Graph& graph, const pcg::GraphNode& node,
                              pcg::NodeId& out) noexcept {
    Expected<pcg::NodeId, Error> added = graph.add(node);
    if (!added) {
        return make_unexpected(added.error());
    }
    out = *added;
    return ok();
}

[[nodiscard]] Status intern(pcg::AttributeTable& table, const char* name,
                            pcg::AttributeId& out) noexcept {
    Expected<pcg::AttributeId, Error> id = table.intern(name, pcg::AttributeType::F32);
    if (!id) {
        return make_unexpected(id.error());
    }
    out = *id;
    return ok();
}

[[nodiscard]] Status build_raster_nodes(GeneratorGraph& built, f32 sea_bias) noexcept {
    pcg::NodeId elevation;
    pcg::NodeId land;
    pcg::NodeId erode;
    pcg::NodeId flow;

    pcg::GraphNode noise;
    noise.name = kElevationNode;
    noise.kind = pcg::NodeKind::Noise;
    noise.output = built.elevation;
    // 700 m features with five octaves: ridges a kilometre-and-a-half world can hold several of,
    // and detail down to about 45 m, which is an order finer than the 4 m raster can resolve.
    noise.params.frequency = 1.0F / 700.0F;
    noise.params.amplitude = kNoiseAmplitude;
    noise.params.count = kNoiseOctaves;
    if (Status added = add_node(built.graph, noise, elevation); !added) {
        return added;
    }

    pcg::GraphNode bias;
    bias.name = kLandNode;
    bias.kind = pcg::NodeKind::Compute;
    bias.inputs[0] = elevation;
    bias.input_count = 1;
    bias.output = built.land;
    bias.params.value = 1.0F;
    bias.params.second = sea_bias;
    if (Status added = add_node(built.graph, bias, land); !added) {
        return added;
    }

    pcg::GraphNode smooth;
    smooth.name = kErodeNode;
    smooth.kind = pcg::NodeKind::Smooth;
    smooth.inputs[0] = land;
    smooth.input_count = 1;
    smooth.output = built.eroded;
    smooth.neighbour_access = pcg::NeighbourAccess::Raster;
    smooth.reach_regions = 1;
    smooth.iteration_bound = 3;
    smooth.params.amplitude = 0.22F;
    if (Status added = add_node(built.graph, smooth, erode); !added) {
        return added;
    }

    pcg::GraphNode propagate;
    propagate.name = kFlowNode;
    propagate.kind = pcg::NodeKind::Propagate;
    propagate.inputs[0] = erode;
    propagate.input_count = 1;
    propagate.output = built.flow;
    propagate.neighbour_access = pcg::NeighbourAccess::Raster;
    propagate.reach_regions = 1;
    // CONVERGENCE, not a budget. The bound is a refusal point: reaching it means the solve did not
    // converge, which `Generator` reports as an error rather than as a result.
    propagate.iteration = pcg::IterationPolicy::Convergence;
    propagate.iteration_bound = 512;
    if (Status added = add_node(built.graph, propagate, flow); !added) {
        return added;
    }

    pcg::GraphNode fertility;
    fertility.name = kFertilityNode;
    fertility.kind = pcg::NodeKind::Compute;
    fertility.inputs[0] = flow;
    fertility.input_count = 1;
    fertility.output = built.fertility;
    // Accumulated water into a fertility in roughly [0, 1]. The scale is small because `Propagate`
    // accumulates a whole catchment into its outlet cells, so a valley floor carries hundreds.
    fertility.params.value = 0.004F;
    fertility.params.second = 0.18F;
    return add_node(built.graph, fertility, built.fertility_node);
}

[[nodiscard]] Status build_point_nodes(GeneratorGraph& built) noexcept {
    pcg::NodeId scatter;
    pcg::NodeId filter;
    pcg::NodeId spacing;
    pcg::NodeId output;

    pcg::GraphNode scatter_node;
    scatter_node.name = kScatterNode;
    scatter_node.kind = pcg::NodeKind::Scatter;
    scatter_node.inputs[0] = built.fertility_node;
    scatter_node.input_count = 1;
    // 96 candidates into a 64 m region against 6 m spacing fights at every boundary, which is what
    // makes the cross-region contention count non-zero — and a conflict resolution that never
    // contends is a conflict resolution nothing has measured.
    scatter_node.params.count = 96;
    if (Status added = add_node(built.graph, scatter_node, scatter); !added) {
        return added;
    }

    pcg::GraphNode filter_node;
    filter_node.name = kFilterNode;
    filter_node.kind = pcg::NodeKind::Filter;
    filter_node.inputs[0] = scatter;
    filter_node.input_count = 1;
    filter_node.reads = built.fertility;
    filter_node.params.value = 0.34F;
    if (Status added = add_node(built.graph, filter_node, filter); !added) {
        return added;
    }

    pcg::GraphNode spacing_node;
    spacing_node.name = kSpacingNode;
    spacing_node.kind = pcg::NodeKind::Spacing;
    spacing_node.inputs[0] = filter;
    spacing_node.input_count = 1;
    spacing_node.neighbour_access = pcg::NeighbourAccess::Candidates;
    spacing_node.reach_regions = 1;
    spacing_node.params.frequency = 6.0F;
    if (Status added = add_node(built.graph, spacing_node, spacing); !added) {
        return added;
    }

    pcg::GraphNode output_node;
    output_node.name = kOutputNode;
    output_node.kind = pcg::NodeKind::Output;
    output_node.inputs[0] = spacing;
    output_node.input_count = 1;
    return add_node(built.graph, output_node, output);
}

[[nodiscard]] Expected<GeneratorGraph, Error> build_generator_graph(Allocator& allocator,
                                                                    f32 sea_bias) noexcept {
    GeneratorGraph built(allocator);
    built.graph.declare("world", 1);
    pcg::DomainMask domains;
    domains.add(pcg::ExecutionDomain::Cook);
    domains.add(pcg::ExecutionDomain::Editor);
    built.graph.declare_domains(domains);
    built.graph.declare_region_size(kRegionMetres, 0);
    // GAMEPLAY determinism, which is what makes the generated world authoritative state a save and
    // a replay can agree about — and which refuses every node `classify_gpu()` would have offered
    // to the device, because M10 has no GPU generation path.
    built.graph.declare_determinism(pcg::DeterminismLevel::Gameplay);

    pcg::AttributeTable& attributes = built.graph.attributes();
    if (Status interned = intern(attributes, "world.elevation", built.elevation); !interned) {
        return make_unexpected(interned.error());
    }
    if (Status interned = intern(attributes, "world.land", built.land); !interned) {
        return make_unexpected(interned.error());
    }
    if (Status interned = intern(attributes, "world.eroded", built.eroded); !interned) {
        return make_unexpected(interned.error());
    }
    if (Status interned = intern(attributes, "world.flow", built.flow); !interned) {
        return make_unexpected(interned.error());
    }
    // `pcg.density` by name, so the raster channel the fertility writes and the candidate column
    // the scatter records are the same compiled identifier — which is what lets the filter test the
    // fertility a candidate landed in.
    if (Status interned = intern(attributes, "pcg.density", built.fertility); !interned) {
        return make_unexpected(interned.error());
    }

    if (Status raster = build_raster_nodes(built, sea_bias); !raster) {
        return make_unexpected(raster.error());
    }
    if (Status points = build_point_nodes(built); !points) {
        return make_unexpected(points.error());
    }
    return built;
}

// ================================================================================================
// THE SPECIES, THE RULES
// ================================================================================================

/// A canopy conifer: high ground, gentle slopes, the full wind hierarchy.
[[nodiscard]] foliage::SpeciesDeclaration pine() noexcept {
    foliage::SpeciesDeclaration species;
    species.name = kPine;
    species.klass = foliage::SpeciesClass::Canopy;
    species.surface = foliage::FoliageSurface::Foliage;
    species.importance = foliage::GameplayImportance::Cover;
    species.tiers = foliage::TierMask::of(foliage::DetailTier::Detailed) |
                    foliage::TierMask::of(foliage::DetailTier::Simplified) |
                    foliage::TierMask::of(foliage::DetailTier::Aggregate);
    species.tier_pixels[0] = 220.0F;
    species.tier_pixels[1] = 40.0F;
    species.tier_pixels[2] = 8.0F;
    species.wind = foliage::WindDetail::Leaf;
    species.wind_amplitude = 0.55F;
    species.stiffness = 0.55F;
    species.scale_min = 0.8F;
    species.scale_max = 1.7F;
    species.variations = 4;
    species.footprint_metres = 3.0F;
    species.promotable = true;
    return species;
}

/// A broadleaf that prefers the damp valley floors the flow node carved.
[[nodiscard]] foliage::SpeciesDeclaration birch() noexcept {
    foliage::SpeciesDeclaration species;
    species.name = kBirch;
    species.klass = foliage::SpeciesClass::Canopy;
    species.surface = foliage::FoliageSurface::Foliage;
    species.importance = foliage::GameplayImportance::Cover;
    species.tiers = foliage::TierMask::of(foliage::DetailTier::Detailed) |
                    foliage::TierMask::of(foliage::DetailTier::Simplified);
    species.tier_pixels[0] = 180.0F;
    species.tier_pixels[1] = 30.0F;
    species.wind = foliage::WindDetail::Leaf;
    species.wind_amplitude = 0.75F;
    species.stiffness = 0.30F;
    species.scale_min = 0.7F;
    species.scale_max = 1.4F;
    species.variations = 3;
    species.footprint_metres = 2.4F;
    species.promotable = true;
    return species;
}

/// Understory scrub on the steep ground the trees refuse.
[[nodiscard]] foliage::SpeciesDeclaration scrub() noexcept {
    foliage::SpeciesDeclaration species;
    species.name = kScrub;
    species.klass = foliage::SpeciesClass::Understory;
    species.surface = foliage::FoliageSurface::Thin;
    species.importance = foliage::GameplayImportance::Decorative;
    species.tiers = foliage::TierMask::of(foliage::DetailTier::Detailed) |
                    foliage::TierMask::of(foliage::DetailTier::Simplified);
    species.tier_pixels[0] = 90.0F;
    species.tier_pixels[1] = 6.0F;
    species.wind = foliage::WindDetail::Branch;
    species.wind_amplitude = 0.35F;
    species.stiffness = 0.12F;
    species.scale_min = 0.6F;
    species.scale_max = 1.2F;
    species.variations = 3;
    species.footprint_metres = 0.9F;
    return species;
}

/// The glacial erratics the GENERATOR scatters, as opposed to the three species the placement rules
/// do. No wind at all, a `Scatter` class, and a wide spacing — the population `pcg::Spacing`
/// resolved, handed to `pcg::FoliageOutputAdapter` and turned into clusters without an entity
/// anywhere in the path.
[[nodiscard]] foliage::SpeciesDeclaration boulder() noexcept {
    foliage::SpeciesDeclaration species;
    species.name = kBoulder;
    species.klass = foliage::SpeciesClass::Scatter;
    species.surface = foliage::FoliageSurface::Aggregate;
    species.importance = foliage::GameplayImportance::Relevant;
    species.tiers = foliage::TierMask::of(foliage::DetailTier::Detailed) |
                    foliage::TierMask::of(foliage::DetailTier::Simplified);
    species.tier_pixels[0] = 120.0F;
    species.tier_pixels[1] = 4.0F;
    species.wind = foliage::WindDetail::None;
    species.scale_min = 0.6F;
    species.scale_max = 2.4F;
    species.variations = 5;
    species.footprint_metres = 1.6F;
    species.promotable = true;
    return species;
}

/// Which cooked material layer a texel carries: 0 rock, 1 sand, 2 grass, 3 the thin ground above
/// the tree line. A free function so the cook and a reader of the cook agree about the rule.
[[nodiscard]] u8 terrain_layer_of(f32 height, f32 slope, f32 sea_level) noexcept {
    if (height < sea_level + 3.0F) {
        return 1;
    }
    if (slope > 0.62F) {
        return 0;
    }
    return height > kAlpineMetres ? 3 : 2;
}

/// The species class decides the proxy's shape and palette. One function, so the renderer beside
/// this file never has to know what a `SpeciesClass` is.
[[nodiscard]] PlantKind kind_of(foliage::SpeciesClass klass) noexcept {
    if (klass == foliage::SpeciesClass::Scatter) {
        return PlantKind::Boulder;
    }
    return klass == foliage::SpeciesClass::Understory ? PlantKind::Bush : PlantKind::Tree;
}

/// Height as a multiple of the species' declared footprint. A boulder is wider than it is tall,
/// which is the whole visual difference between a rock and a shrub at this distance.
[[nodiscard]] f32 height_ratio(PlantKind kind) noexcept {
    switch (kind) {
        case PlantKind::Boulder:
            return 0.85F;
        case PlantKind::Bush:
            return 1.6F;
        default:
            return 3.4F;
    }
}

[[nodiscard]] f32 radius_ratio(PlantKind kind) noexcept {
    switch (kind) {
        case PlantKind::Boulder:
            return 0.95F;
        case PlantKind::Bush:
            return 0.75F;
        default:
            return 0.62F;
    }
}

[[nodiscard]] Vec3 crown_colour_of(PlantKind kind, f32 variation) noexcept {
    switch (kind) {
        case PlantKind::Boulder:
            // Granite, and it varies toward brown rather than toward green: these are the glacial
            // erratics the GENERATOR scattered, and they should not be mistaken for its trees.
            return Vec3{0.34F + (variation * 0.30F), 0.32F + (variation * 0.22F),
                        0.30F + (variation * 0.14F)};
        case PlantKind::Bush:
            return Vec3{0.20F + (variation * 0.55F), 0.33F + (variation * 0.40F),
                        0.14F + (variation * 0.22F)};
        default:
            return Vec3{0.10F + (variation * 0.45F), 0.27F + (variation * 0.32F),
                        0.12F + (variation * 0.20F)};
    }
}

/// Pines high and gentle, birches low and damp, scrub on the steep. Three rules so a spacing
/// conflict between two priorities is real rather than hypothetical.
[[nodiscard]] Status author_rules(foliage::PlacementRuleSet& rules, f32 sea_level) noexcept {
    foliage::PlacementRule pines;
    pines.species = foliage::species_id(kPine);
    pines.density_per_hectare = 260.0F;
    pines.spacing_metres = 5.0F;
    pines.priority = 10;
    pines.orientation = foliage::OrientationMode::AlignToSlope;
    pines.tests[0] =
        foliage::RuleTest{foliage::RuleInput::Slope, 0.0F, 0.0F, 26.0F, 34.0F, 0, false};
    pines.tests[1] = foliage::RuleTest{foliage::RuleInput::Altitude,
                                       sea_level + 2.0F,
                                       sea_level + 30.0F,
                                       400.0F,
                                       520.0F,
                                       0,
                                       false};
    pines.test_count = 2;
    if (Status pushed = rules.rules.push_back(pines); !pushed) {
        return pushed;
    }

    foliage::PlacementRule birches;
    birches.species = foliage::species_id(kBirch);
    birches.density_per_hectare = 200.0F;
    birches.spacing_metres = 4.0F;
    birches.priority = 8;
    birches.orientation = foliage::OrientationMode::AlignToSlope;
    birches.tests[0] =
        foliage::RuleTest{foliage::RuleInput::Slope, 0.0F, 0.0F, 18.0F, 26.0F, 0, false};
    birches.tests[1] = foliage::RuleTest{
        foliage::RuleInput::Altitude, sea_level + 1.0F, sea_level + 4.0F, 90.0F, 150.0F, 0, false};
    birches.test_count = 2;
    if (Status pushed = rules.rules.push_back(birches); !pushed) {
        return pushed;
    }

    foliage::PlacementRule scrubs;
    scrubs.species = foliage::species_id(kScrub);
    scrubs.density_per_hectare = 700.0F;
    scrubs.spacing_metres = 2.0F;
    scrubs.priority = 1;
    scrubs.tests[0] =
        foliage::RuleTest{foliage::RuleInput::Slope, 0.0F, 0.0F, 42.0F, 55.0F, 0, false};
    scrubs.tests[1] = foliage::RuleTest{
        foliage::RuleInput::Altitude, sea_level + 0.5F, sea_level + 3.0F, 520.0F, 600.0F, 0, false};
    scrubs.test_count = 2;
    return rules.rules.push_back(scrubs);
}

/// The world's climate: temperate, damp, westerly. Uniform, so a rain shadow measured against it is
/// the TERRAIN's doing rather than the climate map's gradient.
[[nodiscard]] weather::ClimateSample world_climate() noexcept {
    weather::ClimateSample climate;
    // A COLD PLACE, and that is a decision the artefact has to make rather than a mood. The
    // milestone's own task asks for "wetness AND SNOW accumulating", and
    // src/weather/tests/fixtures.h records why that needs a cold CLIMATE rather than a colder
    // preset: a regional target is half the place's own climate and half the world's mood, so a
    // snowstorm over a temperate island produces cold rain. This is a sub-arctic coast.
    climate.mean_temperature_celsius = -5.0F;
    climate.temperature_range_celsius = 15.0F;
    climate.humidity = 0.55F;
    climate.prevailing_wind = Vec2{11.0F, 3.0F};
    climate.rainfall_potential_mm = 1400.0F;
    climate.solar_exposure = 0.45F;
    climate.ocean_influence = 0.8F;
    return climate;
}

}  // namespace

// ================================================================================================
// CONSTRUCTION
// ================================================================================================

World::World(Allocator& allocator, const WorldOptions& options) noexcept
    : allocator_(&allocator),
      options_(options),
      generated_(allocator, extent_of(options)),
      generator_(allocator),
      generated_clusters_(allocator),
      outputs_(allocator),
      boulder_adapter_(allocator, generated_clusters_, boulder_policy(),
                       foliage::species_id(kBoulder), options.seed),
      layout_(world_layout()),
      terrain_(allocator, layout_),
      deltas_(allocator, layout_),
      heights_(terrain_, &deltas_),
      query_(allocator),
      terrain_system_(allocator, terrain_, deltas_),
      patches_(allocator),
      registry_(allocator),
      fields_(allocator, registry_, partition()),
      water_(allocator, partition()),
      ocean_(allocator),
      climate_(allocator),
      weather_(allocator),
      species_(allocator),
      rules_(allocator),
      sampler_(allocator),
      clusters_(allocator),
      wind_sampler_(allocator),
      plants_(allocator),
      stars_(allocator),
      cloud_map_(allocator),
      sky_dome_(allocator),
      sky_indices_(allocator),
      star_draws_(allocator) {}

WorldVec3d World::centre() const noexcept {
    const f64 half = static_cast<f64>(options_.region_edge) * kRegionMetres * 0.5;
    return WorldVec3d{half, 0.0, half};
}

f32 World::extent_metres() const noexcept {
    return static_cast<f32>(static_cast<f64>(options_.region_edge) * kRegionMetres);
}

Status World::build(BuildReport& report) noexcept {
    if (Status generated = generate(report); !generated) {
        return generated;
    }
    if (Status cooked = cook_terrain(report); !cooked) {
        return cooked;
    }
    if (Status declared = declare_fields(report); !declared) {
        return declared;
    }
    if (Status configured = configure_water(report); !configured) {
        return configured;
    }
    if (Status configured = configure_weather(report); !configured) {
        return configured;
    }
    // THE FIREWALL, OVER THE WHOLE CONFIGURATION, BEFORE A FRAME RUNS. Every producer has claimed
    // and every consumer has declared what it reads by this point, so `validate()` is the one call
    // that catches a gameplay reader of a presentation field — and it is made here rather than
    // discovered at the first sample.
    Array<environment::FirewallViolation> violations(*allocator_);
    if (Status valid = registry_.validate(violations); !valid) {
        return valid;
    }
    if (!violations.empty()) {
        return fail(ErrorCode::PermissionDenied,
                    "a declared consumer may not read the field it declared");
    }
    if (Status placed = place_foliage(report); !placed) {
        return placed;
    }
    if (Status configured = configure_sky(report); !configured) {
        return configured;
    }
    if (Status meshed = mesh_terrain(report); !meshed) {
        return meshed;
    }
    if (Status dome = build_sky_dome(); !dome) {
        return dome;
    }
    report.fields_declared = static_cast<u32>(registry_.size());
    report.field_bytes = fields_.bytes_resident();
    return ok();
}

// ================================================================================================
// SECTION 4 — THE GENERATOR
// ================================================================================================

Status World::generate(BuildReport& report) noexcept {
    const f64 began = now_millis();

    // THE BIAS THAT PUTS THE SEA LEVEL INSIDE THE NOISE'S RANGE, and it is derived rather than
    // tuned. `pcg::NodeKind::Noise` sums octaves of a value noise in [0, 1] with the amplitude
    // halving each octave, so five octaves of 210 m span [0, 406.9] — entirely above any sea level
    // anyone would pick. Subtracting 0.36 of that span puts about a third of the world under water,
    // which is what gives the shoreline, the depth field and the wetness field something to be.
    const f32 span = kNoiseAmplitude * (2.0F - std::pow(0.5F, static_cast<f32>(kNoiseOctaves - 1)));
    const f32 bias = static_cast<f32>(options_.sea_level) - (span * 0.36F);
    Expected<GeneratorGraph, Error> built = build_generator_graph(*allocator_, bias);
    if (!built) {
        return make_unexpected(built.error());
    }
    elevation_channel_ = built->eroded;
    density_channel_ = built->fertility;

    pcg::CompileReport compiled(*allocator_);
    Array<pcg::CompileDiagnostic> diagnostics(*allocator_);
    Expected<pcg::Program, Error> program =
        pcg::compile(*allocator_, built->graph, compiled, diagnostics);
    if (!program) {
        return make_unexpected(program.error());
    }
    report.program_digest = program->digest();
    report.pcg_cacheable = program->cacheable();
    report.pcg_stages = static_cast<u32>(program->stages().size());

    const pcg::RegionExtent extent = extent_of(options_);

    Expected<pcg::Generator, Error> made =
        pcg::Generator::create(*allocator_, std::move(*program), generated_);
    if (!made) {
        return make_unexpected(made.error());
    }
    if (Status pushed = generator_.push_back(std::move(*made)); !pushed) {
        return pushed;
    }

    generation_context_ = pcg::GenerationContext{};
    generation_context_.seed = options_.seed;
    generation_context_.geometry = &surface_;
    generation_context_.origin_x = 0.0;
    generation_context_.origin_z = 0.0;

    // THE TERMINAL'S OUTPUT ADAPTER. Without one the run computes its result and keeps it, which is
    // what every suite in `src/pcg/` wants and what an editor's region debugger wants; with one the
    // accepted points become a FOLIAGE POPULATION. Registering it here is the difference between a
    // generator that produced numbers and a generator that produced world.
    if (Status declared = species_.declare(boulder()); !declared) {
        return declared;
    }
    if (Status bound = outputs_.register_adapter(boulder_adapter_); !bound) {
        return bound;
    }
    generation_context_.outputs = &outputs_;
    generation_context_.output_target = pcg::OutputTarget::Foliage;

    Expected<pcg::RunProgress, Error> run =
        generator_[0].generate_all(pcg::ExecutionDomain::Cook, generation_context_);
    if (!run) {
        return make_unexpected(run.error());
    }
    if (run->iteration_exhausted) {
        return fail(ErrorCode::Internal,
                    "the flow solve reached its declared bound without converging, which is a "
                    "defect in the generator rather than a result");
    }

    report.pcg_regions = static_cast<u32>(generated_.resident());
    for (const pcg::StageProfile& stage : generator_[0].profile().stages) {
        report.pcg_region_evaluations += stage.regions_evaluated;
    }
    for (i32 z = extent.min_z; z <= extent.max_z; ++z) {
        for (i32 x = extent.min_x; x <= extent.max_x; ++x) {
            const pcg::RegionState* state = generated_.find(pcg::RegionCoord{x, z, 0});
            if (state != nullptr) {
                report.pcg_points += state->accepted.size();
            }
        }
    }
    report.pcg_boulders = boulder_adapter_.instances();
    report.pcg_boulder_clusters = static_cast<u32>(boulder_adapter_.clusters());
    report.pcg_millis = now_millis() - began;
    return ok();
}

void World::GeneratedSurface::height_batch(Span<const f64> x, Span<const f64> z,
                                           Span<f32> out) const noexcept {
    for (usize index = 0; index < out.size() && index < x.size() && index < z.size(); ++index) {
        out[index] = world_->generated_height(x[index], z[index]);
    }
}

void World::GeneratedSurface::slope_batch(Span<const f64> x, Span<const f64> z,
                                          Span<f32> out) const noexcept {
    // A central difference over one raster cell, which is the finest the generated field has: a
    // shorter baseline would report the interpolation's own gradient rather than the terrain's.
    for (usize index = 0; index < out.size() && index < x.size() && index < z.size(); ++index) {
        const f32 east = world_->generated_height(x[index] + kCellMetres, z[index]);
        const f32 west = world_->generated_height(x[index] - kCellMetres, z[index]);
        const f32 north = world_->generated_height(x[index], z[index] + kCellMetres);
        const f32 south = world_->generated_height(x[index], z[index] - kCellMetres);
        const f32 run = static_cast<f32>(2.0 * kCellMetres);
        const f32 dx = (east - west) / run;
        const f32 dz = (north - south) / run;
        out[index] = std::atan(std::sqrt((dx * dx) + (dz * dz))) * 57.2958F;
    }
}

void World::GeneratedSurface::surface_batch(Span<const f64> x, Span<const f64> z,
                                            Span<u8> out) const noexcept {
    // One category: dry land above the sea, and nothing below it. A generator that scattered
    // boulders on the sea bed would be a generator nobody had told where the sea was.
    for (usize index = 0; index < out.size() && index < x.size() && index < z.size(); ++index) {
        out[index] = world_->generated_height(x[index], z[index]) >
                             static_cast<f32>(world_->options_.sea_level) + 1.0F
                         ? 1U
                         : 0U;
    }
}

f32 World::generated_height(f64 x, f64 z) const noexcept {
    // Bilinear over the raster's CELL CENTRES, which stand at origin + (cell + 0.5) * cellMetres —
    // `pcg::Generator`'s own convention, in `eval_noise`. Anchoring on the corners instead would
    // shift the whole terrain by half a cell against the points the same generator scattered.
    const f64 u = (x / kCellMetres) - 0.5;
    const f64 v = (z / kCellMetres) - 0.5;
    const f64 u0 = std::floor(u);
    const f64 v0 = std::floor(v);
    const f32 fu = static_cast<f32>(u - u0);
    const f32 fv = static_cast<f32>(v - v0);

    f32 corners[4] = {};
    for (u32 index = 0; index < 4; ++index) {
        const i64 cell_x = static_cast<i64>(u0) + static_cast<i64>(index & 1U);
        const i64 cell_z = static_cast<i64>(v0) + static_cast<i64>((index >> 1U) & 1U);
        const i64 cells = static_cast<i64>(pcg::kRegionCells);
        i64 region_x = cell_x / cells;
        i64 region_z = cell_z / cells;
        i64 local_x = cell_x % cells;
        i64 local_z = cell_z % cells;
        if (local_x < 0) {
            local_x += cells;
            --region_x;
        }
        if (local_z < 0) {
            local_z += cells;
            --region_z;
        }
        const pcg::RegionState* state = generated_.find(
            pcg::RegionCoord{static_cast<i32>(region_x), static_cast<i32>(region_z), 0});
        corners[index] = state == nullptr
                             ? 0.0F
                             : state->raster.at(elevation_channel_, static_cast<u32>(local_x),
                                                static_cast<u32>(local_z));
    }
    const f32 lower = corners[0] + ((corners[1] - corners[0]) * fu);
    const f32 upper = corners[2] + ((corners[3] - corners[2]) * fu);
    return lower + ((upper - lower) * fv);
}

// ================================================================================================
// SECTION 2 — TERRAIN
// ================================================================================================

Status World::cook_tile(const terrain::TileCoord& coord, BuildReport& report) noexcept {
    Expected<terrain::TerrainTile, Error> tile =
        terrain::TerrainTile::create(*allocator_, layout_, coord, 0.0F);
    if (!tile) {
        return make_unexpected(tile.error());
    }
    for (u32 j = 0; j < terrain::kTileVerts; ++j) {
        for (u32 i = 0; i < terrain::kTileVerts; ++i) {
            const terrain::TerrainPoint at = terrain::sample_position(layout_, coord, i, j);
            tile->set_stored(i, j, terrain::quantise_height(layout_, generated_height(at.x, at.z)));
        }
    }
    write_material(*tile);
    tile->refresh_extent(layout_);
    report.terrain_min_height =
        tile->min_height < report.terrain_min_height ? tile->min_height : report.terrain_min_height;
    report.terrain_max_height =
        tile->max_height > report.terrain_max_height ? tile->max_height : report.terrain_max_height;
    if (Status inserted = terrain_.insert(std::move(*tile)); !inserted) {
        return inserted;
    }
    ++report.terrain_tiles;
    return ok();
}

void World::write_material(terrain::TerrainTile& tile) const noexcept {
    // MATERIAL AND BIOME PER TEXEL, from the surface the generator produced. Layer 0 is rock, 1
    // sand, 2 grass and 3 the high ground; `SurfaceSample::layer` is read from here and from
    // nowhere else, so a texel a cook forgot would be visible rather than silently defaulted.
    for (u32 j = 0; j < terrain::kTileTexels; ++j) {
        for (u32 i = 0; i < terrain::kTileTexels; ++i) {
            const f32 height = terrain::dequantise_height(layout_, tile.stored(i, j));
            const f32 rise = terrain::dequantise_height(layout_, tile.stored(i + 1, j)) - height;
            const f32 run = terrain::dequantise_height(layout_, tile.stored(i, j + 1)) - height;
            const f32 slope = std::sqrt((rise * rise) + (run * run)) / layout_.sample_metres(0);
            terrain::MaterialTexel& texel = tile.texel(i, j);
            texel.layer[0] = terrain_layer_of(height, slope, static_cast<f32>(options_.sea_level));
            texel.weight[0] = 255;
            tile.biome[(j * terrain::kTileTexels) + i] = texel.layer[0];
        }
    }
}

Status World::derive_coarse_levels(i32 tiles_per_side, BuildReport& report) noexcept {
    // THE COARSE LEVELS ARE DERIVED, NOT AUTHORED — `terrain`'s own requirement, and a DECIMATION
    // rather than an average so a level transition has no seam for meshing to close.
    for (u8 level = 1; level < layout_.levels; ++level) {
        const i32 coarse_side = (tiles_per_side + (1 << level) - 1) >> level;
        for (i32 tz = 0; tz < coarse_side; ++tz) {
            for (i32 tx = 0; tx < coarse_side; ++tx) {
                const terrain::TileCoord coarse{tx, tz, level};
                const terrain::TerrainTile* children[4] = {};
                for (u32 index = 0; index < 4; ++index) {
                    const terrain::TileCoord child{(tx * 2) + static_cast<i32>(index & 1U),
                                                   (tz * 2) + static_cast<i32>((index >> 1U) & 1U),
                                                   static_cast<u8>(level - 1)};
                    children[index] = terrain_.find(child);
                }
                Expected<terrain::TerrainTile, Error> derived =
                    terrain::derive_coarse(*allocator_, layout_, coarse,
                                           Span<const terrain::TerrainTile* const>(children, 4));
                if (!derived) {
                    return make_unexpected(derived.error());
                }
                if (Status inserted = terrain_.insert(std::move(*derived)); !inserted) {
                    return inserted;
                }
                ++report.terrain_tiles;
            }
        }
    }
    return ok();
}

Status World::cook_terrain(BuildReport& report) noexcept {
    const f64 began = now_millis();
    if (!layout_.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "the terrain layout cannot name tiles");
    }

    const f64 span = static_cast<f64>(options_.region_edge) * kRegionMetres;
    const i32 tiles_per_side =
        static_cast<i32>(std::ceil(span / static_cast<f64>(layout_.tile_metres)));

    report.terrain_min_height = layout_.height_max;
    report.terrain_max_height = layout_.height_min;
    for (i32 tz = 0; tz < tiles_per_side; ++tz) {
        for (i32 tx = 0; tx < tiles_per_side; ++tx) {
            if (Status cooked = cook_tile(terrain::TileCoord{tx, tz, 0}, report); !cooked) {
                return cooked;
            }
        }
    }
    if (Status derived = derive_coarse_levels(tiles_per_side, report); !derived) {
        return derived;
    }

    if (Status added = query_.add_source(heights_); !added) {
        return added;
    }
    report.terrain_bytes = terrain_.bytes_resident();
    report.terrain_millis = now_millis() - began;
    return ok();
}

Status World::mesh_terrain(BuildReport& report) noexcept {
    const f64 began = now_millis();
    Array<terrain::TileCoord> level_zero(*allocator_);
    if (Status listed = terrain_.tiles_at_level(0, level_zero); !listed) {
        return listed;
    }
    for (const terrain::TileCoord& coord : level_zero.span()) {
        terrain::MeshOptions options;
        // Every neighbour is at the same level, because this artefact meshes the whole world at
        // level 0 — there is no streaming binder in the tree to move a tile to a coarser level, and
        // src/terrain/'s README records that gap against its own row. `stitched_vertices` is
        // therefore expected to be zero and is REPORTED rather than assumed, so the day a streamer
        // exists the number moves and says so.
        for (u8& neighbour : options.neighbour_level) {
            neighbour = coord.level;
        }
        terrain::MeshReport mesh_report;
        Expected<terrain::TerrainMesh, Error> mesh = terrain::mesh_tile(
            *allocator_, terrain_, heights_, &deltas_, coord, options, mesh_report);
        if (!mesh) {
            return make_unexpected(mesh.error());
        }
        TerrainPatch patch(*allocator_);
        patch.origin = WorldVec3d{mesh->bounds.min_x, 0.0, mesh->bounds.min_z};
        patch.mesh = std::move(*mesh);
        if (Status sized = patch.colours.resize(patch.mesh.positions.size()); !sized) {
            return sized;
        }
        report.terrain_triangles += mesh_report.triangles;
        report.terrain_stitched_vertices += mesh_report.stitched_vertices;
        if (Status pushed = patches_.push_back(std::move(patch)); !pushed) {
            return pushed;
        }
    }
    report.terrain_millis += now_millis() - began;
    return ok();
}

// ================================================================================================
// SECTION 1 — THE SUBSTRATE, AND WHO CLAIMS WHAT
// ================================================================================================

Status World::declare_fields(BuildReport& report) noexcept {
    // TERRAIN IS A PRODUCER. `register_producer()` declares `soil` and claims it through
    // `FieldRegistry::claim()`; a second terrain doing the same would fail here with both producers
    // named. The claim is kept by the system, so a writer that ignored the refusal could not be
    // spelled.
    if (Status claimed = terrain_system_.register_producer(registry_, "cy::terrain", 8.0F);
        !claimed) {
        return claimed;
    }
    ++report.fields_claimed;
    return ok();
}

// ================================================================================================
// SECTION 2.3 — WATER
// ================================================================================================

f64 World::bed_at(void* user, f64 x, f64 z) noexcept {
    const auto* self = static_cast<const World*>(user);
    const terrain::SurfaceSample sample = self->query_.sample(x, z);
    return sample.resolved ? static_cast<f64>(sample.height)
                           : static_cast<f64>(self->layout_.height_min);
}

Status World::configure_water(BuildReport& report) noexcept {
    water::WaterBodyDesc sea;
    sea.name = "world.sea";
    sea.type = water::WaterBodyType::Ocean;
    sea.backend = water::WaterBackend::Spectral;
    sea.mean_level = options_.sea_level;
    const f64 span = static_cast<f64>(options_.region_edge) * kRegionMetres;
    // The ocean's bounds reach a long way past the generated land, because the horizon of a coastal
    // world is water and a body that stopped at the terrain's edge would put a visible seam there.
    sea.bounds.min_x = -span * 4.0;
    sea.bounds.min_z = -span * 4.0;
    sea.bounds.max_x = span * 5.0;
    sea.bounds.max_z = span * 5.0;
    sea.bounds.min_y = static_cast<f64>(layout_.height_min);
    sea.bounds.max_y = options_.sea_level + 40.0;
    sea.priority = 0;
    sea.density = 1025.0F;
    sea.segment_metres = 256.0F;

    Expected<water::WaterBodyId, Error> added = water_.registry().add(sea);
    if (!added) {
        return make_unexpected(added.error());
    }
    sea_ = *added;

    ocean_params_ = water::OceanParams{};
    ocean_params_.wind_speed_mps = 9.5F;
    ocean_params_.wind_direction_degrees = 20.0F;
    ocean_params_.fetch_km = 240.0F;
    ocean_params_.sea_level = options_.sea_level;
    ocean_params_.choppiness = 0.6F;
    water::OceanReport ocean_report;
    if (Status set = water_.set_ocean(sea_, ocean_params_, options_.seed, ocean_report); !set) {
        return set;
    }
    report.sea_significant_height = ocean_report.significant_height_metres;
    report.sea_peak_wavelength = ocean_report.peak_wavelength_metres;
    report.sea_cascades = ocean_report.cascades;
    report.sea_authoritative_bands = ocean_report.split.authoritative_bands;
    report.sea_visual_bands = ocean_report.split.visual_bands;

    water_.set_bed_source(&World::bed_at, this);

    // FOAM IS PERSISTENT STATE, so its grid has to exist before the first tick advects it. 256
    // cells of two metres covers a five-hundred-metre square around the camera, which is what the
    // shoreline this camera looks at actually spans — `water` scopes foam to regions near a
    // streaming source and `FoamField` centres on one focus, and src/water/'s README records that
    // one focus is a limit rather than a design for a world with two distant players.
    // 128 cells of four metres is a five-hundred-metre square around the camera. The resolution is
    // the one lever that matters here and it is quadratic: the advection and decay pass touches
    // every cell every tick, and at 256 cells it was measured at 47 ms a frame — the single largest
    // producer cost in the whole world, for a quantity visible on a few hundred metres of shore.
    // The figure `just capture-world` writes is where that was read off.
    water::FoamParams foam;
    foam.resolution = 128;
    foam.cell_metres = 4.0F;
    foam.lifetime_seconds = 9.0F;
    if (Status configured = water_.foam().configure(foam); !configured) {
        return configured;
    }

    water::OceanSurfaceParams surface;
    surface.near_cell_metres = 3.0F;
    surface.ring_quads = 32;
    surface.rings = 6;
    if (Status configured = ocean_.configure(surface); !configured) {
        return configured;
    }

    // WETNESS HAS TWO POSSIBLE PRODUCERS AND THE SUBSTRATE ALLOWS ONE, so this world DECLARES which
    // row owns it rather than discovering the conflict at startup. Water publishes
    // `water-shore-wetness`; weather claims `wetness` and composes the shore's contribution into it
    // (`weather::WetnessSource::ComposeShore` below). Leaving both at their defaults is a
    // configuration that fails at `claim()` with the substrate's own refusal naming both producers
    // — which is the design working, and is why this is two lines and not a workaround.
    water::WaterFieldOptions options;
    options.wetness_owner = water::WetnessOwner::External;
    if (Status declared = water_.fields().declare(registry_, options); !declared) {
        return declared;
    }
    if (Status claimed = water_.fields().claim(registry_, fields_); !claimed) {
        return claimed;
    }
    report.fields_claimed += 4;
    return publish_shoreline();
}

Status World::publish_shoreline() noexcept {
    const f64 span = static_cast<f64>(options_.region_edge) * kRegionMetres;
    const water::ShorelineInputs inputs = water_.shoreline_inputs();
    if (Status published = water_.fields().publish(inputs, environment::FieldResidency::Macro, 0.0,
                                                   0.0, span, span);
        !published) {
        return published;
    }
    return ok();
}

// ================================================================================================
// SECTION 3 — WEATHER
// ================================================================================================

f64 World::elevation_at(void* user, f64 x, f64 z) noexcept {
    return bed_at(user, x, z);
}

Status World::configure_weather(BuildReport& report) noexcept {
    if (Status uniform = climate_.set_uniform(world_climate()); !uniform) {
        return uniform;
    }

    const f64 span = static_cast<f64>(options_.region_edge) * kRegionMetres;
    weather::WeatherConfig config;
    // The weather grid DELIBERATELY DOES NOT ALIGN with the world partition: 400 m cells at a prime
    // origin against 128 m world cells, so every weather cell boundary falls inside a world cell.
    // `weather-and-wind` requires exactly that to be legal, and a world that quietly aligned them
    // would be a world in which the requirement was never exercised.
    config.grid.origin_x = -211.0;
    config.grid.origin_z = -307.0;
    config.grid.regional_cell_metres = 400.0F;
    config.grid.width = 12;
    config.grid.height = 12;
    config.grid.refine_ratio = 4;
    config.grid.max_local_cells = 256;
    config.grid.macro_step_seconds = 30.0F;
    config.grid.relaxation_per_second = 0.02F;
    config.fields.wetness = weather::WetnessSource::ComposeShore;
    config.fields.local_cell_metres = 32.0F;
    config.fields.regional_cell_metres = 128.0F;
    config.fields.macro_cell_metres = 512.0F;
    config.fields.wind_vertical_cells = 4;
    config.fields.wind_vertical_metres = 24.0F;
    config.seed = options_.seed ^ 0x5745'4154'4845'5200ULL;
    // A tick of this program is one frame, and a frame is a real fraction of a simulated day —
    // `advance()` passes the elapsed simulated seconds, so the rate has to be the one the director
    // and the cells agree on. Declaring it honestly is the fix; a mismatch here is what makes a
    // storm preset apply and no rain fall.
    // ONE TICK IS ONE FRAME OF THE TAKE, AND A TAKE IS ONE SIMULATED DAY. A frame therefore
    // advances the weather by 86 400 / (seconds_per_day * fps) simulated seconds, and the rate
    // below is its reciprocal — declared honestly, because a director that believed a tick was a
    // sixtieth of a second while the cells advanced two simulated minutes would run transitions
    // seven thousand times slower than the weather they drive. src/weather/tests/fixtures.h records
    // exactly that mismatch as the bug that made a storm preset apply and no rain fall.
    config.ticks_per_second = 1.0 / simulated_seconds_per_frame();
    config.cloud_reference_altitude_metres = 2200.0F;
    if (Status configured = weather_.configure(config, climate_); !configured) {
        return configured;
    }

    weather::TerrainProfile profile;
    profile.elevation_at = &World::elevation_at;
    profile.user = this;
    weather_.set_terrain(profile);

    if (Status bound = weather_.bind_fields(registry_, fields_); !bound) {
        return bound;
    }
    weather::PublishRegion region;
    region.min_x = 0.0;
    region.min_z = 0.0;
    region.max_x = span;
    region.max_z = span;
    region.level = environment::FieldResidency::Macro;
    weather_.set_publish_region(region);

    // A FRONT COMES THROUGH, AND IT IS SCHEDULED RATHER THAN TRIGGERED. A day of unchanging
    // weather would show none of what section 3 built. `set_immediate()` is what a level load does;
    // the two scheduled presets go through `apply()`, which is the module's ONE entry point for a
    // weather change — so a designed sequence is reproducible, which is what makes the video the
    // same take every time it is run.
    //
    // The tick numbers are frames, so they are fractions of the day whatever `--seconds` and
    // `--fps` are set to: the front arrives about a third of the way through the take and clears
    // about two thirds through.
    weather_.director().set_immediate(weather::preset_clear().target, 0);
    const u64 ticks_per_day = frames_per_day();
    if (Status scheduled = weather_.director().schedule(
            weather::preset_snowstorm(), weather::TransitionProfile::standard(), ticks_per_day / 3);
        !scheduled) {
        return scheduled;
    }
    if (Status scheduled = weather_.director().schedule(weather::preset_clear(),
                                                        weather::TransitionProfile::standard(),
                                                        (ticks_per_day * 2) / 3);
        !scheduled) {
        return scheduled;
    }

    wetness_field_ = environment::field_id(environment::fields::kWetness);
    snow_field_ = environment::field_id(environment::fields::kSnowDepth);
    wind_field_ = environment::field_id(environment::fields::kWind);
    vegetation_field_ = environment::field_id(weather::fields::kVegetationDensity);
    water_depth_field_ = environment::field_id(environment::fields::kWaterDepth);
    water_distance_field_ = environment::field_id(environment::fields::kWaterDistance);
    report.fields_claimed += 14;
    return ok();
}

f64 World::simulated_seconds_per_frame() const noexcept {
    const f64 frames =
        static_cast<f64>(options_.seconds_per_day) * static_cast<f64>(options_.take_fps);
    return frames > 0.0 ? 86'400.0 / frames : 1.0;
}

u64 World::frames_per_day() const noexcept {
    const f64 frames =
        static_cast<f64>(options_.seconds_per_day) * static_cast<f64>(options_.take_fps);
    return frames > 1.0 ? static_cast<u64>(frames) : 1ULL;
}

// ================================================================================================
// SECTION 2.4 — FOLIAGE
// ================================================================================================

Status World::place_foliage(BuildReport& report) noexcept {
    const f64 began = now_millis();
    if (Status declared = species_.declare(pine()); !declared) {
        return declared;
    }
    if (Status declared = species_.declare(birch()); !declared) {
        return declared;
    }
    if (Status declared = species_.declare(scrub()); !declared) {
        return declared;
    }
    report.foliage_species = static_cast<u32>(species_.size());
    if (Status authored = author_rules(rules_, static_cast<f32>(options_.sea_level)); !authored) {
        return authored;
    }

    // THE BINDINGS ARE A DECLARATION, NOT A CONSTANT. `FieldBindings::standard()` names the
    // engine's `vegetation`; weather produces `vegetation-density`, which is the same quantity
    // under this world's producers. A project that renamed a field changes exactly this line, and
    // `generate_region()` records the bindings it read in the provenance — so an invalidation is
    // told the fields that were actually sampled rather than the standard set.
    bindings_ = foliage::FieldBindings::standard();
    bindings_.vegetation = vegetation_field_;

    // Placement's sun is DECLARED rather than sampled, so the forest is the same at midnight as at
    // noon. A generated world whose trees depended on the time of day would not be a generated
    // world at all.
    if (Status pushed = sampler_.push_back(foliage::PlacementSampler(
            query_, fields_, bindings_, normalize(Vec3{0.35F, -0.88F, 0.32F})));
        !pushed) {
        return pushed;
    }

    foliage::GenerationContext context;
    context.seed = options_.seed;
    context.library = &species_;
    context.rules = &rules_;
    context.sampler = sampler_.data();
    context.policy.edge_metres = 64.0F;
    context.policy.target_instances = 4096;
    context.policy.max_instances = 16384;
    // OFF, AND THAT IS THE COOK'S ANSWER RATHER THAN A WORKAROUND. The flag scales density by the
    // region's CURRENT ecosystem state; this placement runs before the first weather tick, when
    // `vegetation-density` is still its declared default everywhere, so leaving it on would scale
    // every region by the same number and produce an empty world. `foliage`'s own wording is that
    // it is "off in a cook that wants the potential vegetation rather than the current one", and
    // this is a cook.
    context.apply_ecosystem_state = false;

    const i32 edge =
        options_.foliage_edge < options_.region_edge ? options_.foliage_edge : options_.region_edge;
    for (i32 z = 0; z < edge; ++z) {
        for (i32 x = 0; x < edge; ++x) {
            const foliage::ClusterCoord region{x, z, 0};
            Expected<foliage::FoliagePopulation, Error> population =
                foliage::generate_region(*allocator_, context, region);
            if (!population) {
                return make_unexpected(population.error());
            }
            if (population->cluster.size() == 0) {
                continue;
            }
            report.foliage_instances += population->cluster.size();
            report.foliage_bytes += population->cluster.bytes();
            if (Status pushed = clusters_.push_back(std::move(population->cluster)); !pushed) {
                return pushed;
            }
            ++report.foliage_clusters;
        }
    }

    // THE WIND READER, OPENED ONCE. `WindSampler::open()` makes the firewall decision
    // (`determinism::may_read()`) for the whole system rather than per sample, and foliage declares
    // itself PRESENTATION — which is what makes reading the presentation half of the wind legal.
    Expected<foliage::WindSampler, Error> wind = foliage::WindSampler::open(
        fields_, wind_field_, determinism::SimulationClass::Presentation);
    if (!wind) {
        return make_unexpected(wind.error());
    }
    if (Status pushed = wind_sampler_.push_back(*wind); !pushed) {
        return pushed;
    }
    report.foliage_millis = now_millis() - began;
    return ok();
}

// ================================================================================================
// SECTION 3.3 — THE SKY
// ================================================================================================

Status World::configure_sky(BuildReport& report) noexcept {
    const f64 began = now_millis();
    atmosphere_ = sky::Atmosphere{};
    // `Medium` tables: 64x32 transmittance and the second-order multiple-scattering table over it.
    // The multiple-scattering half is what stops the sky glowing at midnight and what carries a
    // third of a horizon's daytime radiance — src/rendering/sky/'s own A/B measures both.
    if (Status configured = tables_.configure(sky::SkyTableQuality::Medium); !configured) {
        return configured;
    }
    Expected<bool, Error> built = tables_.build(atmosphere_);
    if (!built) {
        return make_unexpected(built.error());
    }
    report.sky_directions_integrated = tables_.directions_integrated();

    celestial_model_ = sky::CelestialModel{};
    celestial_model_.latitude_degrees = 52.0F;
    celestial_model_.axial_tilt_degrees = 23.44F;

    if (Status generated = sky::generate_stars(stars_, options_.seed ^ 0x57A'25EEDULL, 2000);
        !generated) {
        return generated;
    }

    // A cloud weather map over KILOMETRES, refused below 250 m per cell — a finer map is a stored
    // volume by another name, and `configure()` says so rather than allowing it. Forty cells of a
    // kilometre covers a world four hundred times this one's width, which is what a cloud deck
    // seen from the ground actually spans.
    if (Status configured = cloud_map_.configure(40, 1'000.0F); !configured) {
        return configured;
    }
    if (Status generated = cloud_map_.generate(options_.seed ^ 0xC10'D5EULL, 0.45F, 0.3F);
        !generated) {
        return generated;
    }
    cloud_layers_ = sky::default_cloud_layers();
    cloud_field_ = sky::CloudField{};
    cloud_field_.map = &cloud_map_;
    cloud_field_.layers = cloud_layers_;
    cloud_field_.seed = options_.seed ^ 0xC10'D5EULL;

    cloud_quality_ = sky::CloudQuality{};
    cloud_quality_.steps = options_.cloud_steps;
    cloud_quality_.light_steps = 4;
    cloud_quality_.octaves = 3;
    cloud_quality_.multiple_scattering = true;

    report.sky_tables_millis = now_millis() - began;
    return ok();
}

Status World::build_sky_dome() noexcept {
    // A DOME OF DIRECTIONS, not a cube map and not a full-screen march. Every vertex is one call to
    // `sky::compose_sky()` — the engine's own atmosphere, its own cloud march, its own stars — and
    // the rasteriser interpolates between them. That is the honest description of this picture's
    // sky: the MODEL is the shipped one and the RESOLUTION is a dome, because M10 shipped no sky
    // shader and a per-pixel march on the processor would cost minutes a frame.
    const u32 rings = options_.sky_rings;
    const u32 segments = options_.sky_segments;
    if (Status sized = sky_dome_.resize(static_cast<usize>(rings + 1) * segments); !sized) {
        return sized;
    }
    for (u32 ring = 0; ring <= rings; ++ring) {
        // The rings are packed toward the horizon, where the atmosphere's gradient is steepest and
        // a uniform dome bands visibly. `t^1.7` puts about half the rings in the bottom third.
        const f32 t = static_cast<f32>(ring) / static_cast<f32>(rings);
        // From a little above the zenith down to eighteen degrees BELOW the horizon, so the dome
        // still covers the pixels a camera looking downhill puts below it. A dome that stopped at
        // the horizon would leave the clear colour showing under the sea.
        const f32 elevation = ((1.0F - std::pow(t, 1.7F)) * 1.8850F) - 0.3140F;
        for (u32 segment = 0; segment < segments; ++segment) {
            const f32 azimuth = 6.28318F * static_cast<f32>(segment) / static_cast<f32>(segments);
            SkyVertex& vertex = sky_dome_[(static_cast<usize>(ring) * segments) + segment];
            vertex.direction = Vec3{std::cos(elevation) * std::cos(azimuth), std::sin(elevation),
                                    std::cos(elevation) * std::sin(azimuth)};
        }
    }
    sky_indices_.clear();
    for (u32 ring = 0; ring < rings; ++ring) {
        for (u32 segment = 0; segment < segments; ++segment) {
            const u32 next = (segment + 1) % segments;
            const u32 a = (ring * segments) + segment;
            const u32 b = (ring * segments) + next;
            const u32 c = ((ring + 1) * segments) + segment;
            const u32 d = ((ring + 1) * segments) + next;
            const u32 order[6] = {a, c, b, b, c, d};
            for (const u32 index : order) {
                if (Status pushed = sky_indices_.push_back(index); !pushed) {
                    return pushed;
                }
            }
        }
    }
    return ok();
}

// ================================================================================================
// ONE FRAME
// ================================================================================================

Status World::advance(FrameCosts& costs) noexcept {
    const f64 step = simulated_seconds_per_frame();
    ++tick_;
    seconds_ += step;

    // The camera is where the ocean patch is centred, where the wind is prepared from and where the
    // sky is composed from, so it is resolved before anything reads it rather than passed into four
    // calls that could each be given a different one.
    WorldVec3d target;
    camera_at(tick_, frames_per_day(), camera_focus_, target);

    // --- WEATHER FIRST, because everything below reads what it publishes.
    f64 mark = now_millis();
    determinism::SimulationPoint at;
    at.tick = tick_;
    Expected<weather::WeatherTickReport, Error> ticked = weather_.advance(at, step);
    if (!ticked) {
        return make_unexpected(ticked.error());
    }
    const f64 after_weather = now_millis();
    costs.weather_ms = after_weather - mark;
    // The publication is inside `advance()` and cannot be timed apart from it without a second
    // entry point weather does not have, so the field half is REPORTED AS ITS OWN NUMBER from the
    // tick report's own count rather than from a clock that cannot see it.
    costs.fields_ms = 0.0;
    const u64 published = ticked->atmosphere.lattice_points;

    const WorldVec3d middle = centre();
    const weather::EnvironmentSample sample = weather_.sample(
        middle, weather::SampleQuality::Gameplay, determinism::SimulationClass::Persistent);

    // --- WATER, DRIVEN BY THE WIND FIELD WEATHER JUST WROTE. `drive_ocean_from_wind()` re-derives
    // the spectrum from a wind vector, so a gust raises every band and lengthens the peak with
    // nothing re-authored. This is the weather-to-water seam and it is one call.
    mark = now_millis();
    if (Status driven = water_.drive_ocean_from_wind(sea_, sample.wind.authoritative()); !driven) {
        return driven;
    }
    if (Status ticked_water = water_.tick(static_cast<f32>(step), camera_focus_); !ticked_water) {
        return ticked_water;
    }
    const water::DisplacementModel* model = water_.model_of(sea_);
    if (model == nullptr) {
        return fail(ErrorCode::Internal, "the sea lost its displacement model");
    }
    costs.water_ms = now_millis() - mark;
    mark = now_millis();
    if (Status rebuilt = ocean_.build(*model, camera_focus_, water_.time()); !rebuilt) {
        return rebuilt;
    }
    costs.ocean_ms = now_millis() - mark;

    // --- THE SKY. Weather's `CloudDrive` becomes the sky's `CloudWeatherState`; nothing else
    // authors a cloud layer. The two structs are the same six quantities in the same units, and the
    // assignment below is the whole of the seam — `cy::weather` deliberately does not link
    // `cy::rendering-sky`, so this three-line adapter is the artefact's to write.
    mark = now_millis();
    const weather::CloudDrive drive = weather_.cloud_drive(middle);
    sky::CloudWeatherState cloud_state;
    cloud_state.wind = drive.wind;
    cloud_state.shear_per_km = drive.shear_per_km;
    cloud_state.humidity = drive.humidity;
    cloud_state.storm_intensity = drive.storm_intensity;
    cloud_state.precipitation = drive.precipitation_mm_per_hour;
    cloud_state.temperature_celsius = drive.temperature_celsius;
    cloud_state.epoch = drive.epoch;
    cloud_layers_ = sky::default_cloud_layers();
    sky::drive_cloud_layers(cloud_state, cloud_layers_);
    cloud_field_.layers = cloud_layers_;

    sky::TimeOfDay time_of_day;
    time_of_day.seconds_per_day = options_.seconds_per_day;
    const f64 start = static_cast<f64>(options_.day_start);
    time_of_day.fraction = static_cast<f32>(std::fmod(start + (seconds_ / 86'400.0), 1.0));
    time_of_day.day_of_year = 300.0F;
    celestial_ = sky::solve_celestial(celestial_model_, time_of_day);
    state_.day_fraction = time_of_day.fraction;

    if (Status shaded = shade_sky(); !shaded) {
        return shaded;
    }
    costs.sky_ms = now_millis() - mark;

    mark = now_millis();
    if (Status shaded = shade_terrain(); !shaded) {
        return shaded;
    }
    costs.terrain_shade_ms = now_millis() - mark;

    mark = now_millis();
    if (Status updated = update_plants(); !updated) {
        return updated;
    }
    costs.foliage_ms = now_millis() - mark;

    // --- The state the caption is written from, all of it read back off a producer.
    state_.seconds = seconds_;
    state_.sun_elevation_degrees = std::asin(celestial_.sun.direction.y) * 57.2958F;
    state_.temperature_celsius = sample.temperature_celsius;
    state_.wind_speed_mps = sample.wind.speed();
    state_.precipitation_mm_per_hour = sample.precipitation_mm_per_hour;
    state_.cloud_coverage = sample.cloud_coverage;
    state_.precipitation = weather::precipitation_type_name(sample.precipitation_type);
    state_.significant_wave_height = 0.0F;
    for (u32 band = 0; band < model->band_count; ++band) {
        state_.significant_wave_height += model->bands[band].amplitude;
    }
    const environment::FieldSample wetness = fields_.sample_deterministic(wetness_field_, middle);
    const environment::FieldSample snow = fields_.sample_deterministic(snow_field_, middle);
    const environment::FieldSample west =
        fields_.sample_deterministic(snow_field_, WorldVec3d{middle.x - 600.0, 0.0, middle.z});
    state_.snow_depth_west_metres = west.value.x();
    state_.wetness = wetness.value.x();
    state_.cloud_transmittance = lighting_.cloud_transmittance;
    state_.exposure = lighting_.exposure;
    state_.star_visibility = celestial_.star_visibility;
    state_.snow_depth_metres = snow.value.x();
    (void)published;
    return ok();
}

Status World::shade_sky() noexcept {
    sky::SkyCompositionInputs inputs;
    inputs.atmosphere = &atmosphere_;
    inputs.tables = &tables_;
    inputs.celestial = &celestial_;
    // NO STARS IN THE DOME. See `StarDraw`: they are drawn as the points they are, because a dome
    // affordable enough to march clouds through is four degrees of sky per quad and would turn a
    // star into a lozenge. The LIGHTING integral below keeps them.
    inputs.stars = nullptr;
    inputs.clouds = &cloud_field_;
    inputs.cloud_quality = cloud_quality_;
    inputs.time_seconds = seconds_;
    inputs.view = sky::planetary_view(atmosphere_, camera_focus_);

    Vec3 mean{0.0F, 0.0F, 0.0F};
    state_.thickest_cloud = 1.0F;
    for (SkyVertex& vertex : sky_dome_.span()) {
        const sky::SkyCompositionSample composed = sky::compose_sky(inputs, vertex.direction);
        vertex.radiance = composed.radiance;
        mean = mean + composed.radiance;
        state_.thickest_cloud = composed.cloud_transmittance < state_.thickest_cloud
                                    ? composed.cloud_transmittance
                                    : state_.thickest_cloud;
    }
    const f32 count = sky_dome_.empty() ? 1.0F : static_cast<f32>(sky_dome_.size());
    mean = Vec3{mean.x / count, mean.y / count, mean.z / count};

    // THE LIGHT EVERYTHING ELSE IS SHADED BY comes from the same composition the background does,
    // which is what stops a scene lit for a clear noon from being drawn under a storm. The stars
    // ARE in this one: starlight is what lights the ground at midnight, and an integral that left
    // them out would make a moonless night exactly black rather than nearly so.
    sky::SkyCompositionInputs lit = inputs;
    lit.stars = &stars_;
    const sky::SkyLighting lighting = sky::compose_sky_lighting(lit, 12);
    lighting_.sun_travel = sky::light_travel_direction(celestial_.sun);
    lighting_.cloud_transmittance = lighting.cloud_transmittance;

    // EXPOSURE FROM THE SKY'S OWN MEAN RADIANCE — an auto-exposure, and a crude one: this artefact
    // does not link `cy::rendering-post`, whose real one this is standing in for. It is stated
    // rather than hidden because it is what makes the same tone curve work at noon and at dusk.
    //
    // THE FLOOR IS WHAT MAKES NIGHT A NIGHT. Measured on this world: noon is 1 780 nits of mean sky
    // and a moonless midnight is 0.00038 — a ratio of four and a half million, and there is no moon
    // in `sky::compose_sky()` to soften it (`celestial.h` models one; the composition does not
    // consume it, which is a real absence in that row rather than a setting here). A fully adapting
    // exposure would expose midnight exactly like noon; without any floor at all it would divide by
    // the numerical floor and amplify it into noise. Six thousandths lands the night sky around a
    // sixth of white with the stars above it and the ground a silhouette, which is what a dark-
    // adapted eye sees.
    // The exposure is derived from the LIGHTING integral's mean rather than from the dome's own,
    // so that it and the ambient below are one number seen twice — and so that the stars, which the
    // dome does not carry, still count toward how dark a midnight is.
    const Vec3 lit_mean = lighting.mean_sky_radiance;
    const f32 luminance = (lit_mean.x * 0.2126F) + (lit_mean.y * 0.7152F) + (lit_mean.z * 0.0722F);
    state_.mean_sky_nits = luminance;
    lighting_.exposure = luminance > kExposureFloor ? luminance : kExposureFloor;

    // THE THREE DIVISORS ARE ONE EXPOSURE AND TWO RATIOS, and the ratios are physical rather than
    // chosen. The sky's irradiance on a flat surface is pi times its mean radiance, so a sun of
    // about a hundred thousand lux against a mean sky of a few thousand nits is roughly ten times
    // the sky's own contribution — which is what 30 against 15 reproduces. The absolute level (the
    // 4 that the sky itself is divided by) is the only free number, and it is set so that ground of
    // 0.2 albedo under a noon sun lands near the middle of the tone curve rather than at its top.
    const f32 sun_scale = 1.0F / (lighting_.exposure * 30.0F);
    lighting_.sun_colour =
        Vec3{lighting.sun_illuminance.x * sun_scale, lighting.sun_illuminance.y * sun_scale,
             lighting.sun_illuminance.z * sun_scale};
    const f32 ambient_scale = 1.0F / (lighting_.exposure * 15.0F);
    lighting_.ambient =
        Vec3{lit_mean.x * ambient_scale, lit_mean.y * ambient_scale, lit_mean.z * ambient_scale};

    // THE STARS, as points. `StarField` is the engine's own procedural catalogue drawn from the
    // world seed through `determinism::RandomStream`, with the real magnitude distribution — faint
    // stars vastly outnumber bright ones — and `star_visibility` is the celestial model's own
    // number, zero in daylight and one well after dusk.
    star_draws_.clear();
    if (celestial_.star_visibility > 0.01F) {
        // A STAR'S BRIGHTNESS IS NORMALISED RATHER THAN EXPOSED, and that is a deliberate
        // departure from the rest of this frame. `Star::illuminance` is the real thing — lux at the
        // top of the atmosphere, about 1e-5 for Sirius and 1e-8 for the faintest a dark-adapted eye
        // resolves — and a star subtends an angle thousands of times smaller than a pixel of this
        // picture, so its radiance is not a number any renderer can put through an exposure. What
        // is drawn is the MAGNITUDE RELATION: brightest at one, and eight stops of range below it.
        const f32 brightest = 1.0e-5F;
        for (const sky::Star& star : stars_.stars.span()) {
            const f32 relative = clamp01((star.illuminance / brightest) * stars_.intensity) *
                                 celestial_.star_visibility;
            StarDraw drawn;
            drawn.direction = star.direction;
            drawn.radiance =
                Vec3{star.tint.x * relative, star.tint.y * relative, star.tint.z * relative};
            drawn.angular_size = 0.00045F + (0.0011F * relative);
            if (Status pushed = star_draws_.push_back(drawn); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status World::shade_terrain() noexcept {
    // THE CONSUMER SIDE OF THE SUBSTRATE, and the reason this loop exists at all. Every vertex
    // colour is four `sample_deterministic()` calls — soil, wetness, snow and water depth — against
    // fields four different producers claimed. Nothing here knows which module wrote which; that is
    // the whole point of a substrate, and it is why a snowfall turns the ground white without
    // weather and terrain having been introduced.
    //
    // `sample_deterministic()` and NEVER `sample()`: the finest-resident path's answer depends on
    // what streamed, and a world in which the renderer and gameplay disagreed about how wet the
    // ground is would be exactly the defect `environment-fields` forbids.
    const Vec3 rock{0.33F, 0.31F, 0.30F};
    const Vec3 sand{0.62F, 0.56F, 0.42F};
    const Vec3 grass{0.24F, 0.34F, 0.17F};
    const Vec3 alpine{0.44F, 0.43F, 0.40F};
    const Vec3 snow_colour{0.86F, 0.88F, 0.92F};

    for (TerrainPatch& patch : patches_.span()) {
        const usize count = patch.mesh.positions.size();
        for (usize index = 0; index < count; ++index) {
            const Vec3& local = patch.mesh.positions[index];
            const WorldVec3d at{patch.origin.x + static_cast<f64>(local.x),
                                static_cast<f64>(local.y),
                                patch.origin.z + static_cast<f64>(local.z)};
            const f32 slope = 1.0F - clamp01(patch.mesh.normals[index].y);
            // THE BEACH IS THE WATER-DISTANCE FIELD, not a height band. `cy::water`'s shoreline
            // publishes `water-distance` — a two-pass chamfer transform whose answer is the
            // distance from whichever side the water is on — and reading it here is what makes the
            // sand follow the coastline into every inlet instead of following a contour line. It is
            // also the clearest thing in this artefact that the substrate is load-bearing: delete
            // water's producer and the beaches disappear.
            const environment::FieldSample shore =
                fields_.sample_deterministic(water_distance_field_, at);
            Vec3 base = grass;
            if (shore.resolved && shore.value.x() < 14.0F) {
                base = mix(sand, grass, clamp01((shore.value.x() - 4.0F) / 10.0F));
            } else if (slope > 0.45F) {
                base = rock;
            } else if (local.y > kAlpineMetres) {
                base = alpine;
            }
            const environment::FieldSample wetness =
                fields_.sample_deterministic(wetness_field_, at);
            const environment::FieldSample snow = fields_.sample_deterministic(snow_field_, at);
            const environment::FieldSample vegetation =
                fields_.sample_deterministic(vegetation_field_, at);
            // Wet ground is darker and a little more saturated; that is what wetness DOES to a
            // surface, and it is the one place this artefact interprets a field rather than
            // reporting it.
            const f32 wet = clamp01(wetness.value.x());
            Vec3 shaded = Vec3{base.x * (1.0F - (wet * 0.45F)), base.y * (1.0F - (wet * 0.42F)),
                               base.z * (1.0F - (wet * 0.30F))};
            shaded = mix(shaded, mix(shaded, grass, 0.5F), clamp01(vegetation.value.x()) * 0.4F);
            // Snow COVERS rather than tints, and it covers the flat before the steep.
            const f32 depth = snow.value.x();
            // Snow LIES rather than tints: it covers the flat before the steep and it takes a
            // real depth to cover at all, so a few centimetres dusts and half a metre buries.
            const f32 cover = clamp01(depth * 1.2F) * clamp01(1.3F - (slope * 2.6F));
            patch.colours[index] = mix(shaded, snow_colour, cover);
        }
    }
    return ok();
}

Status World::update_plants() noexcept {
    // ONE FIELD SAMPLE PER CLUSTER, NOT PER TREE. `WindSampler::prepare()` writes a `ClusterWind`
    // for each cluster and `WindPrepareReport` counts the samples it took; a forest does not need a
    // wind sample per plant and taking one would be exactly the per-instance processor work
    // `foliage` forbids.
    foliage::WindTuning tuning;
    tuning.hierarchical_metres = 90.0F;
    tuning.sway_metres = 400.0F;
    tuning.amplitude_scale = 1.0F;
    Expected<foliage::WindPrepareReport, Error> prepared =
        wind_sampler_[0].prepare(species_, clusters_.span(), camera_focus_, tuning);
    if (!prepared) {
        return make_unexpected(prepared.error());
    }

    plants_.clear();
    const f32 time = static_cast<f32>(seconds_);
    // The vertex at the top of the crown: the parameterisation a vertex shader would carry in its
    // own stream. `evaluate_response()` is a pure function of these five arguments and is the
    // shipped one — what this artefact does not do is evaluate it on a device, which is the gap
    // src/foliage/'s README records against its own row.
    foliage::VertexParameters crown;
    crown.height_fraction = 1.0F;
    crown.radial_fraction = 0.6F;

    // The two populations, drawn by one loop: the three species the PLACEMENT RULES put there, and
    // the boulders the GENERATOR's own accepted points became through the output adapter. They are
    // both `foliage::FoliageCluster`s and nothing downstream can tell them apart, which is the
    // point of the adapter.
    for (u32 population = 0; population < 2; ++population) {
        const Span<const foliage::FoliageCluster> source =
            population == 0 ? clusters_.span() : generated_clusters_.clusters();
        for (const foliage::FoliageCluster& cluster : source) {
            const foliage::ClusterWind& wind = cluster.wind();
            for (const foliage::SpeciesBlock& block : cluster.blocks()) {
                const foliage::SpeciesDeclaration* species = species_.find(block.species);
                if (species == nullptr) {
                    continue;
                }
                for (u32 offset = 0; offset < block.count; ++offset) {
                    const u32 slot = block.first + offset;
                    const foliage::FoliageInstance* instance = cluster.at(slot);
                    if (instance == nullptr) {
                        continue;
                    }
                    const WorldVec3d position = cluster.bounds().decode(*instance);
                    crown.vertex_phase = static_cast<f32>(slot & 0x3FU) / 64.0F;
                    const foliage::WindDisplacement response =
                        foliage::evaluate_response(*species, wind, *instance, crown, time);
                    PlantDraw plant;
                    plant.position =
                        Vec3{static_cast<f32>(position.x), static_cast<f32>(position.y),
                             static_cast<f32>(position.z)};
                    const f32 scale =
                        species->scale_min + ((species->scale_max - species->scale_min) *
                                              (static_cast<f32>(instance->scale) / 65'535.0F));
                    plant.yaw = (static_cast<f32>(instance->yaw) / 65'536.0F) * 6.28318F;
                    plant.crown_offset = response.offset;
                    plant.kind = kind_of(species->klass);
                    // The variation index picks the tint, so a stand of one species is not one
                    // colour. Every channel moves with it: a tint that lifted red and green and
                    // left blue where it was turned the most varied instances yellow, which is
                    // what the first still of this artefact showed in its forest.
                    const f32 variation = static_cast<f32>(instance->variation) * 0.09F;
                    plant.height = species->footprint_metres * height_ratio(plant.kind) * scale;
                    plant.radius = species->footprint_metres * radius_ratio(plant.kind) * scale;
                    plant.trunk_colour =
                        Vec3{0.20F + (variation * 0.20F), 0.15F + (variation * 0.14F),
                             0.11F + (variation * 0.08F)};
                    plant.crown_colour = crown_colour_of(plant.kind, variation);
                    if (Status pushed = plants_.push_back(plant); !pushed) {
                        return pushed;
                    }
                }
            }
        }
    }
    return ok();
}

// ================================================================================================
// THE CAMERA, AND WHAT IT STANDS ON
// ================================================================================================

f32 World::ground_height(f64 x, f64 z) const noexcept {
    const terrain::SurfaceSample sample = query_.sample(x, z);
    return sample.resolved ? sample.height : static_cast<f32>(options_.sea_level);
}

void World::camera_at(u64 frame, u64 frames, WorldVec3d& eye, WorldVec3d& target) const noexcept {
    // A slow orbit of the world's middle, rising a little as the day goes on. A fixed path
    // derived from the frame index alone, so two runs film the same shot.
    const f64 t = frames == 0 ? 0.0 : static_cast<f64>(frame) / static_cast<f64>(frames);
    const WorldVec3d middle = centre();
    const f64 extent = static_cast<f64>(extent_metres());
    const f64 angle = 0.9 + (t * 1.35);
    const f64 radius = extent * 0.60;
    eye = WorldVec3d{middle.x + (std::cos(angle) * radius), 0.0,
                     middle.z + (std::sin(angle) * radius)};
    const f32 ground = ground_height(eye.x, eye.z);
    const f32 floor = ground > static_cast<f32>(options_.sea_level)
                          ? ground
                          : static_cast<f32>(options_.sea_level);
    eye.y = static_cast<f64>(floor) + 120.0 + (std::sin(t * std::numbers::pi) * 90.0);
    target =
        WorldVec3d{middle.x, static_cast<f64>(ground_height(middle.x, middle.z)) + 30.0, middle.z};
}

// ================================================================================================
// M10 TASKS.MD 7.2 — A DEFORMATION THAT SURVIVES A SAVE
// ================================================================================================

Status World::deform_and_round_trip(const WorldVec3d& at, f32 radius, f32 depth,
                                    PersistenceReport& report) noexcept {
    terrain::Deformation crater;
    crater.klass = terrain::DeformationClass::Gameplay;
    crater.shape = terrain::DeformationShape::Radial;
    crater.bounds =
        terrain::TerrainBounds{at.x - static_cast<f64>(radius), at.z - static_cast<f64>(radius),
                               at.x + static_cast<f64>(radius), at.z + static_cast<f64>(radius)};
    crater.amount = -depth;
    crater.cause = "samples/10-world: the crater M10 task 7.2 saves and restores";

    // The surface over the crater's footprint BEFORE the delta store is dropped. Captured rather
    // than recomputed, because a comparison against a freshly evaluated expectation would pass on a
    // restore that reinstated nothing and a generator that is simply deterministic.
    Array<f32> deformed(*allocator_);
    const i32 half = 8;
    const usize probes = static_cast<usize>((2 * half) + 1) * static_cast<usize>((2 * half) + 1);
    if (Status sized = deformed.reserve(probes); !sized) {
        return sized;
    }

    const f32 before = ground_height(at.x, at.z);
    Expected<terrain::InvalidationSet, Error> invalidated = deltas_.apply(crater);
    if (!invalidated) {
        return make_unexpected(invalidated.error());
    }
    report.depth_before = before - ground_height(at.x, at.z);
    for (i32 j = -half; j <= half; ++j) {
        for (i32 i = -half; i <= half; ++i) {
            const f64 x = at.x + (static_cast<f64>(i) * static_cast<f64>(radius) / half);
            const f64 z = at.z + (static_cast<f64>(j) * static_cast<f64>(radius) / half);
            if (Status pushed = deformed.push_back(ground_height(x, z)); !pushed) {
                return pushed;
            }
        }
    }

    // THE WORLD'S OWN OVERLAY, NOT A SECOND ONE. `terrain` requires runtime change to be recorded
    // "in the world persistence overlay ... rather than a second one", and a terrain delta belongs
    // to a piece of WORLD rather than to an entity — so it goes in as a subsystem BLOB under
    // `terrain::kOverlayChannelTerrain` rather than as a `ComponentOverride` keyed by a
    // `PersistentId` that would have had to be derived from a position.
    cy::world::PersistenceOverlay overlay(*allocator_);
    if (Status recorded = deltas_.record_into(overlay, partition()); !recorded) {
        return recorded;
    }
    // The overlay's own cell count rather than a byte total it does not report, so the number in
    // the report is one the overlay actually holds.
    Array<cy::world::CellId> touched(*allocator_);
    if (Status listed = overlay.cells(touched); !listed) {
        return listed;
    }
    report.overlay_cells = static_cast<u32>(touched.size());
    for (const cy::world::CellId cell : touched.span()) {
        const cy::world::CellOverlay* entry = overlay.find(cell);
        if (entry == nullptr) {
            continue;
        }
        for (const cy::world::OverlayBlob& carried : entry->blobs.span()) {
            if (carried.channel == terrain::kOverlayChannelTerrain) {
                report.overlay_bytes += carried.size;
                ++report.overlay_blobs;
            }
        }
    }

    // Drop everything and restore only from what the overlay carries. A restore that MERGED would
    // pass whether or not the overlay held anything at all, so the store is cleared first — and
    // `restore_from()` clears it again for the same reason.
    deltas_.clear();
    if (ground_height(at.x, at.z) != before) {
        return fail(ErrorCode::Internal,
                    "clearing the delta store did not undo the crater, so the round trip below "
                    "would pass on a restore that did nothing");
    }
    if (Status restored = deltas_.restore_from(overlay, partition()); !restored) {
        return restored;
    }
    report.restored = deltas_.deformed_tiles() > 0;
    report.depth_after_restore = before - ground_height(at.x, at.z);

    usize probe = 0;
    for (i32 j = -half; j <= half; ++j) {
        for (i32 i = -half; i <= half; ++i) {
            const f64 x = at.x + (static_cast<f64>(i) * static_cast<f64>(radius) / half);
            const f64 z = at.z + (static_cast<f64>(j) * static_cast<f64>(radius) / half);
            ++report.samples_compared;
            // EXACT equality. A height delta is stored and restored as the same `f32`, so anything
            // but bit equality here is a lossy round trip and a tolerance would hide it.
            if (deformed[probe] != ground_height(x, z)) {
                ++report.samples_disagreeing;
            }
            ++probe;
        }
    }
    return ok();
}

}  // namespace cy::sample::world
