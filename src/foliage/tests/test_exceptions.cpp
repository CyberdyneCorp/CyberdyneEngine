// Exceptions: storage proportional to change, re-resolution across a rule change, orphans that are
// reported rather than dropped, and the world's own persistence overlay. M10 task 2.4.

#include <cy/test/test.h>

#include <cy/foliage/exceptions.h>
#include <cy/foliage/placement.h>
#include <cy/foliage/system.h>

#include "fixtures.h"

namespace test = cy::foliage::test;
using cy::foliage::ClusterCoord;
using cy::foliage::ClusterId;
using cy::foliage::ExceptionKind;
using cy::foliage::ExceptionStore;
using cy::foliage::FieldBindings;
using cy::foliage::FoliageCluster;
using cy::foliage::FoliageException;
using cy::foliage::FoliageSystem;
using cy::foliage::generate_region;
using cy::foliage::GenerationContext;
using cy::foliage::InstanceFlags;
using cy::foliage::InstanceId;
using cy::foliage::PlacementRuleSet;
using cy::foliage::PlacementSampler;
using cy::foliage::Resolution;
using cy::foliage::ResolutionOutcome;
using cy::foliage::ResolutionPolicy;
using cy::foliage::species_id;
using cy::foliage::SpeciesLibrary;

namespace {

constexpr cy::u64 kSeed = 0xE7CE9710;

[[nodiscard]] FoliageException fell(const FoliageCluster& cluster, cy::u32 slot,
                                    cy::u64 sequence) noexcept {
    FoliageException exception;
    exception.kind = ExceptionKind::Removed;
    exception.identity = cluster.identity_at(kSeed, slot);
    exception.cluster = cluster.id();
    exception.position = cluster.bounds().decode(*cluster.at(slot));
    exception.species = cluster.species_at(cluster.at(slot)->species_slot);
    exception.sequence = sequence;
    return exception;
}

}  // namespace

