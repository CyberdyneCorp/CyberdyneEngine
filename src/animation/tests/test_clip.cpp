// Clips: error-bounded compression measured against the authored keys, cursored sampling, masked
// sampling, events across a loop, property bindings by identity, and root motion. M8.b task 5.1.

#include <cy/animation/clip.h>
#include <cy/test/test.h>

#include "fixture.h"

#include <cmath>

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;

namespace {

/// A translation track along a straight line: a fitter should keep almost none of its keys.
[[nodiscard]] Status build_ramp(Clip& clip, u32 keys) {
    clip.set_name(Name::intern("ramp"));
    clip.set_duration(2.0F);
    clip.set_loop_mode(LoopMode::None);
    Expected<u32, Error> track =
        clip.add_joint_track(TrackKind::Translation, kHips, Interpolation::Linear);
    if (!track) {
        return Status{make_unexpected(track.error())};
    }
    for (u32 index = 0; index < keys; ++index) {
        const f32 time = (static_cast<f32>(index) / static_cast<f32>(keys - 1)) * 2.0F;
        if (Status added = clip.add_key(*track, time, Vec4{time * 0.5F, 0.0F, 0.0F, 0.0F});
            !added) {
            return added;
        }
    }
    return ok();
}

/// A translation track along a curve no straight line approximates: the fitter keeps most of it,
/// which is what a cursor has to walk.
[[nodiscard]] Status build_wave(Clip& clip, u32 keys) {
    clip.set_name(Name::intern("wave"));
    clip.set_duration(2.0F);
    clip.set_loop_mode(LoopMode::None);
    Expected<u32, Error> track =
        clip.add_joint_track(TrackKind::Translation, kHips, Interpolation::Linear);
    if (!track) {
        return Status{make_unexpected(track.error())};
    }
    for (u32 index = 0; index < keys; ++index) {
        const f32 time = (static_cast<f32>(index) / static_cast<f32>(keys - 1)) * 2.0F;
        const f32 value = std::sin(time * 40.0F) * 0.5F;
        if (Status added = clip.add_key(*track, time, Vec4{value, 0.0F, 0.0F, 0.0F}); !added) {
            return added;
        }
    }
    return ok();
}

}  // namespace

CY_TEST_CASE("clip: compression holds its tolerance and the report is measured, not estimated") {
    Clip clip(allocator());
    CY_REQUIRE(build_ramp(clip, 61).has_value());

    CompressionSettings settings;
    settings.translation_tolerance_mm = 0.1F;
    CY_REQUIRE(clip.compress(settings).has_value());

    const CompressionReport& report = clip.report();
    CY_CHECK_EQ(report.keys_before, 61U);
    // A straight line needs two keys, and the fitter finds that.
    CY_CHECK_LE(report.keys_after, 8U);
    CY_CHECK_GT(report.ratio, 1.0F);
    // The quantiser adds its own error on top of the fitter's: the range is one metre over 65,536
    // steps, which is 0.015 mm, so the achieved error stays inside the tolerance it was given.
    CY_CHECK_LE(report.worst_translation_mm, 0.1F);
}

CY_TEST_CASE("clip: a track that never moves collapses to one key") {
    Clip clip(allocator());
    clip.set_duration(1.0F);
    Expected<u32, Error> track =
        clip.add_joint_track(TrackKind::Scale, kHips, Interpolation::Linear);
    CY_REQUIRE(track.has_value());
    for (u32 index = 0; index < 60; ++index) {
        CY_REQUIRE(
            clip.add_key(*track, static_cast<f32>(index) / 60.0F, Vec4{1.0F, 1.0F, 1.0F, 0.0F})
                .has_value());
    }
    CY_REQUIRE(clip.compress(CompressionSettings{}).has_value());
    CY_CHECK_EQ(clip.report().constant_tracks, 1U);
    CY_CHECK_EQ(clip.report().keys_after, 1U);
    CY_CHECK(clip.tracks()[0].constant);
}

