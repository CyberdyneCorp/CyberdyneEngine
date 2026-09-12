// Rivers: spline networks, junctions, and flow that geometry decides. M10 task 2.3, and `water`'s
// "Rivers" requirement.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`:
// `apply_continuity()` was changed to assign every vertex the section's head speed rather than Q/A,
// which is what a river whose flow is "uniform along the spline" looks like. "a narrows accelerates
// the flow, because discharge is conserved" went red on its first assertion. The continuity was
// restored.

#include <cy/test/test.h>

#include <cy/water/river.h>

#include <algorithm>
#include <cmath>

#include "fixtures.h"

namespace test = cy::water::test;
using cy::water::kNoSection;
using cy::water::RiverBuildReport;
using cy::water::RiverControlPoint;
using cy::water::RiverFoamSource;
using cy::water::RiverNetwork;
using cy::water::RiverObstacle;
using cy::water::RiverSample;
using cy::water::RiverSectionDesc;

namespace {

/// A trunk running east along z = 100, and a tributary joining it from the north at its midpoint.
[[nodiscard]] cy::Status author_confluence(RiverNetwork& network,
                                           cy::f64 tributary_level) noexcept {
    RiverControlPoint trunk[3];
    for (cy::u32 index = 0; index < 3; ++index) {
        trunk[index].position = cy::world::WorldVec3d{static_cast<cy::f64>(index) * 100.0,
                                                      10.0 - static_cast<cy::f64>(index), 100.0};
        trunk[index].width = 12.0F;
        trunk[index].depth = 2.0F;
        trunk[index].flow_speed = 1.0F;
    }
    const auto trunk_index = network.add_section(
        RiverSectionDesc{"trunk", cy::Span<const RiverControlPoint>(trunk, 3), kNoSection, 1.0F});
    if (!trunk_index) {
        return cy::make_unexpected(trunk_index.error());
    }

    RiverControlPoint branch[3];
    for (cy::u32 index = 0; index < 3; ++index) {
        branch[index].position =
            cy::world::WorldVec3d{100.0, tributary_level - static_cast<cy::f64>(index) * 0.5,
                                  20.0 + (static_cast<cy::f64>(index) * 40.0)};
        branch[index].width = 6.0F;
        branch[index].depth = 1.0F;
        branch[index].flow_speed = 1.5F;
    }
    const auto branch_index = network.add_section(RiverSectionDesc{
        "tributary", cy::Span<const RiverControlPoint>(branch, 3), *trunk_index, 0.5F});
    return branch_index ? cy::ok() : cy::make_unexpected(branch_index.error());
}

}  // namespace

CY_TEST_CASE("a narrows accelerates the flow, because discharge is conserved") {
    RiverNetwork network(test::allocator());
    CY_REQUIRE(test::author_straight_river(network, 5.0F).has_value());
    RiverBuildReport report;
    CY_REQUIRE(network.build(report).has_value());

    const RiverSample wide = network.sample(cy::world::WorldVec3d{5.0, 9.0, 100.0});
    const RiverSample narrow = network.sample(cy::world::WorldVec3d{150.0, 8.0, 100.0});
    CY_REQUIRE(wide.inside);
    CY_REQUIRE(narrow.inside);

    // Q = w x d x v is conserved, so halving the width doubles the speed. "Flow SHALL be modified
    // by geometry — ACCELERATING IN NARROWS — rather than being uniform along the spline."
    const cy::f32 wide_speed = cy::length(wide.velocity);
    const cy::f32 narrow_speed = cy::length(narrow.velocity);
    CY_CHECK_GT(narrow_speed, wide_speed * 1.5F);
    CY_CHECK_GT(report.largest_narrowing_ratio, 1.5F);

    // A river of constant width does NOT accelerate, which is what says the effect is the geometry
    // rather than an artefact of the resampling.
    RiverNetwork uniform(test::allocator());
    CY_REQUIRE(test::author_straight_river(uniform, 10.0F).has_value());
    RiverBuildReport uniform_report;
    CY_REQUIRE(uniform.build(uniform_report).has_value());
    CY_CHECK_NEAR(uniform_report.largest_narrowing_ratio, 1.0F, 0.05F);
}

