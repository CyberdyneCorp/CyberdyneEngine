// Budgets, coordinated reduction, eviction and churn. M6 task 4.2.
//
// `residency` — "Budgets and pressure response" and "Eviction and churn control". Four scenarios,
// four cases: "Coordinated reduction", "Gameplay-critical content is protected", "No oscillation",
// and "Churn is visible" — plus "A shadow page is not a geometry page", which belongs to the
// eviction score.

#include <cy/servers/residency/policy.h>
#include <cy/test/test.h>

using namespace cy::residency;
using cy::f32;
using cy::u32;

namespace {

[[nodiscard]] ResidentPage page(Subsystem subsystem, cy::u64 id, f32 importance,
                                CostClass cost) noexcept {
    ResidentPage record;
    record.key = PageKey{subsystem, id};
    record.bytes = 65536;
    record.importance = importance;
    record.screen_contribution = importance;
    record.cost = cost;
    record.resident_frames = 100;
    record.last_used_at = 10.0;
    return record;
}

[[nodiscard]] LeverSchedule schedule(f32 normal, f32 elevated, f32 critical) noexcept {
    LeverSchedule lever;
    lever.declared = true;
    lever.normal = normal;
    lever.elevated = elevated;
    lever.critical = critical;
    return lever;
}

}  // namespace

CY_TEST_CASE("a shadow page is not a geometry page: regeneration cost is weighed") {
    const ResidentPage shadow = page(Subsystem::Shadow, 1, 0.5F, CostClass::Rendered);
    const ResidentPage geometry = page(Subsystem::Geometry, 1, 0.5F, CostClass::Streamed);
    // Identical in every other respect. The expensive one is worth more to keep.
    CY_CHECK_GT(eviction_score(shadow, 10.0), eviction_score(geometry, 10.0));
}

CY_TEST_CASE("cost is weighed logarithmically, so one expensive page cannot become immovable") {
    ResidentPage expensive = page(Subsystem::Shadow, 1, 0.0F, CostClass::Rendered);
    expensive.production_cost_ms = 1000.0F;
    ResidentPage visible = page(Subsystem::Geometry, 1, 1.0F, CostClass::Streamed);
    visible.production_cost_ms = 1.0F;
    // A thousandfold cost difference must not outweigh a page that is actually on screen.
    CY_CHECK_GT(eviction_score(visible, 10.0), eviction_score(expensive, 10.0));
}

CY_TEST_CASE("a hold and a guarantee are facts, not weights") {
    ResidentPage held = page(Subsystem::Texture, 1, 0.0F, CostClass::Streamed);
    held.last_used_at = 0.0;  // ancient
    held.holds = 1;
    CY_CHECK_NEAR(eviction_score(held, 1000.0), kNeverEvict, 1.0F);

    ResidentPage tail = page(Subsystem::Texture, 2, 0.0F, CostClass::Streamed);
    tail.last_used_at = 0.0;
    tail.guaranteed = true;
    CY_CHECK_NEAR(eviction_score(tail, 1000.0), kNeverEvict, 1.0F);

    ResidentPage ordinary = page(Subsystem::Texture, 3, 1.0F, CostClass::Rendered);
    CY_CHECK_LT(eviction_score(ordinary, 10.0), kNeverEvict);
}

CY_TEST_CASE("recency decays rather than cliffs, and an active page is preferred") {
    const ResidentPage fresh = page(Subsystem::Texture, 1, 0.5F, CostClass::Streamed);
    ResidentPage stale = fresh;
    stale.key = PageKey{Subsystem::Texture, 2};
    stale.last_used_at = 8.0;  // two seconds ago, at now = 10
    CY_CHECK_GT(eviction_score(fresh, 10.0), eviction_score(stale, 10.0));

    ResidentPage active = fresh;
    active.key = PageKey{Subsystem::Texture, 3};
    active.active = true;
    CY_CHECK_GT(eviction_score(active, 10.0), eviction_score(fresh, 10.0));
}

CY_TEST_CASE("no oscillation: a page that arrived this frame cannot leave this frame") {
    SubsystemPolicy policy;
    policy.min_residency_frames = 3;

    ResidentPage arriving = page(Subsystem::Texture, 1, 0.0F, CostClass::Streamed);
    arriving.resident_frames = 0;
    CY_CHECK_FALSE(past_minimum_age(arriving, policy));
    arriving.resident_frames = 2;
    CY_CHECK_FALSE(past_minimum_age(arriving, policy));
    arriving.resident_frames = 3;
    CY_CHECK(past_minimum_age(arriving, policy));
}