CY_TEST_CASE("clip: forward playback advances the cursor, and only a jump back searches") {
    Clip clip(allocator());
    // A curve no straight line approximates, so the fitter keeps most of its keys: the cursor is
    // what is being measured, not the fitter.
    CY_REQUIRE(build_wave(clip, 300).has_value());
    CompressionSettings settings;
    settings.translation_tolerance_mm = 0.5F;
    CY_REQUIRE(clip.compress(settings).has_value());
    CY_REQUIRE(clip.report().keys_after > 100U);

    ClipCursor cursor(allocator());
    CY_REQUIRE(cursor.reset(clip.track_count()).has_value());
    SampleStats stats;
    Array<Transform> pose(allocator());
    CY_REQUIRE(pose.resize(kJointCount).has_value());

    for (u32 step = 0; step < 200; ++step) {
        const f32 time = (static_cast<f32>(step) / 200.0F) * 2.0F;
        CY_REQUIRE(
            clip.sample(time, JointMask::all(kJointCount), cursor, pose.span(), stats).has_value());
    }
    // Two hundred forward samples over a few hundred keys: a step or two each, and no search.
    CY_CHECK_EQ(cursor.searches(), 0U);
    CY_CHECK_LE(cursor.steps(), 320U);

    cursor.reset_counters();
    CY_REQUIRE(
        clip.sample(0.05F, JointMask::all(kJointCount), cursor, pose.span(), stats).has_value());
    CY_CHECK_EQ(cursor.searches(), 1U);
    CY_CHECK_EQ(cursor.steps(), 0U);
}

CY_TEST_CASE("clip: a rotation is stored hemisphere-aligned and interpolated the short way") {
    Clip clip(allocator());
    clip.set_duration(1.0F);
    clip.set_loop_mode(LoopMode::None);
    Expected<u32, Error> track =
        clip.add_joint_track(TrackKind::Rotation, kUpperArm, Interpolation::Spherical);
    CY_REQUIRE(track.has_value());

    const Quat start = Quat::from_axis_angle(Vec3{0.0F, 1.0F, 0.0F}, -1.5F);
    // Authored on the far side of the hypersphere: componentwise this is a long-way rotation, and
    // hemisphere alignment at import is what stops it being one.
    const Quat end_far = Quat::from_axis_angle(Vec3{0.0F, 1.0F, 0.0F}, 1.5F) * -1.0F;
    CY_REQUIRE(clip.add_key(*track, 0.0F, Vec4{start.x, start.y, start.z, start.w}).has_value());
    CY_REQUIRE(
        clip.add_key(*track, 1.0F, Vec4{end_far.x, end_far.y, end_far.z, end_far.w}).has_value());
    CY_REQUIRE(clip.compress(CompressionSettings{}).has_value());

    ClipCursor cursor(allocator());
    Array<Transform> pose(allocator());
    CY_REQUIRE(pose.resize(kJointCount).has_value());
    SampleStats stats;
    CY_REQUIRE(
        clip.sample(0.5F, JointMask::all(kJointCount), cursor, pose.span(), stats).has_value());

    // Half way between -1.5 and +1.5 radians about Y is the identity, not a rotation the long way
    // round.
    const Quat middle = pose[kUpperArm].rotation;
    CY_CHECK_NEAR(std::fabs(middle.w), 1.0F, 1e-2);
}

CY_TEST_CASE("clip: a masked-out joint's tracks are never read") {
    Clip clip(allocator());
    CY_REQUIRE(build_walk(clip).has_value());

    Array<Transform> pose(allocator());
    CY_REQUIRE(pose.resize(kJointCount).has_value());
    for (Transform& joint : pose) {
        joint = Transform::identity();
    }
    ClipCursor cursor(allocator());
    SampleStats stats;

    // A mask covering the legs only: the clip's arm track is skipped rather than sampled and
    // blended away.
    JointMask legs;
    legs.set(kUpperLeg);
    legs.set(kLowerLeg);
    legs.set(kFoot);
    CY_REQUIRE(clip.sample(0.25F, legs, cursor, pose.span(), stats).has_value());
    CY_CHECK_EQ(stats.tracks_read, 0U);
    CY_CHECK_EQ(stats.tracks_skipped, 2U);
    CY_CHECK(pose[kUpperArm].rotation == Quat::identity());

    // With the arm in the mask the same call reads it.
    SampleStats second;
    JointMask arm = legs;
    arm.set(kUpperArm);
    CY_REQUIRE(clip.sample(0.25F, arm, cursor, pose.span(), second).has_value());
    CY_CHECK_EQ(second.tracks_read, 1U);
    CY_CHECK_FALSE(pose[kUpperArm].rotation == Quat::identity());
}

