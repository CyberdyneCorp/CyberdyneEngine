// Deterministic placement, and the three of the M10 spike's four conditions that live in this
// module. M10 task 2.4; `foliage`'s "Procedural placement" requirement.
//
// The cases that matter most are the two order-freedom ones. The spike's whole finding is that a
// partial regeneration reproduces a full one ONLY when conflict resolution reads neighbours'
// CANDIDATES rather than their ACCEPTED OUTPUT — the `ordered` variant reproduced 2 of 12 trials at
// best — so "one region alone equals that region inside the whole world" is the property this
// module is judged on, not a nicety.

#include <cy/test/test.h>

#include <cy/foliage/placement.h>

#include "fixtures.h"

namespace test = cy::foliage::test;
using cy::foliage::Candidate;
using cy::foliage::ClusterCoord;
using cy::foliage::ExclusionZone;
using cy::foliage::FieldBindings;
using cy::foliage::generate_candidates;
using cy::foliage::generate_region;
using cy::foliage::GenerationContext;
using cy::foliage::PlacementDiagnostic;
using cy::foliage::PlacementRule;
using cy::foliage::PlacementRuleSet;
using cy::foliage::PlacementSampler;
using cy::foliage::resolve_spacing;
using cy::foliage::RuleInput;
using cy::foliage::RuleProblem;
using cy::foliage::RuleTest;
using cy::foliage::species_id;
using cy::foliage::SpeciesLibrary;
using cy::foliage::validate_rules;

namespace {

/// Compare two clusters instance for instance. The comparison the spike made: the OUTPUT and the
/// generated IDENTITY, both.
[[nodiscard]] cy::u32 differences(const cy::foliage::FoliageCluster& a,
                                  const cy::foliage::FoliageCluster& b, cy::u64 seed) noexcept {
    if (a.size() != b.size()) {
        return static_cast<cy::u32>(a.size() > b.size() ? a.size() - b.size()
                                                        : b.size() - a.size()) +
               1000000U;
    }
    cy::u32 count = 0;
    for (cy::u32 slot = 0; slot < static_cast<cy::u32>(a.size()); ++slot) {
        if (!(*a.at(slot) == *b.at(slot))) {
            ++count;
        }
        if (!(a.identity_at(seed, slot) == b.identity_at(seed, slot))) {
            ++count;
        }
    }
    return count;
}

}  // namespace

CY_TEST_CASE("a rule set is refused for each of its own reasons") {
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());

    PlacementRuleSet empty(test::allocator());
    CY_CHECK_EQ(validate_rules(empty, library), RuleProblem::NoRules);

    PlacementRuleSet rules(test::allocator());
    CY_REQUIRE(test::author_rules(rules).has_value());
    CY_CHECK_EQ(validate_rules(rules, library), RuleProblem::None);

    rules.graph_version = 0;
    CY_CHECK_EQ(validate_rules(rules, library), RuleProblem::ZeroGraphVersion);
    rules.graph_version = 1;

    rules.rules[0].species = species_id("nobody.declared.this");
    CY_CHECK_EQ(validate_rules(rules, library), RuleProblem::UnknownSpecies);
    rules.rules[0].species = species_id(test::kPine);

    rules.rules[0].density_per_hectare = 0.0F;
    CY_CHECK_EQ(validate_rules(rules, library), RuleProblem::NonPositiveDensity);
    rules.rules[0].density_per_hectare = 320.0F;

    // A category test with no category selected can never pass, and a continuous test with a
    // category mask is a rule written for a different input. Both are refused rather than ignored.
    rules.rules[0].tests[0] = RuleTest{RuleInput::Biome, 0.0F, 0.0F, 1.0F, 1.0F, 0, false};
    CY_CHECK_EQ(validate_rules(rules, library), RuleProblem::EmptyCategorySet);
    rules.rules[0].tests[0] = RuleTest{RuleInput::Slope, 0.0F, 0.0F, 20.0F, 30.0F, 0x3ULL, false};
    CY_CHECK_EQ(validate_rules(rules, library), RuleProblem::CategoryInputMismatch);
    rules.rules[0].tests[0] = RuleTest{RuleInput::Slope, 30.0F, 20.0F, 10.0F, 0.0F, 0, false};
    CY_CHECK_EQ(validate_rules(rules, library), RuleProblem::TestNotOrdered);
}

