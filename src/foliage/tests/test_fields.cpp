// Foliage as a producer and a consumer of the environment substrate: the claim, its refusal, the
// firewall, the ecosystem state, and a burned forest that costs a field region. M10 task 2.4.

#include <cy/test/test.h>

#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/foliage/placement.h>
#include <cy/foliage/system.h>

#include "fixtures.h"

#include <cstring>

namespace test = cy::foliage::test;
using cy::foliage::ClusterCoord;
using cy::foliage::ClusterStore;
using cy::foliage::FieldBindings;
using cy::foliage::FoliageCluster;
using cy::foliage::FoliageSystem;
using cy::foliage::generate_region;
using cy::foliage::GenerationContext;
using cy::foliage::PlacementRuleSet;
using cy::foliage::PlacementSampler;
using cy::foliage::regional_state_at;
using cy::foliage::RegionalState;
using cy::foliage::SpeciesLibrary;
using cy::foliage::StateBindings;
using cy::foliage::StateThresholds;
using cy::foliage::vegetation_field_declaration;

namespace {

constexpr cy::u64 kSeed = 0xBADF00D;

/// Declare a gameplay-visible scalar field, claim it and write one value everywhere in a rectangle
/// of macro tiles. The shape a weather row would produce; here it is the test's own producer.
[[nodiscard]] cy::Status publish_scalar(cy::environment::FieldRegistry& registry,
                                        cy::environment::FieldStore& store, const char* name,
                                        const char* producer, cy::f32 value, cy::f32 cell_metres,
                                        cy::f32 range_max) noexcept {
    cy::environment::FieldDeclaration declaration;
    declaration.name = name;
    declaration.unit = "fraction";
    declaration.semantics = "a test field";
    declaration.type = cy::environment::FieldType::Scalar;
    declaration.encoding = cy::environment::FieldEncoding::UNorm8;
    declaration.range_min = 0.0F;
    declaration.range_max = range_max;
    declaration.levels[static_cast<cy::u32>(cy::environment::FieldResidency::Macro)] =
        cy::environment::FieldLevel{cell_metres, true};
    declaration.classification = cy::determinism::SimulationClass::Persistent;
    declaration.gameplay_level = cy::environment::FieldResidency::Macro;
    if (cy::Status declared = registry.declare(declaration); !declared) {
        return declared;
    }
    auto token = registry.claim(cy::environment::field_id(name), producer,
                                cy::environment::ProducerKind::System);
    if (!token) {
        return cy::make_unexpected(token.error());
    }
    auto writer = store.open_writer(token.value());
    if (!writer) {
        return cy::make_unexpected(writer.error());
    }
    for (cy::i32 z = -2; z <= 2; ++z) {
        for (cy::i32 x = -2; x <= 2; ++x) {
            cy::environment::TileAddress address;
            address.field = cy::environment::field_id(name);
            address.x = x;
            address.z = z;
            address.level = static_cast<cy::u8>(cy::environment::FieldResidency::Macro);
            address.layer = static_cast<cy::u8>(cy::environment::FieldLayer::Base);
            if (cy::Status staged = writer.value().stage(address); !staged) {
                return staged;
            }
            if (cy::Status filled =
                    writer.value().fill(address, cy::environment::FieldValue::scalar(value));
                !filled) {
                return filled;
            }
        }
    }
    return writer.value().publish();
}

}  // namespace

