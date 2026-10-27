// SPDX-License-Identifier: MIT
// Blend spaces at run time: barycentric pose weights, sync groups keeping the feet in phase across
// clips of different lengths, root motion and events blended by the pose weights, laziness and
// determinism. Issue #92.
//
// INTEGRATION, as test_evaluate.cpp is: every case compiles a graph and advances it over hundreds
// of ticks.

#include <cy/animation/evaluate.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/lower_pose.h>
#include <cy/test/test.h>

#include "fixture.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;
using cy::graph::DiagnosticSink;
using cy::graph::Graph;
using cy::graph::Literal;
using cy::graph::NodeRegistry;

namespace {

constexpr f32 kTick = 1.0F / 60.0F;

[[nodiscard]] Literal text(const char* value) noexcept {
    Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(value);
    return literal;
}

[[nodiscard]] Literal at(f32 x, f32 y = 0.0F) noexcept {
    Literal literal;
    literal.type = Name::intern("vec2");
    literal.value.x = x;
    literal.value.y = y;
    return literal;
}

[[nodiscard]] Literal flag(bool value) noexcept {
    Literal literal;
    literal.type = Name::intern("bool");
    literal.value.mask = value ? 1U : 0U;
    return literal;
}

struct Marker {
    const char* name = "";
    f32 time = 0.0F;
};

/// A locomotion cycle: the root travels forward at `speed` m/s, the hips sit at `hips` (so a blend
/// is readable as a translation), and the clip carries `markers` and one event named `step`.
[[nodiscard]] Status build_cycle(Clip& clip, const char* name, f32 duration, f32 speed, Vec3 hips,
                                 std::initializer_list<Marker> markers,
                                 const char* step = nullptr) noexcept {
    clip.set_name(Name::intern(name));
    clip.set_duration(duration);
    clip.set_loop_mode(LoopMode::Loop);
    clip.set_sample_rate_hint(30.0F);
    clip.set_root_motion_joint(kRoot);
    // A track's keys are authored before the next track is added.
    const u32 frames = static_cast<u32>(std::lround(duration * 30.0F)) + 1U;
    const auto key_time = [duration, frames](u32 frame) noexcept {
        return frame + 1 == frames ? duration : static_cast<f32>(frame) / 30.0F;
    };
    Expected<u32, Error> root =
        clip.add_joint_track(TrackKind::Translation, kRoot, Interpolation::Linear);
    if (!root) {
        return Status{make_unexpected(root.error())};
    }
    for (u32 frame = 0; frame < frames; ++frame) {
        const f32 time = key_time(frame);
        if (Status added = clip.add_key(*root, time, Vec4{0.0F, 0.0F, -speed * time, 0.0F});
            !added) {
            return added;
        }
    }
    Expected<u32, Error> pelvis =
        clip.add_joint_track(TrackKind::Translation, kHips, Interpolation::Linear);
    if (!pelvis) {
        return Status{make_unexpected(pelvis.error())};
    }
    for (u32 frame = 0; frame < frames; ++frame) {
        if (Status added =
                clip.add_key(*pelvis, key_time(frame), Vec4{hips.x, hips.y, hips.z, 0.0F});
            !added) {
            return added;
        }
    }
    for (const Marker& marker : markers) {
        if (Status added = clip.add_marker(Name::intern(marker.name), marker.time); !added) {
            return added;
        }
    }
    if (step != nullptr) {
        if (Status added = clip.add_event(Name::intern(step), duration * 0.5F); !added) {
            return added;
        }
    }
    CompressionSettings settings;
    return clip.compress(settings);
}

struct SpaceSample {
    Clip* clip = nullptr;
    f32 x = 0.0F;
    f32 y = 0.0F;
};

/// A rig whose one state is a blend space over the given clips: `speed` (and `direction` in two
/// dimensions) place the character among them.
class SpaceRig {
public:
    SpaceRig() noexcept
        : registry_(allocator()),
          skeleton_(allocator()),
          graph_(allocator(), Name::intern("space")),
          sink_(allocator()),
          program_(allocator()),
          clips_(allocator()),
          rig_(allocator()) {}