CY_TEST_CASE("a region regenerates identically on a second run") {
    // `foliage` — "WHEN a region is regenerated on another machine or in another session THEN it
    // SHALL produce IDENTICAL INSTANCES for the same seed and rule version."
    test::TestWorld world;
    CY_REQUIRE(world.build(-1, -1, 2, 2).has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    PlacementRuleSet rules(test::allocator());
    CY_REQUIRE(test::author_rules(rules).has_value());
    PlacementSampler sampler(world.query, world.fields, FieldBindings::standard(),
                             test::test_sun());
    const GenerationContext context = test::context_for(library, rules, sampler, 0xC0FFEE);

    auto first = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    auto second = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK_GT(first.value().cluster.size(), 50u);
    CY_CHECK_EQ(differences(first.value().cluster, second.value().cluster, context.seed), 0u);

    // A different seed is a different forest, so the comparison above is measuring something.
    GenerationContext other = context;
    other.seed = 0xC0FFEF;
    auto third = generate_region(test::allocator(), other, ClusterCoord{0, 0, 0});
    CY_REQUIRE(third.has_value());
    CY_CHECK_GT(differences(first.value().cluster, third.value().cluster, context.seed), 0u);
}

CY_TEST_CASE("spacing resolution does not depend on the order of the candidate array") {
    // CONDITION 1 OF THE SPIKE'S FOUR, directly. `resolve_spacing()` is given the same candidates
    // in two orders and must produce the same SET of survivors.
    test::TestWorld world;
    CY_REQUIRE(world.build(-1, -1, 2, 2).has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    PlacementRuleSet rules(test::allocator());
    CY_REQUIRE(test::author_rules(rules).has_value());
    PlacementSampler sampler(world.query, world.fields, FieldBindings::standard(),
                             test::test_sun());
    const GenerationContext context = test::context_for(library, rules, sampler, 7);

    cy::Array<Candidate> forward(test::allocator());
    PlacementDiagnostic why;
    CY_REQUIRE(generate_candidates(context, ClusterCoord{0, 0, 0}, forward, why).has_value());
    CY_REQUIRE((forward.size()) > (100u));

    cy::Array<Candidate> shuffled(test::allocator());
    CY_REQUIRE(shuffled.append(forward.span()).has_value());
    // A fixed deterministic permutation; a random one would make a failure unreproducible.
    for (cy::usize index = 0; index + 1 < shuffled.size(); index += 2) {
        const Candidate keep = shuffled[index];
        shuffled[index] = shuffled[shuffled.size() - 1 - index];
        shuffled[shuffled.size() - 1 - index] = keep;
    }

    cy::Array<cy::u32> accepted_forward(test::allocator());
    cy::Array<cy::u32> accepted_shuffled(test::allocator());
    PlacementDiagnostic a;
    PlacementDiagnostic b;
    CY_REQUIRE(resolve_spacing(forward.span(), accepted_forward, a).has_value());
    CY_REQUIRE(resolve_spacing(shuffled.span(), accepted_shuffled, b).has_value());
    CY_REQUIRE_EQ(accepted_forward.size(), accepted_shuffled.size());
    CY_CHECK_GT(a.rejected_spacing, 0u);  // the resolution actually rejected something

    // The same SET, compared by the candidates' own stable identifiers rather than by index.
    cy::u32 missing = 0;
    for (cy::u32 index : accepted_forward) {
        const Candidate& candidate = forward[index];
        bool found = false;
        for (cy::u32 other : accepted_shuffled) {
            const Candidate& rival = shuffled[other];
            found = found || (rival.rule_index == candidate.rule_index &&
                              rival.sample_index == candidate.sample_index &&
                              rival.region == candidate.region);
        }
        missing += found ? 0U : 1U;
    }
    CY_CHECK_EQ(missing, 0u);
}

CY_TEST_CASE("one region generated alone equals that region inside the whole world") {
    // CONDITION 1 AGAIN, at the level that matters: the partial regeneration the spike measured.
    // A region generated on its own must be bit-identical to the same region generated as part of a
    // sweep over its neighbours — output AND generated identity both, which is what the spike
    // compared and what a `counter` identity failed in all 12 of its trials.
    test::TestWorld world;
    CY_REQUIRE(world.build(-2, -2, 3, 3).has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    PlacementRuleSet rules(test::allocator());
    CY_REQUIRE(test::author_rules(rules).has_value());
    PlacementSampler sampler(world.query, world.fields, FieldBindings::standard(),
                             test::test_sun());
    const GenerationContext context = test::context_for(library, rules, sampler, 0x5EED);

    // The "full" sweep: every region of a 3x3 block, in row-major order.
    cy::Array<cy::u32> full_sizes(test::allocator());
    for (cy::i32 z = -1; z <= 1; ++z) {
        for (cy::i32 x = -1; x <= 1; ++x) {
            auto population = generate_region(test::allocator(), context, ClusterCoord{x, z, 0});
            CY_REQUIRE(population.has_value());
            CY_REQUIRE(full_sizes.push_back(static_cast<cy::u32>(population.value().cluster.size()))
                           .has_value());
        }
    }

    // The "partial": the same nine regions in REVERSE order, each on its own, and each compared
    // against the sweep's answer for it.
    cy::u32 index = 9;
    cy::u32 total_differences = 0;
    for (cy::i32 z = 1; z >= -1; --z) {
        for (cy::i32 x = 1; x >= -1; --x) {
            --index;
            auto partial = generate_region(test::allocator(), context, ClusterCoord{x, z, 0});
            CY_REQUIRE(partial.has_value());
            auto reference = generate_region(test::allocator(), context, ClusterCoord{x, z, 0});
            CY_REQUIRE(reference.has_value());
            CY_CHECK_EQ(static_cast<cy::u32>(partial.value().cluster.size()), full_sizes[index]);
            total_differences +=
                differences(partial.value().cluster, reference.value().cluster, context.seed);
        }
    }
    CY_CHECK_EQ(total_differences, 0u);
}

CY_TEST_CASE("a region's cluster is the region's share of a WHOLE-WORLD resolution") {
    // THE COMPARISON THE SPIKE ACTUALLY MADE, and the one the case above is too weak to make on its
    // own: generating each region independently is reproducible whatever the conflict rule is,
    // because each region builds its candidate array in the same fixed order every time. That is
    // the spike's own "a test that never contended" trap, and the honest answer is to compare
    // against a reference the per-region path did NOT produce.
    //
    // So this builds the FULL regeneration by hand — the union of every candidate over a 5x5 block,
    // resolved in ONE pass — and requires each inner region's independently generated cluster to be
    // exactly that region's share of it. A halo that read too little, or a conflict rule that
    // depended on what a neighbour had accepted, gives a different share and this goes red.
    test::TestWorld world;
    CY_REQUIRE(world.build(-3, -3, 4, 4).has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    PlacementRuleSet rules(test::allocator());
    CY_REQUIRE(test::author_rules(rules).has_value());
    PlacementSampler sampler(world.query, world.fields, FieldBindings::standard(),
                             test::test_sun());
    const GenerationContext context = test::context_for(library, rules, sampler, 0x5A11AD);

    cy::Array<Candidate> world_candidates(test::allocator());
    PlacementDiagnostic sink;
    for (cy::i32 z = -2; z <= 2; ++z) {
        for (cy::i32 x = -2; x <= 2; ++x) {
            CY_REQUIRE(generate_candidates(context, ClusterCoord{x, z, 0}, world_candidates, sink)
                           .has_value());
        }
    }
    CY_REQUIRE((world_candidates.size()) > (500u));

    cy::Array<cy::u32> world_accepted(test::allocator());
    PlacementDiagnostic global_why;
    CY_REQUIRE(resolve_spacing(world_candidates.span(), world_accepted, global_why).has_value());
    CY_REQUIRE((global_why.rejected_spacing) > (0u));

    cy::u32 mismatched_regions = 0;
    cy::u32 unmatched_positions = 0;
    for (cy::i32 z = -1; z <= 1; ++z) {
        for (cy::i32 x = -1; x <= 1; ++x) {
            const ClusterCoord region{x, z, 0};
            auto population = generate_region(test::allocator(), context, region);
            CY_REQUIRE(population.has_value());
            const cy::foliage::FoliageCluster& cluster = population.value().cluster;

            cy::u32 share = 0;
            for (cy::u32 index : world_accepted) {
                if (!(world_candidates[index].region == region)) {
                    continue;
                }
                ++share;
                // And the survivor is really in the cluster, within the quantisation the record
                // carries — a count that matched while the positions did not would be a coincidence
                // rather than an agreement.
                bool found = false;
                for (cy::u32 slot = 0; slot < static_cast<cy::u32>(cluster.size()); ++slot) {
                    const cy::world::WorldVec3d at = cluster.bounds().decode(*cluster.at(slot));
                    const cy::f64 dx = at.x - world_candidates[index].position.x;
                    const cy::f64 dz = at.z - world_candidates[index].position.z;
                    found = found || ((dx * dx) + (dz * dz)) < 0.0001;
                }
                unmatched_positions += found ? 0U : 1U;
            }
            mismatched_regions += share == static_cast<cy::u32>(cluster.size()) ? 0U : 1U;
        }
    }
    CY_CHECK_EQ(mismatched_regions, 0u);
    CY_CHECK_EQ(unmatched_positions, 0u);
}

CY_TEST_CASE("a region's provenance records what it actually read") {
    // CONDITION 2's half of the bargain: the invalidation belongs to `procedural-content-
    // generation`, and what this module owes it is a record of the regions and fields a generation
    // touched — so a dirty set is computed rather than a radius dilated.
    test::TestWorld world;
    CY_REQUIRE(world.build(-2, -2, 3, 3).has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    PlacementRuleSet rules(test::allocator());
    CY_REQUIRE(test::author_rules(rules).has_value());
    PlacementSampler sampler(world.query, world.fields, FieldBindings::standard(),
                             test::test_sun());
    const GenerationContext context = test::context_for(library, rules, sampler, 3);

    auto population = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    CY_REQUIRE(population.has_value());
    const cy::foliage::Provenance& provenance = population.value().provenance;

    // The rules' largest spacing is 4 m against a 64 m region, so the halo is exactly one ring —
    // DERIVED from the rules rather than declared, which is the difference the spike's design note
    // is about.
    CY_CHECK_EQ(provenance.regions.size(), 9u);
    CY_CHECK(provenance.read_region(ClusterCoord{0, 0, 0}));
    CY_CHECK(provenance.read_region(ClusterCoord{1, -1, 0}));
    CY_CHECK_FALSE(provenance.read_region(ClusterCoord{2, 0, 0}));
    CY_CHECK(provenance.intersects(10.0, 10.0, 20.0, 20.0));
    CY_CHECK_FALSE(provenance.intersects(10000.0, 10000.0, 10010.0, 10010.0));
}

CY_TEST_CASE("a rule input that excludes a region is the one the diagnostic names") {
    // `foliage` — "WHEN a region generates no foliage THEN the editor SHALL show WHICH RULE INPUT
    // EXCLUDED IT."
    test::TestWorld world;
    CY_REQUIRE(world.build(-1, -1, 2, 2).has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());

    PlacementRuleSet rules(test::allocator());
    rules.graph_version = 1;
    PlacementRule impossible;
    impossible.species = species_id(test::kPine);
    impossible.density_per_hectare = 400.0F;
    impossible.spacing_metres = 2.0F;
    // An altitude band this hillside never reaches.
    impossible.tests[0] =
        RuleTest{RuleInput::Altitude, 900.0F, 1000.0F, 1200.0F, 1300.0F, 0, false};
    impossible.test_count = 1;
    CY_REQUIRE(rules.rules.push_back(impossible).has_value());

    PlacementSampler sampler(world.query, world.fields, FieldBindings::standard(),
                             test::test_sun());
    const GenerationContext context = test::context_for(library, rules, sampler, 11);
    auto population = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    CY_REQUIRE(population.has_value());
    CY_CHECK_EQ(population.value().cluster.size(), 0u);
    CY_CHECK_EQ(population.value().diagnostic.dominant_exclusion(),
                static_cast<cy::u32>(RuleInput::Altitude));
    CY_CHECK_GT(population.value().diagnostic.candidates, 0u);
}

CY_TEST_CASE("nothing is planted on a hole, and a hole is not merely an unresolved sample") {
    // `terrain`'s own rule: a hole is NOT a surface. A consumer that only checked `resolved` could
    // plant a tree in a cave mouth, and this is the case that says foliage does not.
    test::TestWorld world;
    CY_REQUIRE(world.build(0, 0, 0, 0).has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    PlacementRuleSet rules(test::allocator());
    CY_REQUIRE(test::author_rules(rules).has_value());
    PlacementSampler sampler(world.query, world.fields, FieldBindings::standard(),
                             test::test_sun());
    const GenerationContext context = test::context_for(library, rules, sampler, 21);

    auto before = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    CY_REQUIRE(before.has_value());
    CY_REQUIRE((before.value().cluster.size()) > (20u));
    CY_CHECK_EQ(before.value().diagnostic.rejected_hole, 0u);

    // Punch a hole over most of the tile and regenerate. The same seed, the same rules; only the
    // terrain changed.
    CY_REQUIRE(
        test::punch_hole(world.terrain_store, cy::terrain::TileCoord{0, 0, 0}, 0, 55).has_value());
    auto after = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    CY_REQUIRE(after.has_value());
    CY_CHECK_GT(after.value().diagnostic.rejected_hole, 0u);
    CY_CHECK_LT(after.value().cluster.size(), before.value().cluster.size());
}

CY_TEST_CASE("an exclusion zone suppresses rules inside it and feathers at its edge") {
    test::TestWorld world;
    CY_REQUIRE(world.build(-1, -1, 2, 2).has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    PlacementRuleSet rules(test::allocator());
    CY_REQUIRE(test::author_rules(rules).has_value());
    PlacementSampler sampler(world.query, world.fields, FieldBindings::standard(),
                             test::test_sun());

    const GenerationContext open = test::context_for(library, rules, sampler, 31);
    auto unblocked = generate_region(test::allocator(), open, ClusterCoord{0, 0, 0});
    CY_REQUIRE(unblocked.has_value());

    ExclusionZone road;
    road.min_x = 0.0;
    road.min_z = 0.0;
    road.max_x = 64.0;
    road.max_z = 32.0;
    road.feather_metres = 4.0F;
    CY_REQUIRE(rules.exclusions.push_back(road).has_value());
    auto blocked = generate_region(test::allocator(), open, ClusterCoord{0, 0, 0});
    CY_REQUIRE(blocked.has_value());
    CY_CHECK_LT(blocked.value().cluster.size(), unblocked.value().cluster.size());
    CY_CHECK_GT(blocked.value().diagnostic.rejected_exclusion_zone, 0u);

    // Nothing survives inside the zone's interior.
    cy::u32 inside = 0;
    for (cy::u32 slot = 0; slot < static_cast<cy::u32>(blocked.value().cluster.size()); ++slot) {
        const cy::world::WorldVec3d at =
            blocked.value().cluster.bounds().decode(*blocked.value().cluster.at(slot));
        if (at.x > 4.0 && at.x < 60.0 && at.z > 4.0 && at.z < 28.0) {
            ++inside;
        }
    }
    CY_CHECK_EQ(inside, 0u);
}

CY_TEST_CASE("placement reads the terrain's column, so a mesh cliff carries plants") {
    // The brief for this row: place against what TERRAIN actually produced. A `MeshSource` arch is
    // a second representation whose surfaces a heightfield does not have, and a placement written
    // against an assumed heightmap would put nothing on it.
    test::TestWorld world;
    CY_REQUIRE(world.build(0, 0, 0, 0).has_value());
    cy::terrain::MeshSource ledge(test::allocator());
    // A flat ledge at 60 m over the whole region, above the hillside.
    cy::terrain::WorldTriangle first;
    first.a = cy::terrain::TerrainPoint{0.0, 0.0};
    first.b = cy::terrain::TerrainPoint{64.0, 0.0};
    first.c = cy::terrain::TerrainPoint{64.0, 64.0};
    first.height_a = 60.0F;
    first.height_b = 60.0F;
    first.height_c = 60.0F;
    cy::terrain::WorldTriangle second = first;
    second.b = cy::terrain::TerrainPoint{64.0, 64.0};
    second.c = cy::terrain::TerrainPoint{0.0, 64.0};
    CY_REQUIRE(ledge.add_triangle(first).has_value());
    CY_REQUIRE(ledge.add_triangle(second).has_value());
    CY_REQUIRE(world.query.add_source(ledge).has_value());

    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    PlacementRuleSet rules(test::allocator());
    CY_REQUIRE(test::author_rules(rules).has_value());
    PlacementSampler sampler(world.query, world.fields, FieldBindings::standard(),
                             test::test_sun());
    const GenerationContext context = test::context_for(library, rules, sampler, 41);

    auto population = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    CY_REQUIRE(population.has_value());
    CY_REQUIRE((population.value().cluster.size()) > (10u));
    // Every plant stands on the ledge, because `TerrainQuery::sample()` returns the TOPMOST surface
    // of the column and the ledge is above the ground.
    cy::u32 on_ledge = 0;
    for (cy::u32 slot = 0; slot < static_cast<cy::u32>(population.value().cluster.size()); ++slot) {
        const cy::world::WorldVec3d at =
            population.value().cluster.bounds().decode(*population.value().cluster.at(slot));
        on_ledge += at.y > 55.0 ? 1U : 0U;
    }
    CY_CHECK_EQ(on_ledge, static_cast<cy::u32>(population.value().cluster.size()));
}
