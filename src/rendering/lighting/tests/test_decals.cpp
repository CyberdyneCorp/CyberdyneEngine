// Decals: the projection, the two fades, the normal blend that preserves detail, and the budget's
// deterministic reported eviction. Task 10.3.

#include <cy/test/test.h>

#include <cy/core/math/math.h>
#include <cy/rendering/lighting/decals.h>

#include <cmath>

namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using cy::rendering::blend_decal_normal;
using cy::rendering::decal_angle_fade;
using cy::rendering::decal_distance_fade;
using cy::rendering::decal_retention_score;
using cy::rendering::DecalBudget;
using cy::rendering::DecalEvictionCause;
using cy::rendering::DecalInstance;
using cy::rendering::project_decal;

DecalInstance impact(f32 importance) noexcept {
    DecalInstance decal;
    decal.half_extent = Vec3{0.25F, 0.25F, 0.5F};
    decal.axis_x = Vec3{1.0F, 0.0F, 0.0F};
    decal.axis_y = Vec3{0.0F, 1.0F, 0.0F};
    decal.axis_z = Vec3{0.0F, 0.0F, 1.0F};
    decal.importance = importance;
    return decal;
}

}  // namespace

CY_TEST_CASE("decals: the projection is the box's own frame, and outside is not an error") {
    DecalInstance decal = impact(0.5F);
    decal.center = Vec3{10.0F, 0.0F, 0.0F};

    const auto centre = project_decal(decal, Vec3{10.0F, 0.0F, 0.0F});
    CY_CHECK(centre.inside);
    CY_CHECK_NEAR(centre.uv.x, 0.5F, 1.0e-5F);
    CY_CHECK_NEAR(centre.uv.y, 0.5F, 1.0e-5F);
    CY_CHECK_NEAR(centre.depth, 0.5F, 1.0e-5F);

    const auto corner = project_decal(decal, Vec3{10.25F, 0.25F, 0.5F});
    CY_CHECK(corner.inside);
    CY_CHECK_NEAR(corner.uv.x, 1.0F, 1.0e-5F);
    CY_CHECK_NEAR(corner.uv.y, 1.0F, 1.0e-5F);
    CY_CHECK_NEAR(corner.depth, 1.0F, 1.0e-5F);

    CY_CHECK_FALSE(project_decal(decal, Vec3{11.0F, 0.0F, 0.0F}).inside);
}

CY_TEST_CASE("decals: a steep receiver fades out rather than stretching") {
    // The requirement's own scenario. The stretching it names is what a projection does to a
    // surface nearly parallel to the projection axis: one texel covers an unbounded length. There
    // is no projection that avoids it, so the answer is to stop drawing.
    DecalInstance decal = impact(0.5F);
    decal.fade_angle_radians = 1.0472F;  // 60 degrees

    CY_CHECK_NEAR(decal_angle_fade(decal, Vec3{0.0F, 0.0F, 1.0F}), 1.0F, 1.0e-4F);
    // Just inside the fade angle: some contribution, and less than face on.
    const f32 inside = decal_angle_fade(decal, normalize(Vec3{0.80F, 0.0F, 0.60F}));
    CY_CHECK_GT(inside, 0.0F);
    CY_CHECK_LT(inside, 1.0F);
    // Past it: nothing at all.
    CY_CHECK_EQ(decal_angle_fade(decal, normalize(Vec3{0.95F, 0.0F, 0.31F})), 0.0F);
    // Facing away: nothing, and in particular not a mirrored projection.
    CY_CHECK_EQ(decal_angle_fade(decal, Vec3{0.0F, 0.0F, -1.0F}), 0.0F);
}

CY_TEST_CASE("decals: the distance fade is monotone and reaches both ends exactly") {
    DecalInstance decal = impact(0.5F);
    decal.fade_start = 10.0F;
    decal.fade_end = 20.0F;
    CY_CHECK_EQ(decal_distance_fade(decal, 0.0F), 1.0F);
    CY_CHECK_EQ(decal_distance_fade(decal, 10.0F), 1.0F);
    CY_CHECK_EQ(decal_distance_fade(decal, 20.0F), 0.0F);
    CY_CHECK_EQ(decal_distance_fade(decal, 1000.0F), 0.0F);

    f32 previous = 1.0F;
    for (u32 step = 0; step <= 40; ++step) {
        const f32 distance = static_cast<f32>(step) * 0.75F;
        const f32 value = decal_distance_fade(decal, distance);
        CY_CHECK_LE(value, previous + 1.0e-6F);
        previous = value;
    }
}

CY_TEST_CASE("decals: the normal blend preserves the receiver's detail and stays unit length") {
    const Vec3 receiver = normalize(Vec3{0.3F, 0.2F, 0.93F});
    const Vec3 flat{0.0F, 0.0F, 1.0F};

    // A flat decal normal changes nothing, at any strength. A lerp would drag the receiver towards
    // +Z here, which is exactly the flattening the requirement rules out.
    for (f32 strength : {0.0F, 0.5F, 1.0F}) {
        const Vec3 blended = blend_decal_normal(receiver, flat, strength);
        CY_CHECK_NEAR(length(blended), 1.0F, 1.0e-4F);
        CY_CHECK_NEAR(dot(blended, receiver), 1.0F, 1.0e-3F);
    }

    // A tilted decal normal tilts the result, and the result is still unit length.
    const Vec3 tilted = normalize(Vec3{0.6F, 0.0F, 0.8F});
    const Vec3 blended = blend_decal_normal(receiver, tilted, 1.0F);
    CY_CHECK_NEAR(length(blended), 1.0F, 1.0e-4F);
    CY_CHECK_LT(dot(blended, receiver), 0.999F);
    // Strength zero is the receiver, exactly.
    CY_CHECK_NEAR(dot(blend_decal_normal(receiver, tilted, 0.0F), receiver), 1.0F, 1.0e-4F);
}