    [[nodiscard]] Status build(bool planar, bool sync, std::initializer_list<SpaceSample> samples) {
        if (Status built = build_biped(skeleton_); !built) {
            return built;
        }
        if (Status registered = graph::pose::register_pose_nodes(registry_); !registered) {
            return registered;
        }
        Status status =
            graph_.add_node(100, Name::intern(planar ? "pose.blend2d" : "pose.blend1d"));
        status =
            status ? graph_.set_property(100, Name::intern("x_parameter"), text("speed")) : status;
        status = status && planar
                     ? graph_.set_property(100, Name::intern("y_parameter"), text("direction"))
                     : status;
        status = status ? graph_.set_property(100, Name::intern("sync"), flag(sync)) : status;
        u32 index = 0;
        for (const SpaceSample& sample : samples) {
            const graph::NodeKey node = index + 1;
            char pin[8] = {};
            (void)std::snprintf(pin, sizeof(pin), "s%u", index);
            char clock[24] = {};
            (void)std::snprintf(clock, sizeof(clock), "clock_%u", index);
            const std::string_view clip_name = sample.clip->name().text();
            char named[64] = {};
            (void)std::snprintf(named, sizeof(named), "%.*s", static_cast<int>(clip_name.size()),
                                clip_name.data());
            status = status ? graph_.add_node(node, Name::intern("pose.clip")) : status;
            status = status ? graph_.set_property(node, Name::intern("clip"), text(named)) : status;
            status = status ? graph_.set_property(node, Name::intern("time_parameter"), text(clock))
                            : status;
            status = status ? graph_.connect(node, Name::intern("pose"), 100, Name::intern(pin))
                            : status;
            status = status ? graph_.set_property(100, Name::intern(pin), at(sample.x, sample.y))
                            : status;
            samples_[index++] = sample.clip;
        }
        count_ = index;
        status = status ? graph_.add_node(200, Name::intern("pose.state")) : status;
        status =
            status ? graph_.connect(100, Name::intern("pose"), 200, Name::intern("pose")) : status;
        if (!status) {
            return status;
        }
        graph_.resolve(registry_);
        auto program = graph::pose::compile_pose(graph_, registry_, kJointCount, sink_);
        if (!program) {
            return Status{make_unexpected(program.error())};
        }
        if (sink_.errors() != 0) {
            return fail(ErrorCode::InvalidArgument, "the space did not compile");
        }
        program_ = std::move(*program);
        if (Status sized = clips_.resize(program_.clips().size()); !sized) {
            return sized;
        }
        for (usize clip = 0; clip < program_.clips().size(); ++clip) {
            for (u32 sample = 0; sample < count_; ++sample) {
                if (samples_[sample]->name() == program_.clips()[clip].name) {
                    clips_[clip] = samples_[sample];
                }
            }
        }
        return rig_.bind(skeleton_, program_, clips_.span());
    }

    [[nodiscard]] AnimationRig& rig() noexcept { return rig_; }
    [[nodiscard]] const PoseProgram& program() const noexcept { return program_; }
    [[nodiscard]] const Skeleton& skeleton() const noexcept { return skeleton_; }