CY_TEST_CASE("a tributary joins: one continuous surface and a combined flow") {
    RiverNetwork network(test::allocator());
    // The tributary is authored 0.4 m above the trunk at the confluence — the ordinary authoring
    // error, and the step a player would see.
    CY_REQUIRE(author_confluence(network, 9.9).has_value());
    RiverBuildReport report;
    CY_REQUIRE(network.build(report).has_value());
    CY_CHECK_EQ(report.junctions, 1u);

    // THE CONTINUITY: the mouth was pulled to the trunk's level, and the correction was reported
    // rather than swallowed.
    CY_CHECK_GT(report.largest_junction_correction, 0.01F);
    const RiverSample mouth = network.sample(cy::world::WorldVec3d{100.0, 9.0, 99.0});
    const RiverSample trunk_at_junction = network.sample(cy::world::WorldVec3d{100.0, 9.0, 103.0});
    const RiverSample trunk_above = network.sample(cy::world::WorldVec3d{60.0, 9.0, 100.0});
    const RiverSample trunk_below = network.sample(cy::world::WorldVec3d{140.0, 9.0, 100.0});
    CY_REQUIRE(mouth.inside);
    CY_REQUIRE(trunk_at_junction.inside);
    CY_CHECK_EQ(mouth.section, 1u);
    CY_CHECK_EQ(trunk_at_junction.section, 0u);
    // ONE SURFACE at the confluence: the tributary's mouth, authored 0.1 m high, was pulled to the
    // trunk's level. Without the correction these two differ by that step and a player sees it.
    CY_CHECK_LT(std::fabs(mouth.surface - trunk_at_junction.surface), 0.05);

    // THE COMBINED FLOW: below the confluence the trunk carries both discharges, so it is faster
    // than above it — not two overlapping surfaces, one river with more water in it.
    CY_CHECK_GT(cy::length(trunk_below.velocity), cy::length(trunk_above.velocity));
}

CY_TEST_CASE("a tributary of a tributary reaches the main stem") {
    RiverNetwork network(test::allocator());
    RiverControlPoint stem[2];
    RiverControlPoint first[2];
    RiverControlPoint second[2];
    for (cy::u32 index = 0; index < 2; ++index) {
        stem[index].position = cy::world::WorldVec3d{static_cast<cy::f64>(index) * 200.0, 5.0, 0.0};
        stem[index].width = 20.0F;
        stem[index].depth = 3.0F;
        stem[index].flow_speed = 1.0F;
        first[index].position =
            cy::world::WorldVec3d{100.0, 6.0, -100.0 + (static_cast<cy::f64>(index) * 100.0)};
        first[index].width = 8.0F;
        first[index].depth = 1.5F;
        first[index].flow_speed = 1.0F;
        second[index].position =
            cy::world::WorldVec3d{40.0 + (static_cast<cy::f64>(index) * 60.0), 7.0, -100.0};
        second[index].width = 4.0F;
        second[index].depth = 1.0F;
        second[index].flow_speed = 1.0F;
    }
    const auto stem_index = network.add_section(
        RiverSectionDesc{"stem", cy::Span<const RiverControlPoint>(stem, 2), kNoSection, 1.0F});
    CY_REQUIRE(stem_index.has_value());
    const auto first_index = network.add_section(
        RiverSectionDesc{"first", cy::Span<const RiverControlPoint>(first, 2), *stem_index, 0.5F});
    CY_REQUIRE(first_index.has_value());
    CY_REQUIRE(network
                   .add_section(RiverSectionDesc{
                       "second", cy::Span<const RiverControlPoint>(second, 2), *first_index, 0.0F})
                   .has_value());

    RiverBuildReport report;
    CY_REQUIRE(network.build(report).has_value());
    CY_CHECK_EQ(report.junctions, 2u);

    // The stem below its confluence carries its own discharge plus the first tributary's plus the
    // second's — the transitive sum, not just the one that touches it.
    const cy::Span<const cy::water::RiverSection> sections = network.sections();
    const cy::f32 stem_tributaries = sections[*stem_index].tributary_discharge;
    const cy::f32 first_total =
        sections[*first_index].discharge + sections[*first_index].tributary_discharge;
    CY_CHECK_NEAR(stem_tributaries, first_total, 1e-3F);
    CY_CHECK_GT(sections[*first_index].tributary_discharge, 0.0F);
}

