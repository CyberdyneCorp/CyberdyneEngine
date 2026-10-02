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

// REGRESSION: `bake_clip` resampled `floor(duration * rate) + 1` frames, so its last frame was
// taken at exactly the clip's duration — and `Clip::sample` wrapped that time by the source's loop
// mode, which under `LoopMode::Loop` (every imported clip's mode) is ZERO. A baked clip therefore
// ended on the source's FIRST frame, and the last stretch of every baked clip interpolated toward
// it: samples/09b-animated-character measured a 2.4 m joint step at the end of a baked death.
CY_TEST_CASE("retarget: a baked looping clip ends on the source's last frame, not its first") {
    Skeleton source(allocator());
    Skeleton target(allocator());
    CY_REQUIRE(build_biped(source, "a_").has_value());
    CY_REQUIRE(build_biped(target, "b_", 0.5F).has_value());
    RetargetProfile profile(allocator());
    CY_REQUIRE(map_biped(profile).has_value());
    RetargetReport report;
    CY_REQUIRE(profile.build(source, target, report).has_value());

    // The walk travels the root from z = 0 at its first frame to z = -1 at its last, so its last
    // frame and its first are a metre apart and a wrap to the first frame cannot hide.
    Clip walk(allocator());
    CY_REQUIRE(build_walk(walk).has_value());
    CY_REQUIRE(walk.loop_mode() == LoopMode::Loop);

    Clip baked(allocator());
    CY_REQUIRE(
        bake_clip(allocator(), profile, source, target, walk, 30.0F, CompressionSettings{}, baked)
            .has_value());

    Array<Transform> source_local(allocator());
    Array<Transform> runtime(allocator());
    Array<Transform> played(allocator());
    CY_REQUIRE(source_local.resize(kJointCount).has_value());
    CY_REQUIRE(runtime.resize(kJointCount).has_value());
    CY_REQUIRE(played.resize(kJointCount).has_value());
    ClipCursor source_cursor(allocator());
    ClipCursor baked_cursor(allocator());
    SampleStats stats;

    // Inside the last baked interval, where the defect interpolated toward the first frame.
    const f32 time = walk.duration() - (0.25F / 30.0F);
    source.reference_pose(source_local.span());
    CY_REQUIRE(
        walk.sample(time, JointMask::all(kJointCount), source_cursor, source_local.span(), stats)
            .has_value());
    target.reference_pose(runtime.span());
    CY_REQUIRE(
        profile.retarget_pose(source, target, source_local.span(), runtime.span()).has_value());
    target.reference_pose(played.span());
    CY_REQUIRE(baked.sample(time, JointMask::all(kJointCount), baked_cursor, played.span(), stats)
                   .has_value());
    CY_CHECK_NEAR(played[kRoot].translation.z, runtime[kRoot].translation.z, 5e-3);

    // And the stored last key is the source's last frame, scaled by the height ratio.
    ClipCursor end_cursor(allocator());
    target.reference_pose(played.span());
    CY_REQUIRE(baked
                   .sample_unwrapped(baked.duration(), JointMask::all(kJointCount), end_cursor,
                                     played.span(), stats)
                   .has_value());
    CY_CHECK_NEAR(played[kRoot].translation.z, -0.5F, 5e-3);
}