    /// The clock of the sample that plays `clip`.
    [[nodiscard]] f32 clock(const AnimationInstance& instance, const Clip& clip) const noexcept {
        for (u32 sample = 0; sample < count_; ++sample) {
            if (samples_[sample] == &clip) {
                char name[24] = {};
                (void)std::snprintf(name, sizeof(name), "clock_%u", sample);
                return instance.parameter(rig_, Name::intern(name));
            }
        }
        return -1.0F;
    }

private:
    NodeRegistry registry_;
    Skeleton skeleton_;
    Graph graph_;
    DiagnosticSink sink_;
    PoseProgram program_;
    Array<const Clip*> clips_;
    AnimationRig rig_;
    Clip* samples_[graph::pose::kMaxBlendSamples] = {};
    u32 count_ = 0;
};

/// A clip's position in its stride, in marker space, computed here from the markers alone: the
/// segment from one marker to the next, and how far through it. `first` is the marker the test
/// counts segments from, so two clips that begin on different feet compare.
[[nodiscard]] f32 stride_phase(const Clip& clip, f32 time, Name first) noexcept {
    f32 starts[8] = {};
    u32 count = 0;
    f32 sorted[8] = {};
    Name names[8];
    for (const ClipMarker& marker : clip.markers()) {
        sorted[count] = marker.time;
        names[count++] = marker.name;
    }
    for (u32 outer = 1; outer < count; ++outer) {
        for (u32 inner = outer; inner > 0 && sorted[inner - 1] > sorted[inner]; --inner) {
            std::swap(sorted[inner - 1], sorted[inner]);
            std::swap(names[inner - 1], names[inner]);
        }
    }
    u32 rotation = 0;
    for (u32 index = 0; index < count; ++index) {
        rotation = names[index] == first ? index : rotation;
    }
    const f32 duration = clip.duration();
    for (u32 index = 0; index < count; ++index) {
        const u32 source = rotation + index;
        starts[index] = sorted[source % count] + (source >= count ? duration : 0.0F);
    }
    f32 at = time - (std::floor(time / duration) * duration);
    if (at < starts[0]) {
        at += duration;
    }
    u32 segment = 0;
    for (u32 index = 0; index < count; ++index) {
        segment = starts[index] <= at ? index : segment;
    }
    const f32 end = segment + 1 < count ? starts[segment + 1] : starts[0] + duration;
    return static_cast<f32>(segment) + ((at - starts[segment]) / (end - starts[segment]));
}

/// The shortest distance between two phases on a cycle of `segments`.
[[nodiscard]] f32 phase_gap(f32 a, f32 b, f32 segments) noexcept {
    f32 gap = std::fabs(a - b);
    gap = gap - (std::floor(gap / segments) * segments);
    return math::min(gap, segments - gap);
}

}  // namespace

CY_TEST_CASE(
    "animation: a two-dimensional blend space blends the three poses around the character") {
    Clip idle(allocator());
    Clip forward(allocator());
    Clip right(allocator());
    Clip left(allocator());
    Clip back(allocator());
    CY_REQUIRE(build_cycle(idle, "idle", 1.0F, 0.0F, Vec3{0.0F, 1.0F, 0.0F}, {}).has_value());
    CY_REQUIRE(build_cycle(forward, "forward", 1.0F, 2.0F, Vec3{0.0F, 1.0F, 0.4F}, {}).has_value());
    CY_REQUIRE(build_cycle(right, "right", 1.0F, 0.0F, Vec3{0.4F, 1.0F, 0.0F}, {}).has_value());
    CY_REQUIRE(build_cycle(left, "left", 1.0F, 0.0F, Vec3{-0.4F, 1.0F, 0.0F}, {}).has_value());
    CY_REQUIRE(build_cycle(back, "back", 1.0F, 0.0F, Vec3{0.0F, 1.0F, -0.4F}, {}).has_value());
    SpaceRig space;
    CY_REQUIRE(space
                   .build(true, false,
                          {{&idle, 0.0F, 0.0F},
                           {&forward, 0.0F, 1.0F},
                           {&right, 1.0F, 0.0F},
                           {&left, -1.0F, 0.0F},
                           {&back, 0.0F, -1.0F}})
                   .has_value());

    AnimationInstance instance(allocator());
    CY_REQUIRE(instance.prepare(space.rig()).has_value());
    // Lateral 0.25 on x, forward 0.5 on y: idle 0.25, forward 0.5, right 0.25.
    CY_REQUIRE(instance.set_parameter(space.rig(), Name::intern("speed"), 0.25F).has_value());
    CY_REQUIRE(instance.set_parameter(space.rig(), Name::intern("direction"), 0.5F).has_value());
    PoseScratch scratch(allocator());
    CY_REQUIRE(scratch.prepare(space.rig()).has_value());
    Array<Transform> pose(allocator());
    CY_REQUIRE(pose.resize(kJointCount).has_value());
    space.skeleton().reference_pose(pose.span());
    EvaluationStats stats;
    CY_REQUIRE(evaluate(space.rig(), instance, 0, scratch, pose.span(), stats).has_value());

    const Vec3 expected = (Vec3{0.0F, 1.0F, 0.0F} * 0.25F) + (Vec3{0.0F, 1.0F, 0.4F} * 0.5F) +
                          (Vec3{0.4F, 1.0F, 0.0F} * 0.25F);
    CY_CHECK_LT(length(pose[kHips].translation - expected), 1e-4F);
    // THE TWO SAMPLES AWAY FROM THE CHARACTER ARE NEVER SAMPLED.
    CY_CHECK_EQ(stats.clips_sampled, 3U);
    CY_CHECK_EQ(stats.blend_samples_skipped, 2U);

    // Root motion blends by the same weights: only the forward clip travels, at 2 m/s.
    CY_REQUIRE(advance(space.rig(), instance, kTick, nullptr).has_value());
    CY_CHECK_LT(std::fabs(instance.root_motion().translation.z - (-2.0F * 0.5F * kTick)), 1e-4F);
}