CY_TEST_CASE("clip: events fire once per crossing, in time order, and again on the next loop") {
    Clip clip(allocator());
    CY_REQUIRE(build_walk(clip, 1.0F).has_value());
    CY_REQUIRE(clip.add_event(Name::intern("swish"), 0.75F).has_value());

    EventBuffer buffer(allocator());
    // One tick across the footstep at 0.5 and the swish at 0.75.
    CY_REQUIRE(emit_events(clip, 7, 0.4F, 0.8F, EventPolicy::Emit, buffer).has_value());
    CY_REQUIRE_EQ(buffer.events().size(), 2U);
    CY_CHECK_EQ(buffer.events()[0].name, Name::intern("footstep"));
    CY_CHECK_EQ(buffer.events()[1].name, Name::intern("swish"));
    CY_CHECK_EQ(buffer.events()[0].instance, 7U);
    CY_CHECK_NEAR(buffer.events()[0].normalised_time, 0.5F, 1e-5);

    // A step long enough to cross the whole clip twice emits every crossing, not just the last.
    buffer.clear();
    CY_REQUIRE(emit_events(clip, 7, 0.0F, 2.5F, EventPolicy::Emit, buffer).has_value());
    CY_CHECK_EQ(buffer.events().size(), 5U);

    // The distant-crowd policy counts them instead of writing them.
    buffer.clear();
    CY_REQUIRE(emit_events(clip, 7, 0.0F, 2.5F, EventPolicy::Suppress, buffer).has_value());
    CY_CHECK_EQ(buffer.events().size(), 0U);
    CY_CHECK_EQ(buffer.suppressed(), 5U);
}

CY_TEST_CASE("clip: a property binding keys on identity, and one that cannot resolve is reported") {
    Clip clip(allocator());
    clip.set_duration(1.0F);
    PropertyBinding glow;
    glow.type = reflect::TypeId{42};
    glow.field = reflect::FieldId{7};
    PropertyBinding removed;
    removed.type = reflect::TypeId{42};
    removed.field = reflect::FieldId{99};

    Expected<u32, Error> live =
        clip.add_property_track(Name::intern("Glow"), glow, Interpolation::Linear);
    CY_REQUIRE(live.has_value());
    CY_REQUIRE(clip.add_key(*live, 0.0F, Vec4{0.0F, 0.0F, 0.0F, 0.0F}).has_value());
    CY_REQUIRE(clip.add_key(*live, 1.0F, Vec4{1.0F, 0.0F, 0.0F, 0.0F}).has_value());
    Expected<u32, Error> dead =
        clip.add_property_track(Name::intern("Gone"), removed, Interpolation::Linear);
    CY_REQUIRE(dead.has_value());
    CY_REQUIRE(clip.add_key(*dead, 0.0F, Vec4{5.0F, 0.0F, 0.0F, 0.0F}).has_value());
    CY_REQUIRE(clip.compress(CompressionSettings{}).has_value());

    static f32 target = 0.0F;
    target = 0.0F;
    const auto resolver = [](const PropertyBinding& binding, void* /*user*/) noexcept -> f32* {
        return binding.field == reflect::FieldId{7} ? &target : nullptr;
    };

    Array<f32*> slots(allocator());
    Array<UnresolvedBinding> unresolved(allocator());
    CY_REQUIRE(resolve_properties(clip, resolver, nullptr, slots, unresolved).has_value());
    CY_REQUIRE_EQ(unresolved.size(), 1U);
    CY_CHECK_EQ(unresolved[0].track, Name::intern("Gone"));

    ClipCursor cursor(allocator());
    // The resolved track still plays; the unresolved one is skipped rather than failing the clip.
    CY_CHECK_EQ(clip.write_properties(0.5F, cursor, slots.span()), 1U);
    CY_CHECK_NEAR(target, 0.5F, 1e-3);
}

CY_TEST_CASE("clip: root motion is a delta over an interval, including one that wraps the loop") {
    Clip clip(allocator());
    CY_REQUIRE(build_walk(clip, 1.0F).has_value());

    // The walk travels one metre along -Z per second.
    const RootDelta half = clip.root_delta(0.0F, 0.5F);
    CY_CHECK_NEAR(half.translation.z, -0.5F, 1e-2);
    CY_CHECK_NEAR(half.distance, 0.5F, 1e-2);

    // An interval that crosses the loop boundary is the tail plus the head, not a jump backwards.
    const RootDelta wrapped = clip.root_delta(0.9F, 1.1F);
    CY_CHECK_NEAR(wrapped.translation.z, -0.2F, 2e-2);
    CY_CHECK_GT(wrapped.distance, 0.0F);
}