CY_TEST_CASE("a forward reference is refused, so the network's shape is not declaration order") {
    RiverNetwork network(test::allocator());
    RiverControlPoint points[2];
    for (cy::u32 index = 0; index < 2; ++index) {
        points[index].position =
            cy::world::WorldVec3d{static_cast<cy::f64>(index) * 50.0, 0.0, 0.0};
    }
    CY_CHECK_FALSE(network
                       .add_section(RiverSectionDesc{
                           "orphan", cy::Span<const RiverControlPoint>(points, 2), 7U, 0.5F})
                       .has_value());

    // And a section with one point is not a spline.
    CY_CHECK_FALSE(network
                       .add_section(RiverSectionDesc{
                           "point", cy::Span<const RiverControlPoint>(points, 1), kNoSection, 0.0F})
                       .has_value());
}

CY_TEST_CASE("flow deflects around an obstacle and is undisturbed without one") {
    RiverNetwork clear(test::allocator());
    CY_REQUIRE(test::author_straight_river(clear, 10.0F).has_value());
    RiverBuildReport clear_report;
    CY_REQUIRE(clear.build(clear_report).has_value());

    RiverNetwork obstructed(test::allocator());
    CY_REQUIRE(test::author_straight_river(obstructed, 10.0F).has_value());
    RiverObstacle rock;
    rock.centre = cy::world::WorldVec3d{150.0, 9.0, 100.0};
    rock.radius = 2.0F;
    rock.influence = 4.0F;
    CY_REQUIRE(obstructed.add_obstacle(rock).has_value());
    RiverBuildReport report;
    CY_REQUIRE(obstructed.build(report).has_value());

    // Just upstream of the rock and slightly to one side: the clear river runs straight down the
    // channel, and the obstructed one is pushed aside.
    const cy::world::WorldVec3d beside{147.0, 9.0, 101.5};
    const RiverSample without = clear.sample(beside);
    const RiverSample with = obstructed.sample(beside);
    CY_REQUIRE(without.inside);
    CY_REQUIRE(with.inside);
    CY_CHECK_NEAR(without.velocity.z, 0.0F, 1e-3F);
    CY_CHECK_GT(std::fabs(with.velocity.z), 0.05F);

    // Far from the rock nothing changed, so the deflection is local rather than a global bend.
    const RiverSample far_without = clear.sample(cy::world::WorldVec3d{20.0, 9.0, 100.0});
    const RiverSample far_with = obstructed.sample(cy::world::WorldVec3d{20.0, 9.0, 100.0});
    CY_CHECK_NEAR(far_with.velocity.x, far_without.velocity.x, 1e-4F);
}