CY_TEST_CASE("animation: a sync group keeps a walk and a run of different lengths in step") {
    // A one-second walk putting the left foot down at 0.2 s and the right at 0.7 s, and a 0.6 s run
    // that starts on the RIGHT foot: markers correspond one segment along.
    Clip walk(allocator());
    Clip run(allocator());
    CY_REQUIRE(build_cycle(walk, "walk", 1.0F, 1.5F, Vec3{0.0F, 1.0F, 0.0F},
                           {{"foot_l", 0.2F}, {"foot_r", 0.7F}})
                   .has_value());
    CY_REQUIRE(build_cycle(run, "run", 0.6F, 4.0F, Vec3{0.0F, 0.9F, 0.0F},
                           {{"foot_r", 0.05F}, {"foot_l", 0.35F}})
                   .has_value());
    const Name left = Name::intern("foot_l");

    for (const bool sync : {true, false}) {
        SpaceRig space;
        CY_REQUIRE(space.build(false, sync, {{&walk, 1.5F}, {&run, 4.0F}}).has_value());
        if (sync) {
            CY_REQUIRE_EQ(space.rig().sync_spaces().size(), 1U);
            CY_CHECK_EQ(space.rig().sync_spaces()[0].segments, 2U);
        }
        AnimationInstance instance(allocator());
        CY_REQUIRE(instance.prepare(space.rig()).has_value());
        f32 worst = 0.0F;
        f32 speed = 1.5F;
        for (u32 tick = 0; tick < 600; ++tick) {
            // The speed sweeps from a walk to a run and back, so the weights change under the
            // clocks the whole time.
            speed = 1.5F + (2.5F * (0.5F - (0.5F * std::cos(static_cast<f32>(tick) * 0.02F))));
            CY_REQUIRE(
                instance.set_parameter(space.rig(), Name::intern("speed"), speed).has_value());
            CY_REQUIRE(advance(space.rig(), instance, kTick, nullptr).has_value());
            const f32 walking = stride_phase(walk, space.clock(instance, walk), left);
            const f32 running = stride_phase(run, space.clock(instance, run), left);
            worst = math::max(worst, phase_gap(walking, running, 2.0F));
        }
        if (sync) {
            // THE FEET STAY IN PHASE: both clips are at the same point of the stride on every tick,
            // whatever the weights did.
            CY_CHECK_LT(worst, 1e-3F);
        } else {
            // Without the sync group the two clocks run at their own rates and drift apart.
            CY_CHECK_GT(worst, 0.25F);
        }
    }
}