CY_TEST_CASE("decals: the budget evicts the least important, deterministically and reportably") {
    const auto run = [](DecalBudget& budget) {
        budget.set_capacity(3);
        budget.set_age_half_life(60.0F);
        // Three decals of descending importance, spawned at the same instant so that importance is
        // the only thing distinguishing them.
        for (f32 importance : {0.9F, 0.5F, 0.2F}) {
            CY_REQUIRE(budget.spawn(impact(importance), 0.0));
        }
        // A fourth, more important than the weakest: the weakest goes.
        const auto placed = budget.spawn(impact(0.7F), 1.0);
        CY_REQUIRE(placed);
        CY_CHECK_NE(placed.value(), 0U);
        return placed.value();
    };

    DecalBudget first;
    DecalBudget second;
    const u64 first_id = run(first);
    const u64 second_id = run(second);

    // Two runs of the same session: the same decal evicted, and the same id assigned.
    CY_REQUIRE_EQ(first.evictions().size(), 1U);
    CY_REQUIRE_EQ(second.evictions().size(), 1U);
    CY_CHECK_EQ(first.evictions()[0].id, second.evictions()[0].id);
    CY_CHECK_EQ(first_id, second_id);

    // And the eviction is REPORTABLE: it says which, why, and what the score was built from.
    const auto& eviction = first.evictions()[0];
    CY_CHECK_EQ(eviction.cause, DecalEvictionCause::BudgetReached);
    CY_CHECK_NEAR(eviction.importance, 0.2F, 1.0e-5F);
    CY_CHECK_GT(eviction.score, 0.0F);
    CY_CHECK_EQ(first.size(), 3U);
}

CY_TEST_CASE("decals: a splatter arriving into a wall of persistent damage is the one that goes") {
    DecalBudget budget;
    budget.set_capacity(2);
    CY_REQUIRE(budget.spawn(impact(0.95F), 0.0));
    CY_REQUIRE(budget.spawn(impact(0.90F), 0.0));

    const auto refused = budget.spawn(impact(0.05F), 1.0);
    CY_REQUIRE(refused);
    // Zero: the new decal was the least worth keeping. Reported like any other eviction, because
    // "nothing appeared" is precisely the symptom somebody will be hunting.
    CY_CHECK_EQ(refused.value(), 0U);
    CY_REQUIRE_EQ(budget.evictions().size(), 1U);
    CY_CHECK_NEAR(budget.evictions()[0].importance, 0.05F, 1.0e-5F);
    CY_CHECK_EQ(budget.size(), 2U);
}

CY_TEST_CASE("decals: the arbiter sets the capacity, and there is no frame time to be found") {
    // The budget is "governed by the renderer budget arbiter". The enforcement is the interface:
    // `set_capacity` is the only way in, and a decal system that wanted to react to frame time
    // would have to be given it by somebody who could see one.
    DecalBudget budget;
    budget.set_capacity(0);
    CY_CHECK_FALSE(budget.spawn(impact(1.0F), 0.0));

    budget.set_capacity(1);
    CY_CHECK(budget.spawn(impact(1.0F), 0.0));
    CY_CHECK_EQ(budget.capacity(), 1U);
}

CY_TEST_CASE("decals: application order is total, so a later decal covers an earlier one") {
    DecalBudget budget;
    budget.set_capacity(8);
    DecalInstance under = impact(0.5F);
    under.sort_order = 1;
    DecalInstance over = impact(0.5F);
    over.sort_order = 5;
    DecalInstance tie = impact(0.5F);
    tie.sort_order = 1;

    CY_REQUIRE(budget.spawn(over, 0.0));
    CY_REQUIRE(budget.spawn(under, 0.0));
    CY_REQUIRE(budget.spawn(tie, 0.0));

    u32 order[8] = {};
    const u32 count = budget.application_order(cy::Span<u32>(order, 8));
    CY_REQUIRE_EQ(count, 3U);
    const auto decals = budget.decals();
    CY_CHECK_EQ(decals[order[0]].sort_order, 1);
    CY_CHECK_EQ(decals[order[1]].sort_order, 1);
    CY_CHECK_EQ(decals[order[2]].sort_order, 5);
    // The tie breaks on id, which is spawn order — so the order is total rather than partial and
    // two runs of one frame apply the decals identically.
    CY_CHECK_LT(decals[order[0]].id, decals[order[1]].id);
}

CY_TEST_CASE("decals: age tells against a decal without growing past f32's useful precision") {
    DecalInstance fresh = impact(0.5F);
    fresh.spawn_time_seconds = 0.0;
    // Two very old decals: an hour apart, and both simply old. A subtraction would separate them by
    // 3,600 and let rounding decide which goes.
    const f32 hour = decal_retention_score(fresh, 3600.0, 60.0F);
    const f32 two_hours = decal_retention_score(fresh, 7200.0, 60.0F);
    CY_CHECK_NEAR(hour, two_hours, 1.0e-6F);
    CY_CHECK_GT(decal_retention_score(fresh, 0.0, 60.0F), hour);
}
