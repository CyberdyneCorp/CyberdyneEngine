// Wind response: one preparation per cluster, a gust that travels, and detail that falls off with
// distance. M10 task 2.4; `foliage`'s "Wind response" requirement.

#include <cy/test/test.h>

#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/foliage/wind.h>

#include "fixtures.h"

#include <cmath>

namespace test = cy::foliage::test;
using cy::foliage::ClusterBuilder;
using cy::foliage::ClusterCoord;
using cy::foliage::ClusterId;
using cy::foliage::ClusterWind;
using cy::foliage::evaluate_response;
using cy::foliage::FoliageCluster;
using cy::foliage::InstanceFlags;
using cy::foliage::resolve_wind_detail;
using cy::foliage::species_id;
using cy::foliage::SpeciesLibrary;
using cy::foliage::VertexParameters;
using cy::foliage::wind_field_declaration;
using cy::foliage::WindDetail;
using cy::foliage::WindDisplacement;
using cy::foliage::WindSampler;
using cy::foliage::WindTuning;

namespace {

/// A world with a declared wind field, a producer, and a gust somewhere in it.
struct WindWorld {
    cy::environment::FieldRegistry registry;
    cy::environment::FieldStore fields;
    cy::environment::ProducerToken token;

    WindWorld() noexcept
        : registry(test::allocator()), fields(test::allocator(), registry, test::partition()) {}

    [[nodiscard]] cy::Status open() noexcept {
        if (cy::Status declared = registry.declare(wind_field_declaration(16.0F)); !declared) {
            return declared;
        }
        auto claimed = registry.claim(cy::environment::field_id(cy::environment::fields::kWind),
                                      "test.weather", cy::environment::ProducerKind::System);
        if (!claimed) {
            return cy::make_unexpected(claimed.error());
        }
        token = static_cast<cy::environment::ProducerToken&&>(claimed.value());
        return cy::ok();
    }

    /// Write a west-to-east wind whose speed peaks over the LATTICE COLUMN `gust_cell` of the macro
    /// tile at the origin. Per lattice point and not per tile: a macro tile is sixteen cells of
    /// 128 m, so a whole treeline fits inside one and a per-tile gust would give every cluster the
    /// same value.
    [[nodiscard]] cy::Status write_gust(cy::u32 gust_cell) noexcept {
        auto writer = fields.open_writer(token);
        if (!writer) {
            return cy::make_unexpected(writer.error());
        }
        cy::environment::TileAddress address;
        address.field = cy::environment::field_id(cy::environment::fields::kWind);
        address.x = 0;
        address.z = 0;
        address.level = static_cast<cy::u8>(cy::environment::FieldResidency::Macro);
        address.layer = static_cast<cy::u8>(cy::environment::FieldLayer::Base);
        if (cy::Status staged = writer.value().stage(address); !staged) {
            return staged;
        }
        for (cy::u32 x = 0; x < cy::environment::kTileCells; ++x) {
            const cy::u32 offset = x > gust_cell ? x - gust_cell : gust_cell - x;
            cy::f32 speed = 1.0F;
            if (offset == 0) {
                speed = 14.0F;
            } else if (offset == 1) {
                speed = 6.0F;
            }
            for (cy::u32 y = 0; y < 4; ++y) {
                for (cy::u32 z = 0; z < cy::environment::kTileCells; ++z) {
                    if (cy::Status set = writer.value().set(
                            address, x, y, z, cy::environment::FieldValue::vec3(speed, 0.0F, 0.0F));
                        !set) {
                        return set;
                    }
                }
            }
        }
        return writer.value().publish();
    }
};

[[nodiscard]] cy::Status build_row(cy::Array<FoliageCluster>& out, cy::u32 count) noexcept {
    for (cy::u32 index = 0; index < count; ++index) {
        // Centred on lattice point `index` of the wind field's macro level (128 m cells), so the
        // cluster's own sample is a lattice value rather than the average of two neighbours.
        cy::foliage::ClusterBounds bounds;
        bounds.min_x = (static_cast<cy::f64>(index) * 128.0) - 64.0;
        bounds.min_z = -64.0;
        bounds.max_x = bounds.min_x + 128.0;
        bounds.max_z = 64.0;
        bounds.min_y = 0.0F;
        bounds.max_y = 32.0F;
        ClusterBuilder builder(test::allocator(), test::policy(),
                               ClusterId{static_cast<cy::u64>(index + 1)},
                               ClusterCoord{static_cast<cy::i32>(index), 0, 0}, bounds);
        for (cy::u32 instance = 0; instance < 50; ++instance) {
            if (cy::Status added = builder.add(
                    species_id(test::kPine),
                    cy::world::WorldVec3d{bounds.min_x + (static_cast<cy::f64>(instance) * 2.0),
                                          4.0, 10.0},
                    0.0F, 0.5F, 0, 128, 0.0F, 0.0F, InstanceFlags{});
                !added) {
                return added;
            }
        }
        auto built = builder.finish();
        if (!built) {
            return cy::make_unexpected(built.error());
        }
        if (cy::Status pushed = out.push_back(static_cast<FoliageCluster&&>(built.value()));
            !pushed) {
            return pushed;
        }
    }
    return cy::ok();
}

}  // namespace