CY_TEST_CASE("animation: a synchronised space strides at the blend of its clips' cadences") {
    Clip walk(allocator());
    Clip run(allocator());
    CY_REQUIRE(build_cycle(walk, "walk", 1.0F, 1.5F, Vec3{0.0F, 1.0F, 0.0F},
                           {{"foot_l", 0.2F}, {"foot_r", 0.7F}})
                   .has_value());
    CY_REQUIRE(build_cycle(run, "run", 0.6F, 4.0F, Vec3{0.0F, 0.9F, 0.0F},
                           {{"foot_r", 0.05F}, {"foot_l", 0.35F}})
                   .has_value());
    SpaceRig space;
    CY_REQUIRE(space.build(false, true, {{&walk, 1.5F}, {&run, 4.0F}}).has_value());

    // At the walk's own position the walk plays at its own speed.
    AnimationInstance walking(allocator());
    CY_REQUIRE(walking.prepare(space.rig()).has_value());
    CY_REQUIRE(walking.set_parameter(space.rig(), Name::intern("speed"), 1.5F).has_value());
    const f32 start = space.clock(walking, walk);
    for (u32 tick = 0; tick < 30; ++tick) {
        CY_REQUIRE(advance(space.rig(), walking, kTick, nullptr).has_value());
    }
    CY_CHECK_LT(std::fabs(space.clock(walking, walk) - (start + (30.0F * kTick))), 1e-4F);

    // Half and half, the stride lasts the mean of 1.0 s and 0.6 s per segment: 0.8 s a cycle. The
    // walk clock runs 1.25x and the run 0.75x, so the root travels at the mean of 1.875 m/s and
    // 3 m/s.
    AnimationInstance blended(allocator());
    CY_REQUIRE(blended.prepare(space.rig()).has_value());
    CY_REQUIRE(blended.set_parameter(space.rig(), Name::intern("speed"), 2.75F).has_value());
    const f32 walk_start = space.clock(blended, walk);
    const u32 ticks = 48;  // 0.8 s
    for (u32 tick = 0; tick < ticks; ++tick) {
        CY_REQUIRE(advance(space.rig(), blended, kTick, nullptr).has_value());
    }
    const f32 drift = std::fabs(space.clock(blended, walk) - walk_start);
    CY_CHECK_LT(math::min(drift, walk.duration() - drift), 2e-3F);
    const f32 travelled = -blended.travelled().z;
    const f32 expected = ((1.5F * 1.25F) + (4.0F * 0.75F)) * 0.5F * 0.8F;
    CY_CHECK_LT(std::fabs(travelled - expected), expected * 0.01F);
}

CY_TEST_CASE("animation: a blend space's root motion and events follow its pose weights") {
    Clip walk(allocator());
    Clip run(allocator());
    CY_REQUIRE(
        build_cycle(walk, "walk", 1.0F, 1.0F, Vec3{0.0F, 1.0F, 0.0F}, {}, "walk_step").has_value());
    CY_REQUIRE(
        build_cycle(run, "run", 0.5F, 3.0F, Vec3{0.0F, 1.0F, 0.0F}, {}, "run_step").has_value());
    SpaceRig space;
    CY_REQUIRE(space.build(false, false, {{&walk, 1.0F}, {&run, 3.0F}}).has_value());
    AnimationInstance instance(allocator());
    CY_REQUIRE(instance.prepare(space.rig()).has_value());
    // A quarter walk, three quarters run.
    CY_REQUIRE(instance.set_parameter(space.rig(), Name::intern("speed"), 2.5F).has_value());
    EventBuffer events(allocator());
    for (u32 tick = 0; tick < 120; ++tick) {
        CY_REQUIRE(advance(space.rig(), instance, kTick, &events).has_value());
        if (tick > 0 && tick % 30 != 0) {
            const f32 z = instance.root_motion().translation.z;
            CY_CHECK_LT(std::fabs(z - (-((0.25F * 1.0F) + (0.75F * 3.0F)) * kTick)), 1e-4F);
        }
    }
    // The run is the heavier sample, so it is the one whose steps are heard; the walk's are not
    // emitted as a second set of footsteps.
    u32 run_steps = 0;
    u32 walk_steps = 0;
    for (const EmittedEvent& event : events.events()) {
        run_steps += event.name == Name::intern("run_step") ? 1U : 0U;
        walk_steps += event.name == Name::intern("walk_step") ? 1U : 0U;
    }
    CY_CHECK_EQ(run_steps, 4U);
    CY_CHECK_EQ(walk_steps, 0U);
}

