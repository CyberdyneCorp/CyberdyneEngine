// SPDX-License-Identifier: MIT
// A program loaded and played by a binary that links no pose compiler — proven at LINK time.
// Issue #76 stage 2.
//
// `animation-and-skinning`: "Compilation SHALL occur at cook time; the runtime SHALL contain no
// graph compiler." A claim about what a binary does not contain cannot be tested by running it, so
// this translation unit DEFINES `cy::graph::pose::compile_pose` itself. If anything this test links
// — the runtime, the cooked-asset decoders, the program assembly — pulled in the object that holds
// the real compiler (src/graph/src/lower_pose.cpp), the link would fail on a duplicate definition
// and this suite would not exist to run. The case then loads a cooked program and plays it, so the
// absence is of the compiler and not of the runtime path.
//
// THIS FILE MUST NOT USE THE COMPILER, and neither may anything it includes: `fixture.h` is plain
// runtime, `locomotion_fixture.h` (which calls `compile_locomotion`) is deliberately not included.

#include <cy/animation/cooked.h>
#include <cy/animation/evaluate.h>
#include <cy/test/test.h>

#include "fixture.h"

namespace cy::graph::pose {

// The stand-in. Never called: its only job is to collide with the real one if that is linked.
Expected<PoseProgram, Error> compile_pose(const Graph& graph, const NodeRegistry& registry,
                                          u32 joint_count, DiagnosticSink& sink) noexcept {
    (void)graph;
    (void)registry;
    (void)joint_count;
    (void)sink;
    return fail(ErrorCode::Unsupported, "this binary has no pose compiler");
}

}  // namespace cy::graph::pose

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;
namespace pose = cy::graph::pose;

CY_TEST_CASE("runtime only: a cooked program loads and plays in a binary without the compiler") {
    // The program a cook would have written for one state that samples the walk: built as parts,
    // assembled and cooked, then read back as the runtime reads it.
    pose::PoseInstruction sample;
    sample.op = pose::PoseOp::SampleClip;
    sample.clip = 0;
    sample.time_param = 0;
    sample.required = pose::JointMask::all(kJointCount);
    pose::PoseState walking;
    walking.name = Name::intern("walking");
    walking.root = 0;
    pose::ClipRef walk_ref;
    walk_ref.name = Name::intern("walk");
    walk_ref.duration = 1.0F;
    const Name clock = Name::intern("walk_time");
    pose::PoseProgramParts parts;
    parts.name = Name::intern("probe");
    parts.code = Span<const pose::PoseInstruction>(&sample, 1);
    parts.states = Span<const pose::PoseState>(&walking, 1);
    parts.clips = Span<const pose::ClipRef>(&walk_ref, 1);
    parts.parameters = Span<const Name>(&clock, 1);
    parts.joint_count = kJointCount;
    Expected<pose::PoseProgram, Error> assembled = pose::assemble_pose_program(allocator(), parts);
    CY_REQUIRE(assembled.has_value());
    Array<u8> cooked(allocator());
    CY_REQUIRE(encode_program(*assembled, cooked).has_value());
    Expected<pose::PoseProgram, Error> program = decode_program(allocator(), cooked.span());
    CY_REQUIRE(program.has_value());

    Skeleton skeleton(allocator());
    CY_REQUIRE(build_biped(skeleton).has_value());
    Clip walk(allocator());
    CY_REQUIRE(build_walk(walk).has_value());
    const Clip* table[] = {&walk};
    AnimationRig rig(allocator());
    CY_REQUIRE(rig.bind(skeleton, *program, Span<const Clip* const>(table, 1)).has_value());
    AnimationInstance instance(allocator());
    CY_REQUIRE(instance.prepare(rig).has_value());
    PoseScratch scratch(allocator());
    CY_REQUIRE(scratch.prepare(rig).has_value());
    Array<Transform> local(allocator());
    CY_REQUIRE(local.resize(kJointCount).has_value());

    for (u32 tick = 0; tick < 15; ++tick) {
        CY_REQUIRE(advance(rig, instance, 1.0F / 60.0F, nullptr).has_value());
    }
    skeleton.reference_pose(local.span());
    EvaluationStats stats;
    CY_REQUIRE(evaluate(rig, instance, 0, scratch, local.span(), stats).has_value());
    CY_CHECK_EQ(stats.clips_sampled, 1U);
    // A quarter of a second into the walk the root has gone a quarter of a metre.
    CY_CHECK_NEAR(local[kRoot].translation.z, -0.25F, 1e-3);

    // And the stand-in really is what this binary has.
    graph::NodeRegistry registry(allocator());
    graph::Graph graph(allocator(), Name::intern("unused"));
    graph::DiagnosticSink sink(allocator());
    CY_CHECK_FALSE(pose::compile_pose(graph, registry, kJointCount, sink).has_value());
}
