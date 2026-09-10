// Animation level of detail and pose sharing: tier selection from simulation state, hysteresis, the
// authoritative floor, and the pose cache. M8.b tasks 5.2 and 5.3.

#include <cy/animation/lod.h>
#include <cy/test/test.h>

#include "fixture.h"

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;

CY_TEST_CASE("lod: a tier is chosen from distance, coverage and visibility and from nothing else") {
    LodPolicy policy;
    LodInputs inputs;

    inputs.distance = 5.0F;
    CY_CHECK(select_tier(policy, inputs, LodTier::Full) == LodTier::Full);
    inputs.distance = 25.0F;
    CY_CHECK(select_tier(policy, inputs, LodTier::Full) == LodTier::Simplified);
    inputs.distance = 80.0F;
    CY_CHECK(select_tier(policy, inputs, LodTier::Full) == LodTier::Cached);
    inputs.distance = 500.0F;
    CY_CHECK(select_tier(policy, inputs, LodTier::Full) == LodTier::Baked);

    // Screen coverage overrides distance: a character filling the frame is near whatever the metre
    // count says.
    inputs.distance = 500.0F;
    inputs.coverage = 0.5F;
    CY_CHECK(select_tier(policy, inputs, LodTier::Baked) == LodTier::Full);

    // An invisible instance evaluates no pose; its root motion still runs in advance().
    inputs.coverage = 0.0F;
    inputs.visible = false;
    CY_CHECK(select_tier(policy, inputs, LodTier::Full) == LodTier::Baked);
}

CY_TEST_CASE("lod: the boundary an instance leaves by is further out than the one it entered by") {
    LodPolicy policy;
    LodInputs inputs;
    // Inside the margin past the Full threshold, an instance STAYS where it is: one at Full keeps
    // its full evaluation, and one at Simplified is not promoted. That is what stops an instance
    // hovering on the line from alternating every frame.
    inputs.distance = policy.full_distance * 1.05F;
    CY_CHECK(select_tier(policy, inputs, LodTier::Full) == LodTier::Full);
    CY_CHECK(select_tier(policy, inputs, LodTier::Simplified) == LodTier::Simplified);

    // Past the margin the demotion happens whatever the instance was at.
    inputs.distance = policy.full_distance * 1.5F;
    CY_CHECK(select_tier(policy, inputs, LodTier::Full) == LodTier::Simplified);

    // And well inside it the promotion happens whatever the instance was at.
    inputs.distance = policy.full_distance * 0.5F;
    CY_CHECK(select_tier(policy, inputs, LodTier::Cached) == LodTier::Full);
}

CY_TEST_CASE("lod: an authoritative instance is never taken below Simplified, and a pin wins") {
    LodPolicy policy;
    policy.authoritative = true;
    LodInputs inputs;
    inputs.distance = 500.0F;
    // Its root motion is gameplay state. A cached pose is chosen by a shared phase bucket, and an
    // authoritative instance must not have its pose decided by a neighbour's.
    CY_CHECK(select_tier(policy, inputs, LodTier::Full) == LodTier::Simplified);
    inputs.visible = false;
    CY_CHECK(select_tier(policy, inputs, LodTier::Full) == LodTier::Simplified);

    inputs.pinned = true;
    inputs.pinned_tier = LodTier::Full;
    CY_CHECK(select_tier(policy, inputs, LodTier::Simplified) == LodTier::Full);
}