CY_TEST_CASE("animation: two instances given the same parameters reach the same pose bit for bit") {
    Clip walk(allocator());
    Clip run(allocator());
    Clip sprint(allocator());
    CY_REQUIRE(build_cycle(walk, "walk", 1.0F, 1.5F, Vec3{0.0F, 1.0F, 0.0F},
                           {{"foot_l", 0.2F}, {"foot_r", 0.7F}})
                   .has_value());
    CY_REQUIRE(build_cycle(run, "run", 0.6F, 4.0F, Vec3{0.0F, 0.9F, 0.1F},
                           {{"foot_r", 0.05F}, {"foot_l", 0.35F}})
                   .has_value());
    CY_REQUIRE(build_cycle(sprint, "sprint", 0.45F, 7.0F, Vec3{0.0F, 0.8F, 0.2F},
                           {{"foot_l", 0.1F}, {"foot_r", 0.3F}})
                   .has_value());
    SpaceRig space;
    CY_REQUIRE(
        space.build(false, true, {{&walk, 1.5F}, {&run, 4.0F}, {&sprint, 7.0F}}).has_value());
    AnimationInstance first(allocator());
    AnimationInstance second(allocator());
    CY_REQUIRE(first.prepare(space.rig()).has_value());
    CY_REQUIRE(second.prepare(space.rig()).has_value());
    PoseScratch scratch(allocator());
    CY_REQUIRE(scratch.prepare(space.rig()).has_value());
    Array<Transform> one(allocator());
    Array<Transform> two(allocator());
    CY_REQUIRE(one.resize(kJointCount).has_value());
    CY_REQUIRE(two.resize(kJointCount).has_value());
    for (u32 tick = 0; tick < 300; ++tick) {
        const f32 speed = 1.0F + (6.5F * std::fabs(std::sin(static_cast<f32>(tick) * 0.013F)));
        for (AnimationInstance* instance : {&first, &second}) {
            CY_REQUIRE(
                instance->set_parameter(space.rig(), Name::intern("speed"), speed).has_value());
            CY_REQUIRE(advance(space.rig(), *instance, kTick, nullptr).has_value());
        }
        space.skeleton().reference_pose(one.span());
        space.skeleton().reference_pose(two.span());
        EvaluationStats stats;
        CY_REQUIRE(evaluate(space.rig(), first, 0, scratch, one.span(), stats).has_value());
        CY_REQUIRE(evaluate(space.rig(), second, 0, scratch, two.span(), stats).has_value());
        CY_REQUIRE(same_bits(one.data(), two.data(), sizeof(Transform) * kJointCount));
        // At most two of a one-dimensional space's samples are walked.
        CY_CHECK_LE(stats.clips_sampled, 4U);
    }
    CY_CHECK(same_bits(first.parameters().data(), second.parameters().data(),
                       sizeof(f32) * first.parameters().size()));
}

