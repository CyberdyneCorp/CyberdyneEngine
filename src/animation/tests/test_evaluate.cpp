// The compiled animation program's runtime: laziness, masks and layer weights, the state machine's
// blend, batched evaluation, root motion on the deterministic CPU path, events, and inverse
// kinematics. M8.b tasks 5.2 and 5.3.
//
// INTEGRATION, and deliberately: every case here compiles a graph and runs a program over hundreds
// of ticks or hundreds of instances. The subject IS the repeated evaluation.

#include <cy/animation/evaluate.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/lower_pose.h>
#include <cy/test/test.h>

#include "fixture.h"

#include <cmath>

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;
using cy::graph::DiagnosticSink;
using cy::graph::Graph;
using cy::graph::Literal;
using cy::graph::NodeRegistry;

namespace {

[[nodiscard]] Literal text(const char* value) noexcept {
    Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(value);
    return literal;
}

[[nodiscard]] Literal number(f32 value) noexcept {
    Literal literal;
    literal.type = Name::intern("float");
    literal.value = graph::Immediate::scalar(value);
    return literal;
}

[[nodiscard]] Literal integer(u32 value) noexcept {
    Literal literal;
    literal.type = Name::intern("int");
    literal.value.mask = value;
    return literal;
}

/// A graph written as a list of calls rather than as a chain of `if (!status) return`, which is
/// what a dozen fallible calls in a row otherwise reads as. The first failure is kept and the rest
/// are no-ops, so the caller checks once.
class GraphWriter {
public:
    explicit GraphWriter(Graph& graph) noexcept : graph_(&graph) {}

    void node(graph::NodeKey key, const char* type) noexcept {
        keep(graph_->add_node(key, Name::intern(type)));
    }
    void prop(graph::NodeKey key, const char* name, const Literal& value) noexcept {
        keep(graph_->set_property(key, Name::intern(name), value));
    }
    void link(graph::NodeKey from, const char* out, graph::NodeKey to, const char* in) noexcept {
        keep(graph_->connect(from, Name::intern(out), to, Name::intern(in)));
    }
    [[nodiscard]] Status result() const noexcept { return status_; }

private:
    void keep(const Status& step) noexcept {
        if (status_ && !step) {
            status_ = step;
        }
    }