CY_TEST_CASE("foliage declares the wind field but does not claim it") {
    // `environment-fields` names `weather-and-wind` as the wind field's producer. Foliage declaring
    // it AND claiming it would be exactly the "two systems writing one field" the substrate exists
    // to refuse — so it declares and reads, and the refusal below is what says so.
    cy::environment::FieldRegistry registry(test::allocator());
    CY_REQUIRE(registry.declare(wind_field_declaration(16.0F)).has_value());
    const auto wind = cy::environment::field_id(cy::environment::fields::kWind);

    auto weather = registry.claim(wind, "weather.wind", cy::environment::ProducerKind::System);
    CY_REQUIRE(weather.has_value());
    auto foliage = registry.claim(wind, "foliage.wind", cy::environment::ProducerKind::System);
    CY_CHECK_FALSE(foliage.has_value());
    CY_CHECK_EQ(registry.last_conflict().incumbent, "weather.wind");
    CY_CHECK_EQ(registry.last_conflict().challenger, "foliage.wind");
}

CY_TEST_CASE("wind preparation is per cluster, not per instance") {
    // `foliage` — "Wind response ... SHALL NOT REQUIRE PER-INSTANCE CPU WORK." The measurement:
    // the CPU wrote one struct per cluster while covering two orders of magnitude more instances,
    // and took exactly one field sample per cluster.
    WindWorld world;
    CY_REQUIRE(world.open().has_value());
    CY_REQUIRE(world.write_gust(4u).has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());

    cy::Array<FoliageCluster> clusters(test::allocator());
    CY_REQUIRE(build_row(clusters, 10).has_value());

    auto sampler =
        WindSampler::open(world.fields, cy::environment::field_id(cy::environment::fields::kWind),
                          cy::determinism::SimulationClass::Presentation);
    CY_REQUIRE(sampler.has_value());

    WindTuning tuning;
    auto report = sampler.value().prepare(library, clusters.span(),
                                          cy::world::WorldVec3d{0.0, 10.0, 64.0}, tuning);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().clusters_prepared, 10u);
    CY_CHECK_EQ(report.value().field_samples, 10u);
    CY_CHECK_EQ(report.value().instances_covered, 500u);
    // Fifty times more instances than preparations.
    CY_CHECK_EQ(report.value().instances_covered / report.value().clusters_prepared, 50u);
}

