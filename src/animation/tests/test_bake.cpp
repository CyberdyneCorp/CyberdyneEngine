// The offline retarget bake: a clip mapped onto a second skeleton at cook time, and the runtime
// path it has to agree with. M8.b task 5.3.
//
// INTEGRATION, and it was moved here from the unit suite after being measured: one bake samples the
// source clip at thirty frames, retargets each, authors two tracks per mapped joint and compresses
// the result. At half the unit budget it spent 0.73 ms of 0.50 ms — a case over the line is one
// thing, but this one is a COOK, and the taxonomy puts a cook in the tier above.

#include <cy/animation/retarget.h>
#include <cy/test/test.h>

#include "fixture.h"
#include "retarget_chains.h"

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;

CY_TEST_CASE("retarget: a baked clip plays what the runtime path would have produced") {
    Skeleton source(allocator());
    Skeleton target(allocator());
    CY_REQUIRE(build_biped(source, "a_").has_value());
    CY_REQUIRE(build_biped(target, "b_", 0.5F).has_value());
    RetargetProfile profile(allocator());
    CY_REQUIRE(map_biped(profile).has_value());
    RetargetReport report;
    CY_REQUIRE(profile.build(source, target, report).has_value());

    Clip walk(allocator());
    CY_REQUIRE(build_walk(walk).has_value());

    Clip baked(allocator());
    CY_REQUIRE(
        bake_clip(allocator(), profile, source, target, walk, 30.0F, CompressionSettings{}, baked)
            .has_value());
    CY_CHECK(baked.compressed());
    CY_CHECK_GT(baked.track_count(), 0U);
    // The root motion track follows the mapping rather than being lost in the bake.
    CY_CHECK(baked.has_root_motion());

    // The baked clip and the runtime path answer the same question the same way.
    Array<Transform> source_local(allocator());
    Array<Transform> runtime(allocator());
    Array<Transform> played(allocator());
    CY_REQUIRE(source_local.resize(kJointCount).has_value());
    CY_REQUIRE(runtime.resize(kJointCount).has_value());
    CY_REQUIRE(played.resize(kJointCount).has_value());

    ClipCursor source_cursor(allocator());
    ClipCursor baked_cursor(allocator());
    SampleStats stats;
    for (u32 step = 1; step < 8; ++step) {
        const f32 time = static_cast<f32>(step) / 8.0F;
        source.reference_pose(source_local.span());
        CY_REQUIRE(walk.sample(time, JointMask::all(kJointCount), source_cursor,
                               source_local.span(), stats)
                       .has_value());
        target.reference_pose(runtime.span());
        CY_REQUIRE(
            profile.retarget_pose(source, target, source_local.span(), runtime.span()).has_value());

        target.reference_pose(played.span());
        CY_REQUIRE(
            baked.sample(time, JointMask::all(kJointCount), baked_cursor, played.span(), stats)
                .has_value());

        CY_CHECK(same_rotation(runtime[kUpperArm].rotation, played[kUpperArm].rotation, 5e-2F));
        CY_CHECK_NEAR(played[kRoot].translation.z, runtime[kRoot].translation.z, 5e-2);
    }
}