CY_TEST_CASE("churn is visible: an eviction re-requested inside the window is counted") {
    ChurnTracker churn;
    const PageKey key{Subsystem::Texture, 77};

    CY_REQUIRE(churn.note_eviction(key, 100.0));
    CY_CHECK(churn.note_request(key, 100.4, 1.0));  // inside the window: churn
    CY_CHECK_EQ(churn.stats(Subsystem::Texture).evictions, 1U);
    CY_CHECK_EQ(churn.stats(Subsystem::Texture).refetches, 1U);
    CY_CHECK_NEAR(churn.stats(Subsystem::Texture).rate(), 1.0, 1e-9);

    // And the record is consumed, so the next eviction of the same page starts again rather than
    // counting a refetch that was already counted.
    CY_CHECK_FALSE(churn.note_request(key, 100.5, 1.0));

    CY_REQUIRE(churn.note_eviction(key, 200.0));
    CY_CHECK_FALSE(churn.note_request(key, 260.0, 1.0));  // long after: an ordinary miss
    CY_CHECK_EQ(churn.stats(Subsystem::Texture).refetches, 1U);
}

CY_TEST_CASE("churn records are pruned, so the table does not grow with the session") {
    ChurnTracker churn;
    for (cy::u64 id = 0; id < 64; ++id) {
        CY_REQUIRE(churn.note_eviction(PageKey{Subsystem::Geometry, id}, 10.0));
    }
    CY_CHECK_EQ(churn.tracked(), 64U);
    churn.prune(10.5, 1.0);
    CY_CHECK_EQ(churn.tracked(), 64U);  // still inside the window
    churn.prune(20.0, 1.0);
    CY_CHECK_EQ(churn.tracked(), 0U);
}

CY_TEST_CASE("coordinated reduction walks the declared order rather than broadcasting") {
    SubsystemPolicy policies[kSubsystemCount];
    bool registered[kSubsystemCount] = {};
    f32 levers[kSubsystemCount * kLeverCount] = {};

    // Illumination reduces first, then textures, then shadows. The order is declared, not derived.
    policies[static_cast<u32>(Subsystem::Illumination)].reduction_order = 0;
    policies[static_cast<u32>(Subsystem::Illumination)]
        .levers[static_cast<u32>(Lever::IlluminationCacheDensity)] = schedule(1.0F, 0.5F, 0.25F);
    policies[static_cast<u32>(Subsystem::Texture)].reduction_order = 1;
    policies[static_cast<u32>(Subsystem::Texture)].levers[static_cast<u32>(Lever::TextureMipBias)] =
        schedule(0.0F, 1.0F, 2.0F);
    policies[static_cast<u32>(Subsystem::Shadow)].reduction_order = 2;
    policies[static_cast<u32>(Subsystem::Shadow)]
        .levers[static_cast<u32>(Lever::ShadowResolutionScale)] = schedule(1.0F, 0.75F, 0.5F);
    registered[static_cast<u32>(Subsystem::Illumination)] = true;
    registered[static_cast<u32>(Subsystem::Texture)] = true;
    registered[static_cast<u32>(Subsystem::Shadow)] = true;

    // Start the levers where Normal puts them, so the plan is the change and not the whole table.
    levers[(static_cast<u32>(Subsystem::Illumination) * kLeverCount) +
           static_cast<u32>(Lever::IlluminationCacheDensity)] = 1.0F;
    levers[(static_cast<u32>(Subsystem::Shadow) * kLeverCount) +
           static_cast<u32>(Lever::ShadowResolutionScale)] = 1.0F;

    cy::Array<ReductionStep> plan;
    CY_REQUIRE(plan_reduction(policies, registered, levers, cy::PressureLevel::Elevated, plan));

    CY_REQUIRE_EQ(plan.size(), 3U);
    CY_CHECK(plan[0].subsystem == Subsystem::Illumination);
    CY_CHECK(plan[1].subsystem == Subsystem::Texture);
    CY_CHECK(plan[2].subsystem == Subsystem::Shadow);
    CY_CHECK_NEAR(plan[0].to, 0.5F, 1e-6F);
    CY_CHECK_NEAR(plan[1].to, 1.0F, 1e-6F);
    CY_CHECK_NEAR(plan[2].to, 0.75F, 1e-6F);

    // An undeclared lever is not adjusted — which is what lets audio and world cells sit in the
    // same enumeration as the four caches without pretending to have a mip bias.
    for (const ReductionStep& step : plan) {
        CY_CHECK(step.lever != Lever::TexturePrefetchRadius);
    }
}

CY_TEST_CASE("an unregistered subsystem contributes nothing to a plan") {
    SubsystemPolicy policies[kSubsystemCount];
    bool registered[kSubsystemCount] = {};
    f32 levers[kSubsystemCount * kLeverCount] = {};
    policies[static_cast<u32>(Subsystem::Texture)].levers[static_cast<u32>(Lever::TextureMipBias)] =
        schedule(0.0F, 1.0F, 2.0F);

    cy::Array<ReductionStep> plan;
    CY_REQUIRE(plan_reduction(policies, registered, levers, cy::PressureLevel::Critical, plan));
    CY_CHECK(plan.empty());

    registered[static_cast<u32>(Subsystem::Texture)] = true;
    CY_REQUIRE(plan_reduction(policies, registered, levers, cy::PressureLevel::Critical, plan));
    CY_CHECK_EQ(plan.size(), 1U);
    CY_CHECK_NEAR(plan[0].to, 2.0F, 1e-6F);
}
