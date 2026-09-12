// Promotion and demotion: identity across the round trip, suppression, the four refusals, and the
// budget. M10 task 2.4; `foliage`'s "Promotion to entities" requirement.

#include <cy/test/test.h>

#include <cy/foliage/promotion.h>

#include "fixtures.h"

namespace test = cy::foliage::test;
using cy::foliage::ClusterBuilder;
using cy::foliage::ClusterCoord;
using cy::foliage::ClusterId;
using cy::foliage::DemotionRefusal;
using cy::foliage::DemotionState;
using cy::foliage::ExceptionKind;
using cy::foliage::ExceptionStore;
using cy::foliage::FoliageCluster;
using cy::foliage::InstanceFlags;
using cy::foliage::InstanceId;
using cy::foliage::PromotionBudget;
using cy::foliage::PromotionCause;
using cy::foliage::PromotionProblem;
using cy::foliage::PromotionRegistry;
using cy::foliage::species_id;
using cy::foliage::SpeciesLibrary;

namespace {

constexpr cy::u64 kSeed = 0xF0113A6E;

[[nodiscard]] cy::foliage::ClusterBounds stand_bounds() noexcept {
    cy::foliage::ClusterBounds bounds;
    bounds.min_x = 0.0;
    bounds.min_z = 0.0;
    bounds.max_x = 64.0;
    bounds.max_z = 64.0;
    bounds.min_y = 0.0F;
    bounds.max_y = 30.0F;
    return bounds;
}

/// A stand of pines with a few ferns in it. Pines are promotable; ferns are not.
[[nodiscard]] cy::Expected<FoliageCluster, cy::Error> build_stand() noexcept {
    ClusterBuilder builder(test::allocator(), test::policy(), ClusterId{0x515},
                           ClusterCoord{0, 0, 0}, stand_bounds());
    for (cy::u32 index = 0; index < 24; ++index) {
        const auto species = index % 4 == 3 ? species_id(test::kFern) : species_id(test::kPine);
        if (cy::Status added = builder.add(
                species,
                cy::world::WorldVec3d{(static_cast<cy::f64>(index % 6) * 8.0) + 2.0, 6.0,
                                      (static_cast<cy::f64>(index) / 6.0 * 8.0) + 2.0},
                0.4F, 0.5F, 1, 180, 0.0F, 0.0F, InstanceFlags{});
            !added) {
            return cy::make_unexpected(added.error());
        }
    }
    return builder.finish();
}

}  // namespace

CY_TEST_CASE("chopping a tree promotes it and suppresses its GPU instance") {
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    auto stand = build_stand();
    CY_REQUIRE(stand.has_value());
    PromotionRegistry registry(test::allocator(), library, kSeed);
    registry.begin_frame();

    // Find a pine.
    cy::u32 pine_slot = FoliageCluster::kNoSlot;
    for (cy::u32 slot = 0; slot < static_cast<cy::u32>(stand.value().size()); ++slot) {
        if (stand.value().species_at(stand.value().at(slot)->species_slot) ==
            species_id(test::kPine)) {
            pine_slot = slot;
            break;
        }
    }
    CY_REQUIRE_NE(pine_slot, FoliageCluster::kNoSlot);
    const InstanceId identity = stand.value().identity_at(kSeed, pine_slot);
    const cy::u32 drawable_before = stand.value().blocks()[0].drawable;

    auto order = registry.promote(stand.value(), identity, PromotionCause::Damage, 100);
    CY_REQUIRE(order.has_value());
    CY_CHECK_EQ(order.value().identity.value, identity.value);
    CY_CHECK_EQ(order.value().slot, pine_slot);
    CY_CHECK(order.value().species == species_id(test::kPine));
    // The order carries a decoded transform the caller can build an entity from.
    CY_CHECK_GT(order.value().scale, 0.0F);

    // Suppressed at promotion, not after the caller succeeds: a caller that took the order and then
    // failed would otherwise leave the instance drawn AND simulated.
    CY_CHECK(stand.value().at(pine_slot)->flags.has(InstanceFlags::kPromoted));
    CY_CHECK_FALSE(stand.value().at(pine_slot)->flags.drawable());
    cy::u32 drawable_now = 0;
    for (const cy::foliage::SpeciesBlock& block : stand.value().blocks()) {
        drawable_now += block.drawable;
    }
    CY_CHECK_EQ(drawable_now, static_cast<cy::u32>(stand.value().size()) - 1u);
    CY_CHECK_EQ(drawable_before - stand.value().blocks()[0].drawable, 1u);

    // Until the caller binds an entity the promotion is outstanding, and that is reportable.
    CY_CHECK_EQ(registry.unbound(), 1u);
    CY_REQUIRE(registry.bind(identity, cy::ecs::Entity::make(5, 1)).has_value());
    CY_CHECK_EQ(registry.unbound(), 0u);
    CY_CHECK_EQ(registry.diagnostics().promoted, 1u);
    CY_CHECK_EQ(registry.diagnostics().by_cause[static_cast<cy::u32>(PromotionCause::Damage)], 1u);
}