CY_TEST_CASE("a second producer of the vegetation field fails, naming both") {
    // `environment-fields` — "Two systems writing one field SHALL be a configuration error detected
    // at startup or cook time". Foliage does not get an exemption from the rule it is the second
    // user of, and this is the case that says so.
    cy::environment::FieldRegistry registry(test::allocator());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());

    FoliageSystem first(test::allocator(), library);
    CY_REQUIRE(first.register_producer(registry, "foliage.ecosystem", 64.0F, 0.001F).has_value());
    CY_CHECK(first.produces_vegetation());

    FoliageSystem second(test::allocator(), library);
    CY_CHECK_FALSE(second.register_producer(registry, "mod.overgrowth", 64.0F, 0.001F).has_value());
    CY_CHECK_FALSE(second.produces_vegetation());
    CY_CHECK_EQ(registry.last_conflict().incumbent, "foliage.ecosystem");
    CY_CHECK_EQ(registry.last_conflict().challenger, "mod.overgrowth");
    CY_CHECK_EQ(registry.conflict_count(), 1u);
}

CY_TEST_CASE("the vegetation field is declared with the properties its two scenarios need") {
    const cy::environment::FieldDeclaration declaration =
        vegetation_field_declaration(64.0F, 0.01F);
    CY_CHECK_EQ(cy::environment::validate_declaration(declaration),
                cy::environment::DeclarationProblem::None);
    // Gameplay-visible, so placement's answer cannot depend on what streamed.
    CY_CHECK(declaration.gameplay_visible());
    // Persistent, so a burned forest survives a save.
    CY_CHECK(declaration.persistent);
    // Macro-resident everywhere, which is what lets an UNLOADED region's ecosystem evolve.
    CY_CHECK(declaration.levels[static_cast<cy::u32>(cy::environment::FieldResidency::Macro)]
                 .resident_everywhere);
    // And it recovers toward a potential rather than being repainted.
    CY_CHECK(declaration.potential.is_valid());
    CY_CHECK_GT(declaration.recovery_per_second, 0.0F);
    // CPU-produced: `validate_declaration()` refuses a gameplay-visible field written by a GPU
    // pass, and a declaration that asked for one would be refused rather than merely unwise.
    cy::environment::FieldDeclaration on_gpu = declaration;
    on_gpu.production = cy::environment::FieldProduction::Gpu;
    CY_CHECK_EQ(cy::environment::validate_declaration(on_gpu),
                cy::environment::DeclarationProblem::GpuAuthoritative);
}

