// The foliage allocation, the declared reduction order, and the species the order never reaches.
// M10 task 2.4; `foliage`'s "Foliage budget" requirement.

#include <cy/test/test.h>

#include <cy/foliage/budget.h>

#include "fixtures.h"

namespace test = cy::foliage::test;
using cy::foliage::BudgetAxis;
using cy::foliage::FoliageBudget;
using cy::foliage::FoliageLadder;
using cy::foliage::FoliageSettings;
using cy::foliage::kMaxBudgetPositions;
using cy::foliage::ReductionStep;
using cy::foliage::SpeciesClass;

namespace {

[[nodiscard]] FoliageSettings richest() noexcept {
    FoliageSettings settings;
    settings.grass.distance_metres = 60.0F;
    settings.grass.density_scale = 1.0F;
    settings.grass.max_blades = 1'000'000;
    settings.interaction.trail_cells = 256;
    settings.tier_threshold_scale = 1.0F;
    return settings;
}

}  // namespace

CY_TEST_CASE("ground cover is reduced before trees are removed") {
    // `foliage` — "WHEN the foliage allocation is exceeded THEN GROUND COVER DISTANCE AND DENSITY
    // SHALL BE REDUCED BEFORE TREES ARE REMOVED." The declared order is the mechanism; this is the
    // measurement of it.
    FoliageBudget budget(richest());
    CY_CHECK_EQ(budget.position(), 0u);
    CY_CHECK_EQ(budget.settings().grass.distance_metres, 60.0F);

    budget.set_position(1);
    CY_CHECK_LT(budget.settings().grass.distance_metres, 60.0F);
    CY_CHECK_LT(budget.settings().grass.density_scale, 1.0F);
    // Instance counts have not moved yet, which is the whole of the sentence.
    CY_CHECK_EQ(budget.settings().instance_fraction[static_cast<cy::u32>(SpeciesClass::Canopy)],
                1.0F);

    budget.set_position(4);
    CY_CHECK_LT(budget.settings().instance_fraction[static_cast<cy::u32>(SpeciesClass::Canopy)],
                1.0F);

    // And the reported walk names ground cover first and instance counts last.
    CY_REQUIRE((budget.steps().size()) > (4u));
    CY_CHECK_EQ(budget.steps()[0].axis, BudgetAxis::GroundCoverDistance);
    CY_CHECK_EQ(budget.steps()[1].axis, BudgetAxis::GroundCoverDensity);
    bool instances_last = false;
    for (const ReductionStep& step : budget.steps()) {
        if (step.axis == BudgetAxis::InstanceCounts) {
            instances_last = true;
        } else if (instances_last) {
            // Something reduced AFTER instance counts did, at the same position. That is the
            // failure this ordering exists to prevent.
            CY_CHECK_EQ(step.from, budget.steps()[budget.steps().size() - 1].from);
        }
    }
    CY_CHECK(instances_last);
}

CY_TEST_CASE("gameplay-relevant cover persists while decorative species are cut") {
    // `foliage` — "WHEN a species is marked gameplay-relevant THEN it SHALL NOT be culled by budget
    // pressure while decorative species remain."
    FoliageBudget budget(richest());
    budget.set_position(static_cast<cy::u8>(kMaxBudgetPositions - 1));

    const cy::foliage::SpeciesDeclaration cover = test::pine();        // GameplayImportance::Cover
    const cy::foliage::SpeciesDeclaration scenery = test::fern();      // Decorative
    const cy::foliage::SpeciesDeclaration relevant = test::boulder();  // Relevant

    CY_CHECK_EQ(budget.fraction_for(cover), 1.0F);
    CY_CHECK_LT(budget.fraction_for(scenery), 1.0F);
    CY_CHECK_LT(budget.fraction_for(relevant), 1.0F);
    CY_CHECK(FoliageBudget::protected_from_reduction(cover));
    CY_CHECK_FALSE(FoliageBudget::protected_from_reduction(scenery));
}

CY_TEST_CASE("every lever the requirement names actually moves") {
    // The requirement enumerates five axes the allocation is distributed across, and a budget whose
    // ladder moved only grass would satisfy the scenario above and not the requirement.
    FoliageBudget budget(richest());
    const FoliageSettings top = budget.settings();
    budget.set_position(4);
    const FoliageSettings bottom = budget.settings();

    CY_CHECK_LT(bottom.grass.distance_metres, top.grass.distance_metres);
    CY_CHECK_LT(bottom.grass.density_scale, top.grass.density_scale);
    CY_CHECK_GT(bottom.tier_threshold_scale, top.tier_threshold_scale);
    CY_CHECK_LT(static_cast<cy::u32>(bottom.wind.ceiling), static_cast<cy::u32>(top.wind.ceiling));
    CY_CHECK_LT(bottom.interaction.trail_cells, top.interaction.trail_cells);
    CY_CHECK_LT(bottom.instance_fraction[0], top.instance_fraction[0]);
}

CY_TEST_CASE("the priced ladder descends and position zero costs one") {
    // `rendering::QualityLadder`'s contract: position 0 is 1.0 by construction and every later
    // position costs less, or the arbiter cannot use the prices to choose.
    FoliageBudget budget(richest());
    const FoliageLadder ladder = budget.ladder();
    CY_CHECK_EQ(ladder.positions, kMaxBudgetPositions);
    CY_CHECK_EQ(ladder.cost_at(0), 1.0F);
    for (cy::u8 position = 1; position < kMaxBudgetPositions; ++position) {
        CY_CHECK_LT(ladder.cost_at(position), ladder.cost_at(static_cast<cy::u8>(position - 1)));
        CY_CHECK_GT(ladder.cost_at(position), 0.0F);
    }
    // An out-of-range position is clamped rather than reading past the array.
    CY_CHECK_EQ(ladder.cost_at(200), ladder.cost_at(ladder.last_position()));
}

CY_TEST_CASE("a position beyond the ladder is clamped rather than read past") {
    FoliageBudget budget(richest());
    budget.set_position(200);
    CY_CHECK_EQ(budget.position(), kMaxBudgetPositions - 1);
    CY_CHECK_GT(budget.settings().grass.distance_metres, 0.0F);
    CY_CHECK_GT(budget.settings().interaction.trail_cells, 0u);
}

CY_TEST_CASE("the allowed instance count follows the class's fraction") {
    FoliageBudget budget(richest());
    budget.set_position(4);
    const FoliageSettings& settings = budget.settings();
    const cy::u32 allowed = settings.allowed(SpeciesClass::Canopy, 1000);
    CY_CHECK_LT(allowed, 1000u);
    CY_CHECK_GT(allowed, 0u);
    CY_CHECK_EQ(settings.allowed(SpeciesClass::Canopy, 0), 0u);
}