CY_TEST_CASE("a species with no gameplay surface is refused rather than given an entity") {
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    auto stand = build_stand();
    CY_REQUIRE(stand.has_value());
    PromotionRegistry registry(test::allocator(), library, kSeed);
    registry.begin_frame();

    cy::u32 fern_slot = FoliageCluster::kNoSlot;
    for (cy::u32 slot = 0; slot < static_cast<cy::u32>(stand.value().size()); ++slot) {
        if (stand.value().species_at(stand.value().at(slot)->species_slot) ==
            species_id(test::kFern)) {
            fern_slot = slot;
            break;
        }
    }
    CY_REQUIRE_NE(fern_slot, FoliageCluster::kNoSlot);
    auto refused = registry.promote(stand.value(), stand.value().identity_at(kSeed, fern_slot),
                                    PromotionCause::Physics, 1);
    CY_CHECK_FALSE(refused.has_value());
    CY_CHECK_EQ(registry.last_problem(), PromotionProblem::SpeciesNotPromotable);
    CY_CHECK_FALSE(stand.value().at(fern_slot)->flags.has(InstanceFlags::kPromoted));
}

CY_TEST_CASE("promotion and demotion are bounded per frame and by a ceiling") {
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    auto stand = build_stand();
    CY_REQUIRE(stand.has_value());
    PromotionRegistry registry(test::allocator(), library, kSeed);
    PromotionBudget budget;
    budget.promotions_per_frame = 3;
    budget.demotions_per_frame = 1;
    budget.max_promoted = 4;
    registry.set_budget(budget);
    registry.begin_frame();

    cy::u32 promoted = 0;
    cy::u32 refused = 0;
    for (cy::u32 slot = 0; slot < static_cast<cy::u32>(stand.value().size()); ++slot) {
        if (!(stand.value().species_at(stand.value().at(slot)->species_slot) ==
              species_id(test::kPine))) {
            continue;
        }
        auto order = registry.promote(stand.value(), stand.value().identity_at(kSeed, slot),
                                      PromotionCause::Felling, slot);
        if (order) {
            ++promoted;
        } else {
            ++refused;
        }
    }
    CY_CHECK_EQ(promoted, 3u);
    CY_CHECK_GT(refused, 0u);
    CY_CHECK_EQ(registry.last_problem(), PromotionProblem::BudgetExhausted);
    CY_CHECK_GT(registry.diagnostics().refused_budget, 0u);

    // A new frame restores the per-frame half but not the ceiling.
    registry.begin_frame();
    cy::u32 more = 0;
    for (cy::u32 slot = 0; slot < static_cast<cy::u32>(stand.value().size()); ++slot) {
        if (!(stand.value().species_at(stand.value().at(slot)->species_slot) ==
              species_id(test::kPine))) {
            continue;
        }
        if (registry.promote(stand.value(), stand.value().identity_at(kSeed, slot),
                             PromotionCause::Felling, slot)) {
            ++more;
        }
    }
    CY_CHECK_EQ(more, 1u);
    CY_CHECK_EQ(registry.size(), 4u);
    CY_CHECK_GT(registry.diagnostics().refused_ceiling, 0u);
}

CY_TEST_CASE("demotion is refused for each of the four states the specification names") {
    // `foliage` — "An instance whose state cannot be represented in the compact instance form —
    // MID-FALL, PARTIALLY DESTRUCTED, PHYSICALLY CONSTRAINED, or CARRYING GAMEPLAY STATE — SHALL
    // REMAIN AN ENTITY rather than losing that state."
    DemotionState falling;
    falling.mid_fall = true;
    CY_CHECK_EQ(PromotionRegistry::representable(falling), DemotionRefusal::MidFall);

    DemotionState broken;
    broken.partially_destructed = true;
    CY_CHECK_EQ(PromotionRegistry::representable(broken), DemotionRefusal::PartiallyDestructed);

    DemotionState roped;
    roped.constrained = true;
    CY_CHECK_EQ(PromotionRegistry::representable(roped), DemotionRefusal::PhysicallyConstrained);

    DemotionState quest;
    quest.gameplay_state = true;
    CY_CHECK_EQ(PromotionRegistry::representable(quest), DemotionRefusal::CarriesGameplayState);

    // Felled and destroyed ARE representable: sixteen bytes plus an exception carries both.
    DemotionState felled;
    felled.felled = true;
    CY_CHECK_EQ(PromotionRegistry::representable(felled), DemotionRefusal::None);
    DemotionState gone;
    gone.destroyed = true;
    CY_CHECK_EQ(PromotionRegistry::representable(gone), DemotionRefusal::None);
}