CY_TEST_CASE("bends and drops are turbulent, and foam sources follow them") {
    RiverNetwork network(test::allocator());
    // A hairpin with a drop through it.
    RiverControlPoint points[5];
    const cy::f64 xs[5] = {0.0, 40.0, 60.0, 40.0, 0.0};
    const cy::f64 zs[5] = {0.0, 0.0, 30.0, 60.0, 60.0};
    for (cy::u32 index = 0; index < 5; ++index) {
        points[index].position =
            cy::world::WorldVec3d{xs[index], 20.0 - (static_cast<cy::f64>(index) * 3.0), zs[index]};
        points[index].width = 8.0F;
        points[index].depth = 1.0F;
        points[index].flow_speed = 2.0F;
        points[index].turbulence = 0.02F;
    }
    CY_REQUIRE(network
                   .add_section(RiverSectionDesc{
                       "hairpin", cy::Span<const RiverControlPoint>(points, 5), kNoSection, 1.0F})
                   .has_value());
    RiverBuildReport report;
    CY_REQUIRE(network.build(report).has_value());

    cy::f32 highest = 0.0F;
    for (const cy::water::RiverVertex& vertex : network.vertices()) {
        highest = std::max(highest, vertex.turbulence);
    }
    // The authored baseline was 0.02; the bend and the drop add to it rather than replacing it.
    CY_CHECK_GT(highest, 0.25F);

    cy::Array<RiverFoamSource> sources(test::allocator());
    CY_REQUIRE(network.foam_sources(sources).has_value());
    CY_CHECK_GT(sources.size(), 0u);
    for (const RiverFoamSource& source : sources) {
        CY_CHECK_GE(source.strength, 0.25F);
        // Foam leaves a source along the flow, which is what advects it downstream.
        CY_CHECK_GT(cy::length(source.drift), 0.0F);
    }

    // A straight, level reach generates none: a foam source list that was full whatever the river
    // did would pass the case above for the wrong reason.
    RiverNetwork calm(test::allocator());
    CY_REQUIRE(test::author_straight_river(calm, 10.0F).has_value());
    RiverBuildReport calm_report;
    CY_REQUIRE(calm.build(calm_report).has_value());
    cy::Array<RiverFoamSource> none(test::allocator());
    CY_REQUIRE(calm.foam_sources(none).has_value());
    CY_CHECK_EQ(none.size(), 0u);
}

CY_TEST_CASE("the bed profile is a channel, and the sample carries the bank distance") {
    RiverNetwork network(test::allocator());
    CY_REQUIRE(test::author_straight_river(network, 10.0F).has_value());
    RiverBuildReport report;
    CY_REQUIRE(network.build(report).has_value());

    const RiverSample centre = network.sample(cy::world::WorldVec3d{50.0, 9.0, 100.0});
    const RiverSample edge = network.sample(cy::world::WorldVec3d{50.0, 9.0, 104.5});
    const RiverSample bank = network.sample(cy::world::WorldVec3d{50.0, 9.0, 112.0});

    CY_REQUIRE(centre.inside);
    CY_REQUIRE(edge.inside);
    CY_CHECK_FALSE(bank.inside);

    // Deepest at the thalweg, shallow at the edge, and the bank distance is signed so a consumer
    // can use it directly as a water-distance contribution.
    CY_CHECK_LT(centre.bed, edge.bed);
    CY_CHECK_GT(centre.distance_to_bank, edge.distance_to_bank);
    CY_CHECK_LT(bank.distance_to_bank, 0.0F);
    CY_CHECK_NEAR(static_cast<cy::f32>(centre.surface - centre.bed), 2.0F, 1e-3F);
}

CY_TEST_CASE("a sample before the build finds nothing, rather than answering from half a network") {
    RiverNetwork network(test::allocator());
    CY_REQUIRE(test::author_straight_river(network, 10.0F).has_value());
    CY_CHECK_FALSE(network.built());
    CY_CHECK_FALSE(network.sample(cy::world::WorldVec3d{50.0, 9.0, 100.0}).found);

    cy::Array<RiverFoamSource> sources(test::allocator());
    CY_CHECK_FALSE(network.foam_sources(sources).has_value());
}

CY_TEST_CASE("building twice produces the same river") {
    RiverNetwork network(test::allocator());
    CY_REQUIRE(author_confluence(network, 9.9).has_value());
    RiverBuildReport first;
    CY_REQUIRE(network.build(first).has_value());
    const RiverSample before = network.sample(cy::world::WorldVec3d{150.0, 9.0, 100.0});

    RiverBuildReport second;
    CY_REQUIRE(network.build(second).has_value());
    const RiverSample after = network.sample(cy::world::WorldVec3d{150.0, 9.0, 100.0});

    // Idempotent: a junction correction applied twice would sink the tributary by twice the error,
    // and a discharge propagated twice would double the trunk's flow.
    CY_CHECK_EQ(before.surface, after.surface);
    CY_CHECK_EQ(before.velocity.x, after.velocity.x);
    CY_CHECK_NEAR(first.largest_junction_correction, second.largest_junction_correction, 1e-6F);
}