CY_TEST_CASE("lod: what each tier evaluates is a property of the tier, not of a caller's habit") {
    CY_CHECK(evaluates_pose(LodTier::Cached));
    CY_CHECK_FALSE(evaluates_pose(LodTier::Baked));
    CY_CHECK(evaluates_modifiers(LodTier::Full));
    CY_CHECK_FALSE(evaluates_modifiers(LodTier::Simplified));
    // "Pose sharing SHALL be tier-gated: disabled at Full, available at reduced tiers."
    CY_CHECK_FALSE(may_share_pose(LodTier::Full));
    CY_CHECK(may_share_pose(LodTier::Cached));
    CY_CHECK_LT(bone_lod_for(LodTier::Full), bone_lod_for(LodTier::Cached));

    LodDistribution distribution;
    distribution.note(LodTier::Full);
    distribution.note(LodTier::Cached);
    distribution.note(LodTier::Cached);
    CY_CHECK_EQ(distribution.total(), 3U);
    CY_CHECK_EQ(distribution.counts[static_cast<usize>(LodTier::Cached)], 2U);
}

CY_TEST_CASE("lod: a crowd in one phase bucket evaluates one pose and samples it many times") {
    PoseCache cache(allocator(), kJointCount, 4);
    CY_REQUIRE_EQ(cache.capacity(), 4U);

    // Ten thousand instances would ask; here twenty do, in two buckets.
    u32 filled = 0;
    for (u32 instance = 0; instance < 20; ++instance) {
        PoseCacheKey key;
        key.clip = 0;
        key.bone_lod = 2;
        // Phases within one bucket of eight are the same key.
        key.phase_bucket = PoseCache::bucket_of(instance % 2 == 0 ? 0.02F : 0.51F, 8);
        bool must_fill = false;
        Expected<u32, Error> slot = cache.acquire(key, must_fill);
        CY_REQUIRE(slot.has_value());
        if (must_fill) {
            ++filled;
            cache.pose(*slot)[kHips].translation = Vec3{0.0F, static_cast<f32>(instance), 0.0F};
        }
    }
    CY_CHECK_EQ(filled, 2U);
    CY_CHECK_EQ(cache.stats().lookups, 20U);
    CY_CHECK_EQ(cache.stats().hits, 18U);
    CY_CHECK_EQ(cache.stats().occupancy, 2U);

    // A frame is where a shared pose lives. The next one starts empty.
    cache.begin_frame();
    CY_CHECK_EQ(cache.stats().occupancy, 0U);
}

CY_TEST_CASE("lod: a full pose cache refuses rather than evicting a pose somebody is reading") {
    PoseCache cache(allocator(), kJointCount, 2);
    bool must_fill = false;
    for (u16 bucket = 0; bucket < 2; ++bucket) {
        PoseCacheKey key;
        key.phase_bucket = bucket;
        CY_REQUIRE(cache.acquire(key, must_fill).has_value());
    }
    PoseCacheKey third;
    third.phase_bucket = 9;
    const Expected<u32, Error> refused = cache.acquire(third, must_fill);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == ErrorCode::Unavailable);
    CY_CHECK_EQ(cache.stats().evictions, 1U);
}

CY_TEST_CASE("lod: shared-pose instances keep their own aim without touching the shared pose") {
    PoseCache cache(allocator(), kJointCount, 2);
    bool must_fill = false;
    Expected<u32, Error> slot = cache.acquire(PoseCacheKey{}, must_fill);
    CY_REQUIRE(slot.has_value());
    CY_REQUIRE(must_fill);
    for (Transform& joint : cache.pose(*slot)) {
        joint = Transform::identity();
    }

    PoseVariation variation;
    variation.aim = Quat::from_axis_angle(Vec3{0.0F, 1.0F, 0.0F}, 0.5F);
    variation.mask.set(kChest);
    variation.mask.set(kUpperArm);

    Array<Transform> mine(allocator());
    CY_REQUIRE(mine.resize(kJointCount).has_value());
    apply_variation(cache.pose(*slot), variation, mine.span());

    CY_CHECK_FALSE(mine[kChest].rotation == Quat::identity());
    CY_CHECK(mine[kFoot].rotation == Quat::identity());
    // The shared pose is unchanged: the next instance reads what was evaluated, not what the last
    // one aimed.
    CY_CHECK(cache.pose(*slot)[kChest].rotation == Quat::identity());
}