CY_TEST_CASE("a gust crosses a treeline because every cluster samples the same field") {
    // `foliage` — "WHEN a gust moves across a treeline THEN the response SHALL travel with it,
    // BECAUSE ALL INSTANCES SAMPLE THE SAME FIELD."
    WindWorld world;
    CY_REQUIRE(world.open().has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    cy::Array<FoliageCluster> clusters(test::allocator());
    CY_REQUIRE(build_row(clusters, 10).has_value());

    auto sampler =
        WindSampler::open(world.fields, cy::environment::field_id(cy::environment::fields::kWind),
                          cy::determinism::SimulationClass::Presentation);
    CY_REQUIRE(sampler.has_value());
    WindTuning tuning;
    tuning.hierarchical_metres = 100'000.0F;

    const cy::foliage::SpeciesDeclaration* pine = library.find(species_id(test::kPine));
    CY_REQUIRE(pine != nullptr);
    VertexParameters canopy;
    canopy.height_fraction = 1.0F;
    canopy.radial_fraction = 1.0F;
    canopy.vertex_phase = 0.25F;

    // Walk the gust across the row and record which cluster deflects most at each step.
    cy::u32 previous_peak = 0;
    cy::u32 advances = 0;
    for (cy::u32 gust = 1; gust <= 6; ++gust) {
        CY_REQUIRE(world.write_gust(gust).has_value());
        CY_REQUIRE(
            sampler.value()
                .prepare(library, clusters.span(), cy::world::WorldVec3d{0.0, 10.0, 64.0}, tuning)
                .has_value());
        cy::f32 best = -1.0F;
        cy::u32 peak = 0;
        for (cy::u32 index = 0; index < static_cast<cy::u32>(clusters.size()); ++index) {
            const ClusterWind& wind = clusters[index].wind();
            // The cluster's own sampled speed is what travels; the per-vertex response is a
            // function of it, so peak speed and peak deflection are the same cluster.
            const cy::f32 speed = std::abs(wind.wind.x);
            if (speed > best) {
                best = speed;
                peak = index;
            }
        }
        if (gust > 1) {
            advances += peak > previous_peak ? 1U : 0U;
        }
        previous_peak = peak;
    }
    CY_CHECK_EQ(advances, 5u);

    // And the response really is larger where the wind is: a clamp that ignored the field would
    // pass the walk above and fail this.
    const WindDisplacement strong = evaluate_response(*pine, clusters[previous_peak].wind(),
                                                      *clusters[previous_peak].at(0), canopy, 1.7F);
    const WindDisplacement calm =
        evaluate_response(*pine, clusters[0].wind(), *clusters[0].at(0), canopy, 1.7F);
    CY_CHECK_GT(std::abs(strong.offset.x), std::abs(calm.offset.x));
}

CY_TEST_CASE("distant foliage gets a simple sway and near foliage the full hierarchy") {
    WindTuning tuning;
    tuning.hierarchical_metres = 40.0F;
    tuning.sway_metres = 150.0F;
    CY_CHECK_EQ(resolve_wind_detail(WindDetail::Leaf, 10.0F, tuning), WindDetail::Leaf);
    CY_CHECK_EQ(resolve_wind_detail(WindDetail::Leaf, 90.0F, tuning), WindDetail::Branch);
    CY_CHECK_EQ(resolve_wind_detail(WindDetail::Leaf, 400.0F, tuning), WindDetail::Sway);
    // A species' own declaration is a ceiling: no distance and no budget can give grass branches.
    CY_CHECK_EQ(resolve_wind_detail(WindDetail::Sway, 1.0F, tuning), WindDetail::Sway);
    CY_CHECK_EQ(resolve_wind_detail(WindDetail::None, 1.0F, tuning), WindDetail::None);
    // And so is the budget's.
    tuning.ceiling = WindDetail::Sway;
    CY_CHECK_EQ(resolve_wind_detail(WindDetail::Leaf, 1.0F, tuning), WindDetail::Sway);
}

CY_TEST_CASE("the hierarchy really has four terms and a sway has one") {
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    const cy::foliage::SpeciesDeclaration* pine = library.find(species_id(test::kPine));
    CY_REQUIRE(pine != nullptr);

    ClusterWind wind;
    wind.wind = cy::Vec3{9.0F, 0.0F, 0.0F};
    wind.phase = 0.3F;
    wind.detail = WindDetail::Leaf;
    cy::foliage::FoliageInstance instance;
    instance.yaw = 4000;

    VertexParameters leaf;
    leaf.height_fraction = 1.0F;
    leaf.radial_fraction = 1.0F;
    leaf.vertex_phase = 0.4F;
    const WindDisplacement full = evaluate_response(*pine, wind, instance, leaf, 2.0F);
    CY_CHECK_EQ(full.term_count, 4u);

    wind.detail = WindDetail::Sway;
    const WindDisplacement simple = evaluate_response(*pine, wind, instance, leaf, 2.0F);
    CY_CHECK_EQ(simple.term_count, 1u);
    CY_CHECK_EQ(simple.terms[1], 0.0F);

    // The base of the trunk barely moves and the canopy moves most, which is what makes the
    // deformation read as a plant rather than as a translation.
    VertexParameters base;
    base.height_fraction = 0.0F;
    wind.detail = WindDetail::Leaf;
    const WindDisplacement at_base = evaluate_response(*pine, wind, instance, base, 2.0F);
    CY_CHECK_LT(std::abs(at_base.offset.x), std::abs(full.offset.x));

    // No wind, no deflection — and no NaN from normalising a zero vector.
    wind.wind = cy::Vec3{0.0F, 0.0F, 0.0F};
    const WindDisplacement still = evaluate_response(*pine, wind, instance, leaf, 2.0F);
    CY_CHECK_EQ(still.offset.x, 0.0F);
    CY_CHECK_EQ(still.offset.z, 0.0F);
}