CY_TEST_CASE("a save records fifty exceptions, not a hundred thousand instances") {
    // `foliage` — "WHEN a player fells fifty trees in a forest of a hundred thousand THEN the save
    // SHALL RECORD FIFTY EXCEPTIONS, not a hundred thousand instances."
    test::TestWorld world;
    CY_REQUIRE(world.build(-2, -2, 3, 3).has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    PlacementRuleSet rules(test::allocator());
    CY_REQUIRE(test::author_rules(rules).has_value());
    PlacementSampler sampler(world.query, world.fields, FieldBindings::standard(),
                             test::test_sun());
    const GenerationContext context = test::context_for(library, rules, sampler, kSeed);

    // Nine regions, so the forest is large enough for the comparison to measure something: felling
    // fifty trees in a stand of sixty would be a saving nobody would notice.
    cy::Array<cy::foliage::FoliagePopulation> forest(test::allocator());
    cy::u64 instance_bytes = 0;
    cy::u64 instances = 0;
    for (cy::i32 z = -1; z <= 1; ++z) {
        for (cy::i32 x = -1; x <= 1; ++x) {
            auto population = generate_region(test::allocator(), context, ClusterCoord{x, z, 0});
            CY_REQUIRE(population.has_value());
            instance_bytes += population.value().cluster.bytes();
            instances += population.value().cluster.size();
            CY_REQUIRE(
                forest.push_back(static_cast<cy::foliage::FoliagePopulation&&>(population.value()))
                    .has_value());
        }
    }
    CY_REQUIRE((instances) > (500u));

    ExceptionStore exceptions(test::allocator());
    cy::u32 recorded = 0;
    for (const cy::foliage::FoliagePopulation& region : forest) {
        for (cy::u32 slot = 0; slot < 6 && recorded < 50; ++slot) {
            if (slot >= region.cluster.size()) {
                break;
            }
            CY_REQUIRE(exceptions.record(fell(region.cluster, slot, recorded)).has_value());
            ++recorded;
        }
    }
    CY_CHECK_EQ(recorded, 50u);
    CY_CHECK_EQ(exceptions.size(), 50u);
    // The comparison the requirement is about, in bytes: the exceptions are a fraction of what the
    // instances would cost if the forest were serialised instance by instance — and the saving
    // grows with the forest rather than with the number of trees felled.
    CY_CHECK_LT(exceptions.bytes(), instance_bytes);
    CY_CHECK_EQ(exceptions.cluster_count(), 9u);

    // Felling the same tree twice is one exception, so a replay that re-applies an event does not
    // grow the save.
    CY_REQUIRE(exceptions.record(fell(forest[0].cluster, 3, 900)).has_value());
    CY_CHECK_EQ(exceptions.size(), 50u);
}

CY_TEST_CASE("an exception binds by identity across a regeneration of the same seed") {
    test::TestWorld world;
    CY_REQUIRE(world.build(-2, -2, 3, 3).has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    PlacementRuleSet rules(test::allocator());
    CY_REQUIRE(test::author_rules(rules).has_value());
    PlacementSampler sampler(world.query, world.fields, FieldBindings::standard(),
                             test::test_sun());
    const GenerationContext context = test::context_for(library, rules, sampler, kSeed);

    auto first = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    CY_REQUIRE(first.has_value());
    ExceptionStore exceptions(test::allocator());
    for (cy::u32 slot = 0; slot < 8; ++slot) {
        CY_REQUIRE(exceptions.record(fell(first.value().cluster, slot * 3, slot)).has_value());
    }

    // Regenerate and re-resolve. The spike's own number is what this has to beat: 7 877 overrides,
    // ZERO mis-bound under a derived identity.
    auto regenerated = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    CY_REQUIRE(regenerated.has_value());
    cy::Array<Resolution> resolved(test::allocator());
    auto report = exceptions.resolve(kSeed, first.value().cluster.id(), regenerated.value().cluster,
                                     ResolutionPolicy{}, resolved);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().examined, 8u);
    CY_CHECK_EQ(report.value().bound_by_identity, 8u);
    CY_CHECK_EQ(report.value().orphans(), 0u);
    // And each one bound to the slot it names, not merely to A slot.
    for (cy::u32 index = 0; index < 8; ++index) {
        CY_CHECK_EQ(resolved[index].slot, index * 3);
        CY_CHECK_EQ(resolved[index].drift_metres, 0.0F);
    }

    auto applied =
        exceptions.apply(first.value().cluster.id(), resolved.span(), regenerated.value().cluster);
    CY_REQUIRE(applied.has_value());
    CY_CHECK_EQ(applied.value(), 8u);
    for (cy::u32 index = 0; index < 8; ++index) {
        CY_CHECK(regenerated.value().cluster.at(index * 3)->flags.has(InstanceFlags::kRemoved));
        CY_CHECK_FALSE(regenerated.value().cluster.at(index * 3)->flags.drawable());
    }
}

CY_TEST_CASE("a rule change orphans an exception rather than dropping it") {
    // `foliage` — "WHEN a rule change removes the tree an exception referred to THEN the exception
    // SHALL BE REPORTED AS ORPHANED rather than dropped, and SHALL be resolvable by an author."
    test::TestWorld world;
    CY_REQUIRE(world.build(-2, -2, 3, 3).has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    PlacementRuleSet rules(test::allocator());
    CY_REQUIRE(test::author_rules(rules).has_value());
    PlacementSampler sampler(world.query, world.fields, FieldBindings::standard(),
                             test::test_sun());
    GenerationContext context = test::context_for(library, rules, sampler, kSeed);

    auto before = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    CY_REQUIRE(before.has_value());
    ExceptionStore exceptions(test::allocator());
    // Fell a boulder — a sparse species, so a spatial re-bind will not find another of its kind
    // within the radius after the graph changes.
    cy::u32 boulder_slot = FoliageCluster::kNoSlot;
    for (cy::u32 slot = 0; slot < static_cast<cy::u32>(before.value().cluster.size()); ++slot) {
        if (before.value().cluster.species_at(before.value().cluster.at(slot)->species_slot) ==
            species_id(test::kBoulder)) {
            boulder_slot = slot;
            break;
        }
    }
    CY_REQUIRE_NE(boulder_slot, FoliageCluster::kNoSlot);
    CY_REQUIRE(exceptions.record(fell(before.value().cluster, boulder_slot, 1)).has_value());

    // The author edits the rule graph and removes the boulder rule entirely.
    PlacementRuleSet edited(test::allocator());
    CY_REQUIRE(edited.rules.push_back(rules.rules[0]).has_value());
    CY_REQUIRE(edited.rules.push_back(rules.rules[1]).has_value());
    edited.graph_version = 2;
    context.rules = &edited;

    auto after = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    CY_REQUIRE(after.has_value());
    // The cluster is RENAMED by the version bump, which is what forces the re-resolution path.
    CY_CHECK_NE(after.value().cluster.id().value, before.value().cluster.id().value);

    cy::Array<Resolution> resolved(test::allocator());
    ResolutionPolicy policy;
    policy.radius_metres = 1.5F;
    auto report = exceptions.resolve(kSeed, before.value().cluster.id(), after.value().cluster,
                                     policy, resolved);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().examined, 1u);
    CY_CHECK_EQ(report.value().bound_by_identity, 0u);
    CY_CHECK_EQ(report.value().orphans(), 1u);

    // Retained and reportable, not discarded. The store still holds it.
    cy::Array<FoliageException> orphans(test::allocator());
    CY_REQUIRE(
        exceptions.orphans(before.value().cluster.id(), resolved.span(), orphans).has_value());
    CY_CHECK_EQ(orphans.size(), 1u);
    CY_CHECK_EQ(exceptions.size(), 1u);
    CY_CHECK(orphans[0].species == species_id(test::kBoulder));
}

CY_TEST_CASE("two equally good spatial matches are an orphan, not a coin toss") {
    // The failure mode the spike's override table is about: a wrong binding silently moves a
    // DIFFERENT object and nobody ever finds it. An ambiguous match is refused instead.
    cy::foliage::ClusterBounds bounds;
    bounds.min_x = 0.0;
    bounds.min_z = 0.0;
    bounds.max_x = 64.0;
    bounds.max_z = 64.0;
    bounds.max_y = 10.0F;
    cy::foliage::ClusterBuilder builder(test::allocator(), test::policy(), ClusterId{900},
                                        ClusterCoord{0, 0, 0}, bounds);
    // Two pines exactly equidistant from the anchor below.
    CY_REQUIRE(builder
                   .add(species_id(test::kPine), cy::world::WorldVec3d{9.0, 2.0, 10.0}, 0.0F, 0.5F,
                        0, 100, 0.0F, 0.0F, InstanceFlags{})
                   .has_value());
    CY_REQUIRE(builder
                   .add(species_id(test::kPine), cy::world::WorldVec3d{11.0, 2.0, 10.0}, 0.0F, 0.5F,
                        0, 100, 0.0F, 0.0F, InstanceFlags{})
                   .has_value());
    auto cluster = builder.finish();
    CY_REQUIRE(cluster.has_value());

    ExceptionStore exceptions(test::allocator());
    FoliageException exception;
    exception.kind = ExceptionKind::Removed;
    // An identity from a cluster that no longer exists: the identity anchor cannot bind.
    exception.identity = InstanceId{0xDEAD'BEEF};
    exception.cluster = ClusterId{899};
    exception.position = cy::world::WorldVec3d{10.0, 2.0, 10.0};
    exception.species = species_id(test::kPine);
    CY_REQUIRE(exceptions.record(exception).has_value());

    cy::Array<Resolution> resolved(test::allocator());
    ResolutionPolicy policy;
    policy.radius_metres = 4.0F;
    policy.ambiguity_metres = 0.5F;
    auto report = exceptions.resolve(kSeed, ClusterId{899}, cluster.value(), policy, resolved);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().ambiguous, 1u);
    CY_CHECK_EQ(resolved[0].outcome, ResolutionOutcome::Ambiguous);
    CY_CHECK_EQ(resolved[0].slot, FoliageCluster::kNoSlot);

    // Move one of them away and the same exception binds unambiguously — so the refusal above is
    // about ambiguity rather than about the spatial path being broken.
    cy::foliage::ClusterBuilder second(test::allocator(), test::policy(), ClusterId{901},
                                       ClusterCoord{0, 0, 0}, bounds);
    CY_REQUIRE(second
                   .add(species_id(test::kPine), cy::world::WorldVec3d{9.8, 2.0, 10.0}, 0.0F, 0.5F,
                        0, 100, 0.0F, 0.0F, InstanceFlags{})
                   .has_value());
    CY_REQUIRE(second
                   .add(species_id(test::kPine), cy::world::WorldVec3d{30.0, 2.0, 40.0}, 0.0F, 0.5F,
                        0, 100, 0.0F, 0.0F, InstanceFlags{})
                   .has_value());
    auto moved = second.finish();
    CY_REQUIRE(moved.has_value());
    cy::Array<Resolution> rebound(test::allocator());
    auto again = exceptions.resolve(kSeed, ClusterId{899}, moved.value(), policy, rebound);
    CY_REQUIRE(again.has_value());
    CY_CHECK_EQ(again.value().bound_by_space, 1u);
    CY_CHECK_LT(rebound[0].drift_metres, 0.5F);

    // And a spatial re-bind is refusable outright, for a project that would rather see orphans.
    ResolutionPolicy strict = policy;
    strict.allow_spatial = false;
    cy::Array<Resolution> strict_resolved(test::allocator());
    auto strict_report =
        exceptions.resolve(kSeed, ClusterId{899}, moved.value(), strict, strict_resolved);
    CY_REQUIRE(strict_report.has_value());
    CY_CHECK_EQ(strict_report.value().orphaned, 1u);
}

CY_TEST_CASE(
    "runtime exceptions go through the world's persistence overlay and authored ones do not") {
    cy::world::PersistenceOverlay overlay(test::allocator());
    const cy::world::CellId cell =
        cy::world::cell_id_of(test::partition(), cy::world::CellCoord{0, 0, 0, 0});

    ExceptionStore exceptions(test::allocator());
    for (cy::u32 index = 0; index < 6; ++index) {
        FoliageException exception;
        exception.kind = ExceptionKind::Removed;
        exception.identity = InstanceId{100 + index};
        exception.cluster = ClusterId{55};
        exception.position = cy::world::WorldVec3d{static_cast<cy::f64>(index), 1.0, 2.0};
        exception.species = species_id(test::kPine);
        // Two of them are AUTHORING data: cooked, and writing them to a save would grow every save
        // by the whole authored set.
        exception.authored = index >= 4;
        exception.sequence = index;
        CY_REQUIRE(exceptions.record(exception).has_value());
    }

    auto written = exceptions.write_overlay(overlay, cell, ClusterId{55});
    CY_REQUIRE(written.has_value());
    CY_CHECK_EQ(written.value(), 4u);
    CY_CHECK_EQ(!overlay.blob(cell, cy::foliage::kOverlayChannelFoliage, 55).empty(), true);

    // Read back into a fresh store: the four runtime exceptions come back and the two authored ones
    // are not in the save at all.
    ExceptionStore restored(test::allocator());
    auto read = restored.read_overlay(overlay, cell, ClusterId{55});
    CY_REQUIRE(read.has_value());
    CY_CHECK_EQ(read.value(), 4u);
    CY_CHECK_EQ(restored.size(), 4u);
    for (const FoliageException& exception : restored.of_cluster(ClusterId{55})) {
        CY_CHECK_FALSE(exception.authored);
        CY_CHECK(exception.species == species_id(test::kPine));
    }

    // A cluster nothing was written for reads back as nothing rather than as an error.
    auto absent = restored.read_overlay(overlay, cell, ClusterId{56});
    CY_REQUIRE(absent.has_value());
    CY_CHECK_EQ(absent.value(), 0u);
}

CY_TEST_CASE("an added exception survives regeneration and changes the cluster's slots") {
    // An ADDED instance has to go in before the cluster is finished, because it changes slot
    // numbering — and a slot is what every other identity is derived from.
    test::TestWorld world;
    CY_REQUIRE(world.build(-2, -2, 3, 3).has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    PlacementRuleSet rules(test::allocator());
    CY_REQUIRE(test::author_rules(rules).has_value());
    PlacementSampler sampler(world.query, world.fields, FieldBindings::standard(),
                             test::test_sun());
    const GenerationContext context = test::context_for(library, rules, sampler, kSeed);

    auto plain = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    CY_REQUIRE(plain.has_value());
    const cy::usize generated = plain.value().cluster.size();

    ExceptionStore exceptions(test::allocator());
    FoliageException painted;
    painted.kind = ExceptionKind::Added;
    painted.cluster = plain.value().cluster.id();
    painted.species = species_id(test::kPine);
    painted.authored = true;
    plain.value().cluster.bounds().encode(cy::world::WorldVec3d{20.0, 12.0, 20.0},
                                          painted.instance);
    painted.instance.scale = 40000;
    painted.instance.variation = 2;
    CY_REQUIRE(exceptions.record(painted).has_value());

    FoliageSystem system(test::allocator(), library);
    auto materialised = system.materialise(context, ClusterCoord{0, 0, 0}, &exceptions,
                                           ResolutionPolicy{}, plain.value().cluster.id());
    CY_REQUIRE(materialised.has_value());
    CY_CHECK_EQ(materialised.value().cluster.size(), generated + 1);
    CY_CHECK_EQ(materialised.value().exceptions.standalone, 1u);

    // The painted instance is in there, marked as one.
    cy::u32 painted_count = 0;
    for (cy::u32 slot = 0; slot < static_cast<cy::u32>(materialised.value().cluster.size());
         ++slot) {
        painted_count +=
            materialised.value().cluster.at(slot)->flags.has(InstanceFlags::kPainted) ? 1U : 0U;
    }
    CY_CHECK_EQ(painted_count, 1u);
}