CY_TEST_CASE("an instance mid-fall stays an entity") {
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    auto stand = build_stand();
    CY_REQUIRE(stand.has_value());
    PromotionRegistry registry(test::allocator(), library, kSeed);
    registry.begin_frame();
    ExceptionStore exceptions(test::allocator());

    const InstanceId identity = stand.value().identity_at(kSeed, 0);
    CY_REQUIRE(registry.promote(stand.value(), identity, PromotionCause::Felling, 1).has_value());
    CY_REQUIRE(registry.bind(identity, cy::ecs::Entity::make(9, 1)).has_value());

    DemotionState falling;
    falling.mid_fall = true;
    auto refused = registry.demote(stand.value(), identity, falling, &exceptions, 1);
    CY_CHECK_FALSE(refused.has_value());
    CY_CHECK_EQ(registry.last_refusal(), DemotionRefusal::MidFall);
    // It is STILL PROMOTED — the instance did not quietly come back as a standing tree.
    CY_CHECK_EQ(registry.size(), 1u);
    CY_CHECK(stand.value().at(0)->flags.has(InstanceFlags::kPromoted));
    CY_CHECK_EQ(exceptions.size(), 0u);
}

CY_TEST_CASE("a felled tree stays felled across the round trip") {
    // `foliage` — "WHEN a felled tree's entity is demoted after the player leaves THEN its removal
    // or its fallen state SHALL BE RECORDED, and it SHALL NOT REAPPEAR STANDING."
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    auto stand = build_stand();
    CY_REQUIRE(stand.has_value());
    PromotionRegistry registry(test::allocator(), library, kSeed);
    registry.begin_frame();
    ExceptionStore exceptions(test::allocator());

    const InstanceId identity = stand.value().identity_at(kSeed, 0);
    CY_REQUIRE(registry.promote(stand.value(), identity, PromotionCause::Felling, 1).has_value());
    CY_REQUIRE(registry.bind(identity, cy::ecs::Entity::make(3, 1)).has_value());

    DemotionState felled;
    felled.felled = true;
    auto order = registry.demote(stand.value(), identity, felled, &exceptions, 7);
    CY_REQUIRE(order.has_value());
    CY_CHECK(order.value().recorded_exception);
    CY_CHECK_EQ(order.value().entity.bits(), cy::ecs::Entity::make(3, 1).bits());
    CY_CHECK_EQ(registry.size(), 0u);

    // The instance is back in the cluster, not promoted, and FELLED — and the exception recording
    // that is not the caller's to remember.
    CY_CHECK_FALSE(stand.value().at(0)->flags.has(InstanceFlags::kPromoted));
    CY_CHECK(stand.value().at(0)->flags.has(InstanceFlags::kFelled));
    CY_REQUIRE_EQ(exceptions.size(), 1u);
    const cy::Span<const cy::foliage::FoliageException> recorded =
        exceptions.of_cluster(ClusterId{0x515});
    CY_REQUIRE_EQ(recorded.size(), 1u);
    CY_CHECK_EQ(recorded[0].kind, ExceptionKind::Modified);
    CY_CHECK_EQ(recorded[0].identity.value, identity.value);
    CY_CHECK_FALSE(recorded[0].authored);

    // Demoting a state that must be recorded WITHOUT an exception store is refused rather than
    // silently dropping the state.
    registry.begin_frame();
    const InstanceId second = stand.value().identity_at(kSeed, 1);
    CY_REQUIRE(registry.promote(stand.value(), second, PromotionCause::Felling, 2).has_value());
    DemotionState destroyed;
    destroyed.destroyed = true;
    CY_CHECK_FALSE(registry.demote(stand.value(), second, destroyed, nullptr, 8).has_value());
}

CY_TEST_CASE("a promoted instance's identity survives the round trip unchanged") {
    // The property the specification is actually asking for: the identity that came out of the
    // promotion is the identity the cluster still derives afterwards, so damage and removal bind.
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    auto stand = build_stand();
    CY_REQUIRE(stand.has_value());
    PromotionRegistry registry(test::allocator(), library, kSeed);
    registry.begin_frame();
    ExceptionStore exceptions(test::allocator());

    const InstanceId identity = stand.value().identity_at(kSeed, 4);
    CY_REQUIRE(registry.promote(stand.value(), identity, PromotionCause::Script, 1).has_value());
    CY_REQUIRE(registry.bind(identity, cy::ecs::Entity::make(11, 2)).has_value());
    DemotionState plain;
    CY_REQUIRE(registry.demote(stand.value(), identity, plain, &exceptions, 3).has_value());

    CY_CHECK_EQ(stand.value().identity_at(kSeed, 4).value, identity.value);
    CY_CHECK_EQ(stand.value().slot_of(kSeed, identity), 4u);
    // Nothing was recorded, because nothing changed — a demotion that returns the plant unchanged
    // must not grow the save.
    CY_CHECK_EQ(exceptions.size(), 0u);
}