CY_TEST_CASE("animation: a mask with half weight on the spine blends that joint proportionally") {
    // Two clips that differ only at the spine: the layer moves it 20 cm forward.
    Clip base(allocator());
    Clip lean(allocator());
    for (Clip* clip : {&base, &lean}) {
        clip->set_name(Name::intern(clip == &base ? "base" : "lean"));
        clip->set_duration(1.0F);
        clip->set_loop_mode(LoopMode::Loop);
        Expected<u32, Error> spine =
            clip->add_joint_track(TrackKind::Translation, kSpine, Interpolation::Linear);
        CY_REQUIRE(spine.has_value());
        const f32 forward = clip == &base ? 0.0F : 0.2F;
        CY_REQUIRE(clip->add_key(*spine, 0.0F, Vec4{0.0F, 0.4F, forward, 0.0F}).has_value());
        CY_REQUIRE(clip->add_key(*spine, 1.0F, Vec4{0.0F, 0.4F, forward, 0.0F}).has_value());
        CY_REQUIRE(clip->compress(CompressionSettings{}).has_value());
    }
    NodeRegistry registry(allocator());
    CY_REQUIRE(graph::pose::register_pose_nodes(registry).has_value());
    Graph graph(allocator(), Name::intern("layered"));
    Literal first;
    first.type = Name::intern("int");
    first.value.mask = kSpine;
    Literal count = first;
    count.value.mask = 2;  // spine and chest
    CY_REQUIRE(graph.add_node(1, Name::intern("pose.clip")).has_value());
    CY_REQUIRE(graph.set_property(1, Name::intern("clip"), text("base")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("pose.clip")).has_value());
    CY_REQUIRE(graph.set_property(2, Name::intern("clip"), text("lean")).has_value());
    CY_REQUIRE(graph.add_node(3, Name::intern("pose.layer")).has_value());
    CY_REQUIRE(graph.set_property(3, Name::intern("mask_first"), first).has_value());
    CY_REQUIRE(graph.set_property(3, Name::intern("mask_count"), count).has_value());
    CY_REQUIRE(graph.set_property(3, Name::intern("weight_parameter"), text("lean")).has_value());
    CY_REQUIRE(graph.add_node(4, Name::intern("pose.state")).has_value());
    CY_REQUIRE(graph.connect(1, Name::intern("pose"), 3, Name::intern("a")).has_value());
    CY_REQUIRE(graph.connect(2, Name::intern("pose"), 3, Name::intern("b")).has_value());
    CY_REQUIRE(graph.connect(3, Name::intern("pose"), 4, Name::intern("pose")).has_value());
    graph.resolve(registry);
    DiagnosticSink sink(allocator());
    auto program = graph::pose::compile_pose(graph, registry, kJointCount, sink);
    CY_REQUIRE(program.has_value());
    Skeleton skeleton(allocator());
    CY_REQUIRE(build_biped(skeleton).has_value());
    Array<const Clip*> clips(allocator());
    for (const graph::pose::ClipRef& reference : program->clips()) {
        CY_REQUIRE(
            clips.push_back(reference.name == Name::intern("base") ? &base : &lean).has_value());
    }
    AnimationRig rig(allocator());
    CY_REQUIRE(rig.bind(skeleton, *program, clips.span()).has_value());
    const u16 mask = program->code()[program->states()[0].root].mask;
    CY_REQUIRE(rig.set_mask_weight(mask, kSpine, 0.5F).has_value());

    AnimationInstance instance(allocator());
    CY_REQUIRE(instance.prepare(rig).has_value());
    CY_REQUIRE(instance.set_parameter(rig, Name::intern("lean"), 1.0F).has_value());
    PoseScratch scratch(allocator());
    CY_REQUIRE(scratch.prepare(rig).has_value());
    Array<Transform> pose(allocator());
    CY_REQUIRE(pose.resize(kJointCount).has_value());
    skeleton.reference_pose(pose.span());
    EvaluationStats stats;
    CY_REQUIRE(evaluate(rig, instance, 0, scratch, pose.span(), stats).has_value());
    // Half of the layer's 20 cm at the spine; the chest, at full weight, would take all of it.
    CY_CHECK_LT(std::fabs(pose[kSpine].translation.z - 0.1F), 1e-3F);
}