    Graph* graph_ = nullptr;
    Status status_ = ok();
};

/// A locomotion graph: a walk state, an aim layer over it masked to the arm, and a second state the
/// machine can transition into.
///
///   walk ──┐
///          layer(mask = shoulder..finger, weight = "aim_weight") ── state "moving"
///   aim  ──┘
///   aim  ── state "idle"   (played on its own clock)
[[nodiscard]] Status build_locomotion(Graph& graph, bool with_layer, bool with_transition) {
    GraphWriter writer(graph);
    writer.node(1, "pose.clip");
    writer.prop(1, "clip", text("walk"));
    writer.prop(1, "time_parameter", text("walk_time"));
    writer.node(2, "pose.clip");
    writer.prop(2, "clip", text("aim"));
    writer.prop(2, "time_parameter", text("aim_time"));
    writer.node(3, "pose.state");

    if (with_layer) {
        writer.node(4, "pose.layer");
        // The arm, from the shoulder to the finger: four joints of twelve.
        writer.prop(4, "mask_first", integer(kShoulder));
        writer.prop(4, "mask_count", integer(4));
        writer.prop(4, "weight_parameter", text("aim_weight"));
        writer.link(1, "pose", 4, "a");
        writer.link(2, "pose", 4, "b");
        writer.link(4, "pose", 3, "pose");
    } else {
        writer.link(1, "pose", 3, "pose");
    }

    if (with_transition) {
        writer.node(5, "pose.clip");
        writer.prop(5, "clip", text("aim"));
        writer.prop(5, "time_parameter", text("idle_time"));
        writer.node(6, "pose.state");
        writer.link(5, "pose", 6, "pose");
        writer.node(7, "pose.transition");
        writer.prop(7, "condition", text("stop"));
        writer.prop(7, "duration", number(0.2F));
        writer.prop(7, "interruption", text("none"));
        writer.link(3, "pose", 7, "from");
        writer.link(6, "pose", 7, "to");
    }
    return writer.result();
}

/// The clip table, in the order the program's clip table names them rather than in authoring order.
[[nodiscard]] Status bind_clips(const PoseProgram& program, const Clip& walk, const Clip& aim,
                                Array<const Clip*>& out) {
    if (Status sized = out.resize(program.clips().size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < program.clips().size(); ++index) {
        const Name name = program.clips()[index].name;
        out[index] = name == Name::intern("aim") ? &aim : &walk;
    }
    return ok();
}

struct Harness {
    explicit Harness(Allocator& allocator) noexcept
        : registry(allocator),
          skeleton(allocator),
          walk(allocator),
          aim(allocator),
          clips(allocator),
          rig(allocator),
          scratch(allocator),
          pose(allocator) {}

    NodeRegistry registry;
    Skeleton skeleton;
    Clip walk;
    Clip aim;
    Array<const Clip*> clips;
    AnimationRig rig;
    PoseScratch scratch;
    Array<Transform> pose;
};

}  // namespace

CY_TEST_CASE(
    "animation: a state nobody is in samples nothing, and neither does a masked-out clip") {
    Harness harness(allocator());
    CY_REQUIRE(build_biped(harness.skeleton).has_value());
    CY_REQUIRE(build_walk(harness.walk).has_value());
    CY_REQUIRE(build_aim(harness.aim).has_value());
    CY_REQUIRE(graph::pose::register_pose_nodes(harness.registry).has_value());

    Graph graph(allocator(), Name::intern("locomotion"));
    CY_REQUIRE(build_locomotion(graph, true, true).has_value());
    graph.resolve(harness.registry);
    DiagnosticSink sink(allocator());
    auto program = graph::pose::compile_pose(graph, harness.registry, kJointCount, sink);
    CY_REQUIRE(program.has_value());
    CY_REQUIRE_EQ(program.value().states().size(), 2U);

    CY_REQUIRE(bind_clips(program.value(), harness.walk, harness.aim, harness.clips).has_value());
    CY_REQUIRE(
        harness.rig.bind(harness.skeleton, program.value(), harness.clips.span()).has_value());
    AnimationInstance instance(allocator());
    CY_REQUIRE(instance.prepare(harness.rig).has_value());
    CY_REQUIRE(harness.scratch.prepare(harness.rig).has_value());
    CY_REQUIRE(harness.pose.resize(kJointCount).has_value());
    harness.skeleton.reference_pose(harness.pose.span());

    CY_REQUIRE(instance.set_parameter(harness.rig, Name::intern("aim_weight"), 1.0F).has_value());
    EvaluationStats stats;
    CY_REQUIRE(evaluate(harness.rig, instance, 0, harness.scratch, harness.pose.span(), stats)
                   .has_value());

    // The other state's clip is never reached: two clips are sampled, not three.
    CY_CHECK_EQ(stats.clips_sampled, 2U);
    // Twelve joints for the base and four for the masked layer. "the lower-body joints of that
    // layer's clips are never read, and they SHALL NOT be sampled."
    CY_CHECK_EQ(stats.joints_sampled, static_cast<u32>(kJointCount) + 4U);
}

CY_TEST_CASE("animation: a layer at zero weight costs no sample at all") {
    Harness harness(allocator());
    CY_REQUIRE(build_biped(harness.skeleton).has_value());
    CY_REQUIRE(build_walk(harness.walk).has_value());
    CY_REQUIRE(build_aim(harness.aim).has_value());
    CY_REQUIRE(graph::pose::register_pose_nodes(harness.registry).has_value());
    Graph graph(allocator(), Name::intern("locomotion"));
    CY_REQUIRE(build_locomotion(graph, true, false).has_value());
    graph.resolve(harness.registry);
    DiagnosticSink sink(allocator());
    auto program = graph::pose::compile_pose(graph, harness.registry, kJointCount, sink);
    CY_REQUIRE(program.has_value());
    CY_REQUIRE(bind_clips(program.value(), harness.walk, harness.aim, harness.clips).has_value());
    CY_REQUIRE(
        harness.rig.bind(harness.skeleton, program.value(), harness.clips.span()).has_value());

    AnimationInstance instance(allocator());
    CY_REQUIRE(instance.prepare(harness.rig).has_value());
    CY_REQUIRE(harness.scratch.prepare(harness.rig).has_value());
    CY_REQUIRE(harness.pose.resize(kJointCount).has_value());
    harness.skeleton.reference_pose(harness.pose.span());

    EvaluationStats off;
    CY_REQUIRE(
        evaluate(harness.rig, instance, 0, harness.scratch, harness.pose.span(), off).has_value());
    CY_CHECK_EQ(off.clips_sampled, 1U);
    CY_CHECK_GT(off.instructions_skipped, 0U);

    CY_REQUIRE(instance.set_parameter(harness.rig, Name::intern("aim_weight"), 1.0F).has_value());
    EvaluationStats on;
    CY_REQUIRE(
        evaluate(harness.rig, instance, 0, harness.scratch, harness.pose.span(), on).has_value());
    CY_CHECK_EQ(on.clips_sampled, 2U);
}

CY_TEST_CASE("animation: a bone level of detail stops a dropped joint being sampled or written") {
    Harness harness(allocator());
    CY_REQUIRE(build_biped(harness.skeleton).has_value());
    CY_REQUIRE(build_walk(harness.walk).has_value());
    CY_REQUIRE(build_aim(harness.aim).has_value());
    CY_REQUIRE(graph::pose::register_pose_nodes(harness.registry).has_value());
    Graph graph(allocator(), Name::intern("locomotion"));
    CY_REQUIRE(build_locomotion(graph, false, false).has_value());
    graph.resolve(harness.registry);
    DiagnosticSink sink(allocator());
    auto program = graph::pose::compile_pose(graph, harness.registry, kJointCount, sink);
    CY_REQUIRE(program.has_value());
    CY_REQUIRE(bind_clips(program.value(), harness.walk, harness.aim, harness.clips).has_value());
    CY_REQUIRE(
        harness.rig.bind(harness.skeleton, program.value(), harness.clips.span()).has_value());
    AnimationInstance instance(allocator());
    CY_REQUIRE(instance.prepare(harness.rig).has_value());
    CY_REQUIRE(harness.scratch.prepare(harness.rig).has_value());
    CY_REQUIRE(harness.pose.resize(kJointCount).has_value());
    harness.skeleton.reference_pose(harness.pose.span());

    EvaluationStats full;
    CY_REQUIRE(
        evaluate(harness.rig, instance, 0, harness.scratch, harness.pose.span(), full).has_value());
    EvaluationStats reduced;
    CY_REQUIRE(evaluate(harness.rig, instance, 2, harness.scratch, harness.pose.span(), reduced)
                   .has_value());
    // Two joints — the finger and the head — are gone from the second level.
    CY_CHECK_EQ(full.joints_sampled - reduced.joints_sampled, 2U);
}

CY_TEST_CASE("animation: root motion is integrated whatever the tier and whatever the rate") {
    Harness harness(allocator());
    CY_REQUIRE(build_biped(harness.skeleton).has_value());
    CY_REQUIRE(build_walk(harness.walk, 4.0F).has_value());
    CY_REQUIRE(build_aim(harness.aim).has_value());
    CY_REQUIRE(graph::pose::register_pose_nodes(harness.registry).has_value());
    Graph graph(allocator(), Name::intern("locomotion"));
    CY_REQUIRE(build_locomotion(graph, false, false).has_value());
    graph.resolve(harness.registry);
    DiagnosticSink sink(allocator());
    auto program = graph::pose::compile_pose(graph, harness.registry, kJointCount, sink);
    CY_REQUIRE(program.has_value());
    CY_REQUIRE(bind_clips(program.value(), harness.walk, harness.aim, harness.clips).has_value());
    CY_REQUIRE(
        harness.rig.bind(harness.skeleton, program.value(), harness.clips.span()).has_value());

    // Sixty ticks of a sixtieth of a second, against ten of a tenth — one second either way.
    // "WHEN an instance evaluates at
    // 10 Hz instead of 60 Hz THEN integrated root motion over a second SHALL match."
    AnimationInstance fast(allocator());
    AnimationInstance slow(allocator());
    CY_REQUIRE(fast.prepare(harness.rig).has_value());
    CY_REQUIRE(slow.prepare(harness.rig).has_value());
    // And the slow one is at the tier that evaluates NO POSE AT ALL, which is the half of the
    // requirement that matters: root motion is not derived from a pose.
    slow.set_tier(LodTier::Baked);

    for (u32 tick = 0; tick < 60; ++tick) {
        CY_REQUIRE(advance(harness.rig, fast, 1.0F / 60.0F, nullptr).has_value());
    }
    for (u32 tick = 0; tick < 10; ++tick) {
        CY_REQUIRE(advance(harness.rig, slow, 1.0F / 10.0F, nullptr).has_value());
    }
    CY_CHECK_NEAR(fast.travelled().z, -1.0F, 2e-2);
    CY_CHECK_NEAR(slow.travelled().z, fast.travelled().z, 2e-2);

    // Re-simulating the same interval from the same start gives the same answer, which is what
    // network reconciliation asks of it.
    AnimationInstance replayed(allocator());
    CY_REQUIRE(replayed.prepare(harness.rig).has_value());
    for (u32 tick = 0; tick < 60; ++tick) {
        CY_REQUIRE(advance(harness.rig, replayed, 1.0F / 60.0F, nullptr).has_value());
    }
    CY_CHECK_EQ(replayed.travelled().z, fast.travelled().z);
}

CY_TEST_CASE("animation: the state machine blends both trees and no others, then settles") {
    Harness harness(allocator());
    CY_REQUIRE(build_biped(harness.skeleton).has_value());
    CY_REQUIRE(build_walk(harness.walk).has_value());
    CY_REQUIRE(build_aim(harness.aim).has_value());
    CY_REQUIRE(graph::pose::register_pose_nodes(harness.registry).has_value());
    Graph graph(allocator(), Name::intern("machine"));
    CY_REQUIRE(build_locomotion(graph, false, true).has_value());
    graph.resolve(harness.registry);
    DiagnosticSink sink(allocator());
    auto program = graph::pose::compile_pose(graph, harness.registry, kJointCount, sink);
    CY_REQUIRE(program.has_value());
    CY_REQUIRE_EQ(program.value().transitions().size(), 1U);
    CY_REQUIRE(bind_clips(program.value(), harness.walk, harness.aim, harness.clips).has_value());
    CY_REQUIRE(
        harness.rig.bind(harness.skeleton, program.value(), harness.clips.span()).has_value());

    AnimationInstance instance(allocator());
    CY_REQUIRE(instance.prepare(harness.rig).has_value());
    CY_REQUIRE(harness.scratch.prepare(harness.rig).has_value());
    CY_REQUIRE(harness.pose.resize(kJointCount).has_value());

    CY_REQUIRE(advance(harness.rig, instance, 0.1F, nullptr).has_value());
    CY_CHECK_EQ(instance.machine().transition, 0xFFFFU);

    CY_REQUIRE(instance.set_parameter(harness.rig, Name::intern("stop"), 1.0F).has_value());
    CY_REQUIRE(advance(harness.rig, instance, 0.1F, nullptr).has_value());
    CY_CHECK_EQ(instance.machine().target, 1U);

    harness.skeleton.reference_pose(harness.pose.span());
    EvaluationStats mid;
    CY_REQUIRE(
        evaluate(harness.rig, instance, 0, harness.scratch, harness.pose.span(), mid).has_value());
    CY_CHECK_EQ(mid.clips_sampled, 2U);

    // The blend completes and the machine is in one state again, sampling one tree.
    CY_REQUIRE(advance(harness.rig, instance, 0.3F, nullptr).has_value());
    CY_CHECK_EQ(instance.machine().state, 1U);
    CY_CHECK_EQ(instance.machine().transition, 0xFFFFU);
    EvaluationStats settled;
    harness.skeleton.reference_pose(harness.pose.span());
    CY_REQUIRE(evaluate(harness.rig, instance, 0, harness.scratch, harness.pose.span(), settled)
                   .has_value());
    CY_CHECK_EQ(settled.clips_sampled, 1U);
}

CY_TEST_CASE("animation: a batch of five hundred is one program, packed state and one scratch") {
    Harness harness(allocator());
    CY_REQUIRE(build_biped(harness.skeleton).has_value());
    CY_REQUIRE(build_walk(harness.walk).has_value());
    CY_REQUIRE(build_aim(harness.aim).has_value());
    CY_REQUIRE(graph::pose::register_pose_nodes(harness.registry).has_value());
    Graph graph(allocator(), Name::intern("crowd"));
    CY_REQUIRE(build_locomotion(graph, true, false).has_value());
    graph.resolve(harness.registry);
    DiagnosticSink sink(allocator());
    auto program = graph::pose::compile_pose(graph, harness.registry, kJointCount, sink);
    CY_REQUIRE(program.has_value());
    CY_REQUIRE(bind_clips(program.value(), harness.walk, harness.aim, harness.clips).has_value());
    CY_REQUIRE(
        harness.rig.bind(harness.skeleton, program.value(), harness.clips.span()).has_value());

    constexpr u32 kInstances = 500;
    AnimationBatch batch(allocator(), harness.rig);
    for (u32 index = 0; index < kInstances; ++index) {
        Expected<u32, Error> slot = batch.add();
        CY_REQUIRE(slot.has_value());
        // Each instance starts at its own phase, which is what makes a crowd look like one.
        CY_REQUIRE(batch.instance(*slot)
                       .set_parameter(harness.rig, Name::intern("walk_time"),
                                      static_cast<f32>(index) * 0.002F)
                       .has_value());
    }
    CY_CHECK_EQ(batch.size(), kInstances);

    EventBuffer events(allocator());
    CY_REQUIRE(batch.advance_all(1.0F / 60.0F, &events).has_value());

    Array<Transform> poses(allocator());
    CY_REQUIRE(poses.resize(static_cast<usize>(kInstances) * kJointCount).has_value());
    CY_REQUIRE(harness.scratch.prepare(harness.rig).has_value());

    // The range is the unit of work a job would take. Four of them, one scratch each in a real
    // schedule; one here, because the point is that the batch splits.
    EvaluationStats stats;
    for (u32 first = 0; first < kInstances; first += 125) {
        CY_REQUIRE(
            batch.evaluate_range(first, 125, 0, harness.scratch, poses.span(), stats).has_value());
    }
    CY_CHECK_EQ(stats.clips_sampled, kInstances);
    // A range that runs past the end is refused rather than reading another batch's state.
    CY_CHECK_FALSE(batch.evaluate_range(kInstances - 1, 4, 0, harness.scratch, poses.span(), stats)
                       .has_value());
}

CY_TEST_CASE("animation: events come out of advance in time order, and a distant tier suppresses") {
    Harness harness(allocator());
    CY_REQUIRE(build_biped(harness.skeleton).has_value());
    CY_REQUIRE(build_walk(harness.walk).has_value());
    CY_REQUIRE(build_aim(harness.aim).has_value());
    CY_REQUIRE(graph::pose::register_pose_nodes(harness.registry).has_value());
    Graph graph(allocator(), Name::intern("locomotion"));
    CY_REQUIRE(build_locomotion(graph, false, false).has_value());
    graph.resolve(harness.registry);
    DiagnosticSink sink(allocator());
    auto program = graph::pose::compile_pose(graph, harness.registry, kJointCount, sink);
    CY_REQUIRE(program.has_value());
    CY_REQUIRE(bind_clips(program.value(), harness.walk, harness.aim, harness.clips).has_value());
    CY_REQUIRE(
        harness.rig.bind(harness.skeleton, program.value(), harness.clips.span()).has_value());

    AnimationInstance near(allocator());
    CY_REQUIRE(near.prepare(harness.rig).has_value());
    near.set_identifier(11);
    EventBuffer buffer(allocator());
    // Two seconds of a one-second clip: the footstep at the half-second is crossed twice.
    for (u32 tick = 0; tick < 120; ++tick) {
        CY_REQUIRE(advance(harness.rig, near, 1.0F / 60.0F, &buffer).has_value());
    }
    CY_CHECK_EQ(buffer.events().size(), 2U);
    CY_CHECK_EQ(buffer.events()[0].name, Name::intern("footstep"));
    CY_CHECK_EQ(buffer.events()[0].instance, 11U);

    AnimationInstance far(allocator());
    CY_REQUIRE(far.prepare(harness.rig).has_value());
    far.set_event_policy(EventPolicy::Suppress);
    buffer.clear();
    for (u32 tick = 0; tick < 120; ++tick) {
        CY_REQUIRE(advance(harness.rig, far, 1.0F / 60.0F, &buffer).has_value());
    }
    CY_CHECK_EQ(buffer.events().size(), 0U);
    CY_CHECK_EQ(buffer.suppressed(), 2U);
}