CY_TEST_CASE("a grown forest appears grown and a depleted one appears depleted") {
    // `foliage` — "WHEN a region whose vegetation state increased while unloaded is materialised
    // THEN the generated foliage SHALL REFLECT THAT STATE."
    test::TestWorld world;
    CY_REQUIRE(world.build(-2, -2, 3, 3).has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    PlacementRuleSet rules(test::allocator());
    CY_REQUIRE(test::author_rules(rules).has_value());

    FoliageSystem system(test::allocator(), library);
    CY_REQUIRE(
        system.register_producer(world.registry, "foliage.ecosystem", 64.0F, 0.01F).has_value());

    // The vegetation state is published through the system's OWN producer path below — there is no
    // other way in, because `FieldStore` accepts a write only through a `ProducerToken` and the
    // system holds the only one.
    PlacementSampler sampler(world.query, world.fields, FieldBindings::standard(),
                             test::test_sun());
    const GenerationContext context = test::context_for(library, rules, sampler, kSeed);
    auto full = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    CY_REQUIRE(full.has_value());
    const cy::usize with_no_field = full.value().cluster.size();
    CY_REQUIRE((with_no_field) > (40u));

    // Now publish a real vegetation state through the system's own producer path, at a quarter.
    ClusterStore clusters(test::allocator());
    cy::foliage::ClusterPolicy sparse_policy = test::policy();
    sparse_policy.target_instances = static_cast<cy::u32>(with_no_field) * 4;
    auto cluster_copy = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    CY_REQUIRE(cluster_copy.has_value());
    CY_REQUIRE(
        clusters.insert(static_cast<FoliageCluster&&>(cluster_copy.value().cluster)).has_value());
    auto tiles = system.publish_vegetation(world.fields, clusters, sparse_policy,
                                           cy::environment::FieldResidency::Macro);
    CY_REQUIRE(tiles.has_value());
    CY_CHECK_GT(tiles.value(), 0u);

    // The same seed and the same rules, against a world that now says the region carries a quarter
    // of what it could: fewer plants, and the difference is the field rather than the rules.
    auto depleted = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    CY_REQUIRE(depleted.has_value());
    CY_CHECK_LT(depleted.value().cluster.size(), with_no_field);
    CY_CHECK_GT(depleted.value().cluster.size(), 0u);

    // And a cook that wants the POTENTIAL rather than the current state asks for it and gets the
    // full stand back.
    GenerationContext potential = context;
    potential.apply_ecosystem_state = false;
    auto cooked = generate_region(test::allocator(), potential, ClusterCoord{0, 0, 0});
    CY_REQUIRE(cooked.has_value());
    CY_CHECK_EQ(cooked.value().cluster.size(), with_no_field);
}

CY_TEST_CASE("a forest burns without a single per-instance write") {
    // `foliage` — "Regional state SHALL be READ FROM FIELDS rather than stored per instance, so
    // that a BURNED FOREST COSTS A FIELD REGION rather than a million instance updates."
    //
    // The measurement is the ABSENCE: the cluster's instance bytes are compared with a memcmp
    // before and after the fire.
    test::TestWorld world;
    CY_REQUIRE(world.build(-1, -1, 2, 2).has_value());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    PlacementRuleSet rules(test::allocator());
    CY_REQUIRE(test::author_rules(rules).has_value());
    PlacementSampler sampler(world.query, world.fields, FieldBindings::standard(),
                             test::test_sun());
    const GenerationContext context = test::context_for(library, rules, sampler, kSeed);
    auto population = generate_region(test::allocator(), context, ClusterCoord{0, 0, 0});
    CY_REQUIRE(population.has_value());
    const FoliageCluster& forest = population.value().cluster;
    CY_REQUIRE((forest.size()) > (20u));

    const StateBindings bindings = StateBindings::standard();
    const StateThresholds thresholds;
    const cy::world::WorldVec3d at{32.0, 10.0, 32.0};
    // No burn field declared yet: the state is whatever the other fields say, which is Normal.
    CY_CHECK_EQ(regional_state_at(world.fields, bindings, thresholds, at).state,
                RegionalState::Normal);

    // Snapshot the instances.
    cy::Array<cy::foliage::FoliageInstance> before(test::allocator());
    CY_REQUIRE(before.append(forest.instances()).has_value());

    // The fire: one field, one producer, one publish.
    CY_REQUIRE(publish_scalar(world.registry, world.fields, cy::environment::fields::kBurnState,
                              "fire.spread", 0.9F, 64.0F, 1.0F)
                   .has_value());
    const cy::foliage::RegionalStateSample burning =
        regional_state_at(world.fields, bindings, thresholds, at);
    CY_CHECK_EQ(burning.state, RegionalState::Burning);
    CY_CHECK_GT(burning.burn, 0.8F);
    // The diagnostic answers with the NUMBER, not only the conclusion.
    CY_CHECK_GT(burning.version, 0u);

    // And not one instance changed.
    CY_REQUIRE_EQ(before.size(), forest.instances().size());
    CY_CHECK_EQ(std::memcmp(before.data(), forest.instances().data(),
                            before.size() * sizeof(cy::foliage::FoliageInstance)),
                0);
}

CY_TEST_CASE("the regional state reads every field the requirement names") {
    cy::environment::FieldRegistry registry(test::allocator());
    cy::environment::FieldStore store(test::allocator(), registry, test::partition());
    const StateBindings bindings = StateBindings::standard();
    StateThresholds thresholds;

    CY_REQUIRE(publish_scalar(registry, store, cy::environment::fields::kSnowDepth, "weather.snow",
                              0.4F, 64.0F, 2.0F)
                   .has_value());
    const cy::world::WorldVec3d at{10.0, 2.0, 10.0};
    CY_CHECK_EQ(regional_state_at(store, bindings, thresholds, at).state,
                RegionalState::SnowCovered);

    // Burning outranks snow: a burning tree under snow is burning.
    CY_REQUIRE(publish_scalar(registry, store, cy::environment::fields::kBurnState, "fire.spread",
                              0.8F, 64.0F, 1.0F)
                   .has_value());
    CY_CHECK_EQ(regional_state_at(store, bindings, thresholds, at).state, RegionalState::Burning);

    // A world with no fields at all answers Normal rather than faulting.
    cy::environment::FieldRegistry empty(test::allocator());
    cy::environment::FieldStore nothing(test::allocator(), empty, test::partition());
    CY_CHECK_EQ(regional_state_at(nothing, bindings, thresholds, at).state, RegionalState::Normal);
}

CY_TEST_CASE("the determinism firewall reports a placement rule that reads a visual field") {
    // `environment-fields` — a visual field "SHALL be declared visual, and gameplay SHALL be
    // PREVENTED FROM READING IT BY CONFIGURATION VALIDATION". Placement is gameplay-visible, so a
    // rule bound to a presentation field is a crossing `validate()` must report before a frame
    // runs.
    cy::environment::FieldRegistry registry(test::allocator());
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());

    cy::environment::FieldDeclaration visual;
    visual.name = cy::environment::fields::kMoisture;
    visual.unit = "fraction";
    visual.semantics = "a presentation-only moisture, for a puddle shader";
    visual.type = cy::environment::FieldType::Scalar;
    visual.encoding = cy::environment::FieldEncoding::UNorm8;
    visual.levels[static_cast<cy::u32>(cy::environment::FieldResidency::Macro)] =
        cy::environment::FieldLevel{64.0F, true};
    visual.classification = cy::determinism::SimulationClass::Presentation;
    CY_REQUIRE(registry.declare(visual).has_value());

    PlacementRuleSet rules(test::allocator());
    cy::foliage::PlacementRule wet;
    wet.species = cy::foliage::species_id(test::kFern);
    wet.density_per_hectare = 100.0F;
    wet.tests[0] =
        cy::foliage::RuleTest{cy::foliage::RuleInput::Moisture, 0.0F, 0.3F, 1.0F, 1.0F, 0, false};
    wet.test_count = 1;
    CY_REQUIRE(rules.rules.push_back(wet).has_value());

    FoliageSystem system(test::allocator(), library);
    CY_REQUIRE(system
                   .declare_consumption(registry, "foliage.placement", rules,
                                        FieldBindings::standard(), StateBindings::standard())
                   .has_value());
    cy::Array<cy::environment::FirewallViolation> violations(test::allocator());
    CY_REQUIRE(registry.validate(violations).has_value());
    CY_REQUIRE_EQ(violations.size(), 1u);
    CY_CHECK_EQ(violations[0].consumer, "foliage.placement");
    CY_CHECK_EQ(violations[0].field_class, cy::determinism::SimulationClass::Presentation);

    // The same rule against a gameplay-visible moisture field is a valid configuration, so the
    // report above is about the CLASS rather than about foliage reading fields at all.
    cy::environment::FieldRegistry authoritative(test::allocator());
    cy::environment::FieldDeclaration real = visual;
    real.classification = cy::determinism::SimulationClass::Persistent;
    real.gameplay_level = cy::environment::FieldResidency::Macro;
    CY_REQUIRE(authoritative.declare(real).has_value());
    FoliageSystem clean(test::allocator(), library);
    CY_REQUIRE(clean
                   .declare_consumption(authoritative, "foliage.placement", rules,
                                        FieldBindings::standard(), StateBindings::standard())
                   .has_value());
    cy::Array<cy::environment::FirewallViolation> none(test::allocator());
    CY_REQUIRE(authoritative.validate(none).has_value());
    CY_CHECK_EQ(none.size(), 0u);
}
