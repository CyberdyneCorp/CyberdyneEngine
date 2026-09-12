// Four separately exported rigs driving one character: the milestone case, end to end. M8.d.
//
// THE SHAPE OF THE PROBLEM. A Mixamo library is one file per animation, and every one of them
// carries its own copy of the skeleton — only the file with the mesh carries a skin. A clip is
// authored against the joint INDICES of the rig beside it, so three of the four rigs here are
// rigs nothing will ever skin, and their clips have to end up on the fourth. This suite is the
// integration one because a bake is a cook: three clips are resampled and recompressed against a
// twenty-eight joint skeleton, which is the work `src/animation/tests/CMakeLists.txt` moved the
// retarget bake here for.
//
// WHAT IS ASSERTED, AND WHY EACH ONE. A retarget that silently produces the target's bind pose
// fails nothing: the character stands there, every transform is finite, and no status is an error.
// So "differs from the bind pose" is asserted on every crossing, beside the finiteness and the
// bounds, and so is the one thing a wrong CORRESPONDENCE breaks rather than a wrong pose — that the
// left leg's animation arrives on the left leg.

#include <cy/animation/evaluate.h>
#include <cy/animation/retarget_build.h>
#include <cy/core/memory/array.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/lower_pose.h>
#include <cy/test/test.h>

#include "fixture.h"
#include "rig_exports.h"

#include <cmath>
#include <string>

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;
using namespace cy::animation::testing::rigs;

namespace {

/// The export with the mesh and the skin: the character.
ExportShape character_shape() noexcept {
    return ExportShape{};
}

/// One animation file: the rig it shipped with, and the animation authored against that rig.
struct SourceExport {
    const char* label = "";
    ExportShape rig;
    ClipShape animation;
};

/// The three animation-only exports, standing in for `Breathing Idle`, `Running` and
/// `Dying Backwards`: the same rig with the namespace rewritten, the same body plan written in a
/// different joint order, and another studio's character — its own joint vocabulary, a third again
/// as tall, and a root joint above the hips this character's rig does not have.
SourceExport re_export() noexcept {
    SourceExport source;
    source.label = "re-export of the character's own rig";
    source.rig.prefix = "mixamorig:";
    source.animation.duration = 1.5F;
    source.animation.travel = 0.9F;
    return source;
}

SourceExport reordered_export() noexcept {
    SourceExport source;
    source.label = "the same body plan in a different joint order";
    source.rig.order = kLegsFirstOrder;
    source.animation.duration = 0.8F;
    source.animation.travel = 1.4F;
    source.animation.stride_radians = 0.9F;
    return source;
}

SourceExport tall_export() noexcept {
    SourceExport source;
    source.label = "another studio's taller character, with a root joint above its hips";
    source.rig.names = kStudioNames;
    source.rig.prefix = "";
    source.rig.scale = 1.3F;
    source.rig.armature_root = true;
    source.animation.duration = 1.2F;
    // Proportional to the character it was authored for: 1.3 m of stride for a rig a third again as
    // tall is 1.0 m of stride for this one.
    source.animation.travel = 1.3F;
    source.animation.bob = 0.039F;
    return source;
}

/// What one sampled pose of the character says. Everything the milestone asks to be checked is a
/// fold over these.
struct PoseSurvey {
    bool finite = true;
    bool normalised = true;
    /// Metres: the furthest any joint sits from the hips, in model space. A skeleton is about a
    /// metre across, so this is the bound that catches a pose that exploded.
    f32 widest_reach = 0.0F;
    f32 lowest_hips = 1e9F;
    f32 highest_hips = -1e9F;
    f32 first_hips_z = 0.0F;
    f32 last_hips_z = 0.0F;
    /// How much of the clip the samples actually spanned. A looping clip wraps its duration back to
    /// zero, so the last sample sits just short of the end and the travel measured over it is that
    /// fraction of the clip's own.
    f32 sampled_fraction = 1.0F;
    /// Degrees: the furthest any joint's local rotation departs from the rest pose over the clip.
    /// Zero here is the silent failure — a retarget that produced the bind pose every frame.
    f32 worst_departure = 0.0F;
};

void survey_pose(const ExportedRig& character, Span<const Transform> local, bool first,
                 PoseSurvey& out) noexcept {
    const Transform hips = model_of(character.skeleton, local, character.joint(RigJoint::Hips));
    out.lowest_hips = math::min(out.lowest_hips, hips.translation.y);
    out.highest_hips = math::max(out.highest_hips, hips.translation.y);
    out.last_hips_z = hips.translation.z;
    if (first) {
        out.first_hips_z = hips.translation.z;
    }
    for (u16 joint = 0; joint < character.skeleton.joint_count(); ++joint) {
        const Transform& placement = local[joint];
        out.finite = out.finite && math::is_finite(placement.translation.x) &&
                     math::is_finite(placement.translation.y) &&
                     math::is_finite(placement.translation.z);
        out.normalised =
            out.normalised && math::nearly_equal(length(placement.rotation), 1.0F, 1e-3F);
        out.worst_departure = math::max(
            out.worst_departure,
            math::degrees(angle_between(placement.rotation,
                                        character.skeleton.joints()[joint].bind_local.rotation)));
        const Transform model = model_of(character.skeleton, local, joint);
        out.widest_reach =
            math::max(out.widest_reach, length(model.translation - hips.translation));
    }
}

/// Sample a clip on the character at `samples` evenly spaced times over [0, duration) and fold
/// every pose into one survey. The pose is seeded with the reference pose each time, which is what
/// `Clip::sample` documents its caller does.
///
/// The half-open interval is not an accident: `Clip::wrap` takes `duration` back to zero under
/// `LoopMode::Loop`, so a sample AT the duration reads the first frame and a travel measured to it
/// is zero.
[[nodiscard]] Status survey_clip(const ExportedRig& character, const Clip& clip, u32 samples,
                                 PoseSurvey& out) noexcept {
    Array<Transform> local(allocator());
    if (Status sized = local.resize(character.skeleton.joint_count()); !sized) {
        return sized;
    }
    ClipCursor cursor(allocator());
    SampleStats stats;
    out.sampled_fraction = static_cast<f32>(samples - 1U) / static_cast<f32>(samples);
    for (u32 index = 0; index < samples; ++index) {
        const f32 time = clip.duration() * (static_cast<f32>(index) / static_cast<f32>(samples));
        character.skeleton.reference_pose(local.span());
        if (Status sampled = clip.sample(time, JointMask::all(character.skeleton.joint_count()),
                                         cursor, local.span(), stats);
            !sampled) {
            return sampled;
        }
        survey_pose(character, local.span(), index == 0, out);
    }
    return ok();
}

/// Derive the correspondence between one export and the character and bake the export's clip onto
/// the character's skeleton — the offline half of "retargeting SHALL be performable offline and at
/// runtime".
[[nodiscard]] Status cross_onto(const ExportedRig& source, const Clip& animation,
                                const ExportedRig& character, RetargetProfile& profile,
                                RetargetBuildReport& report, Clip& baked) noexcept {
    if (Status built = build_retarget_profile(source.skeleton, source.humanoid, character.skeleton,
                                              character.humanoid, profile, report);
        !built) {
        return built;
    }
    return bake_clip(allocator(), profile, source.skeleton, character.skeleton, animation, 30.0F,
                     CompressionSettings{}, baked);
}

}  // namespace

CY_TEST_CASE("retarget exports: three separately exported clips drive one character's skeleton") {
    ExportedRig character(allocator());
    CY_REQUIRE(build_export(character, character_shape()).has_value());

    const SourceExport exports[] = {re_export(), reordered_export(), tall_export()};
    for (const SourceExport& exported : exports) {
        CY_TEST_MESSAGE("crossing ", std::string(exported.label));
        ExportedRig source(allocator());
        CY_REQUIRE(build_export(source, exported.rig).has_value());
        Clip animation(allocator());
        CY_REQUIRE(author_clip(animation, source, Name::intern("locomotion"), exported.animation)
                       .has_value());

        RetargetProfile profile(allocator());
        RetargetBuildReport report;
        Clip baked(allocator());
        CY_REQUIRE(cross_onto(source, animation, character, profile, report, baked).has_value());

        // The baked clip addresses THIS character's joints and nothing beyond them, which is the
        // check `AnimationRig::bind` cannot make for itself.
        CY_CHECK(baked.compressed());
        CY_CHECK_NEAR(baked.duration(), animation.duration(), 1e-4);
        CY_CHECK_GT(baked.track_count(), 0U);
        for (const TrackDesc& track : baked.tracks()) {
            CY_CHECK_LT(track.joint, character.skeleton.joint_count());
        }

        PoseSurvey survey;
        CY_REQUIRE(survey_clip(character, baked, 24U, survey).has_value());
        CY_CHECK(survey.finite);
        CY_CHECK(survey.normalised);
        // A human is about a metre from hip to fingertip and to toe; anything past two is a pose
        // that came apart.
        CY_CHECK_LT(survey.widest_reach, 2.0F);
        // The hips stay where a walking character's hips are: within a hand's width of their rest
        // height, on a skeleton whose hips rest at 0.99 m.
        CY_CHECK_GT(survey.lowest_hips, 0.9F);
        CY_CHECK_LT(survey.highest_hips, 1.1F);
        // And the character moves: the animation is a walk, so the hips travel along −Z.
        CY_CHECK_GT(survey.first_hips_z - survey.last_hips_z, 0.5F);
        // THE GUARD THIS SUITE EXISTS FOR. A correspondence that mapped nothing would pass every
        // check above and leave the character standing in its bind pose.
        CY_CHECK_GT(survey.worst_departure, 10.0F);
    }
}

CY_TEST_CASE(
    "retarget exports: a taller character's stride arrives in this character's own scale") {
    ExportedRig character(allocator());
    ExportedRig tall(allocator());
    const SourceExport exported = tall_export();
    CY_REQUIRE(build_export(character, character_shape()).has_value());
    CY_REQUIRE(build_export(tall, exported.rig).has_value());

    Clip animation(allocator());
    CY_REQUIRE(
        author_clip(animation, tall, Name::intern("stride"), exported.animation).has_value());

    RetargetProfile profile(allocator());
    RetargetBuildReport report;
    Clip baked(allocator());
    CY_REQUIRE(cross_onto(tall, animation, character, profile, report, baked).has_value());
    CY_CHECK_NEAR(report.retarget.height_scale, 1.0F / 1.3F, 1e-3);

    PoseSurvey survey;
    CY_REQUIRE(survey_clip(character, baked, 24U, survey).has_value());
    // 1.3 m of stride for a rig a third again as tall is 1.0 m here — the same fraction of its own
    // gait, which is what keeps the feet from sliding.
    CY_CHECK_NEAR(survey.first_hips_z - survey.last_hips_z, 1.0F * survey.sampled_fraction, 0.05F);
    // The hips sit at THIS character's height, not at the tall one's 1.287 m.
    CY_CHECK_LT(survey.highest_hips, 1.1F);
}

CY_TEST_CASE("retarget exports: the left leg's animation arrives on the left leg") {
    // The failure a nearly-right correspondence produces, and the one the milestone is about: the
    // source rig writes its legs before its spine, so its left thigh sits at an index this
    // character gave to a joint of the spine.
    ExportedRig character(allocator());
    ExportedRig source(allocator());
    const SourceExport exported = reordered_export();
    CY_REQUIRE(build_export(character, character_shape()).has_value());
    CY_REQUIRE(build_export(source, exported.rig).has_value());
    CY_REQUIRE_NE(source.joint(RigJoint::LeftUpperLeg), character.joint(RigJoint::LeftUpperLeg));

    Clip animation(allocator());
    CY_REQUIRE(author_clip(animation, source, Name::intern("run"), exported.animation).has_value());

    RetargetProfile profile(allocator());
    RetargetBuildReport report;
    Clip baked(allocator());
    CY_REQUIRE(cross_onto(source, animation, character, profile, report, baked).has_value());

    Array<Transform> retargeted(allocator());
    Array<Transform> naive(allocator());
    CY_REQUIRE(retargeted.resize(character.skeleton.joint_count()).has_value());
    CY_REQUIRE(naive.resize(character.skeleton.joint_count()).has_value());
    ClipCursor cursor(allocator());
    SampleStats stats;
    const f32 time = animation.duration() * 0.25F;

    character.skeleton.reference_pose(retargeted.span());
    CY_REQUIRE(baked
                   .sample(time, JointMask::all(character.skeleton.joint_count()), cursor,
                           retargeted.span(), stats)
                   .has_value());
    // The naive path: bind the source's own clip to this character because the joint counts agree,
    // which is all `AnimationRig::bind` checks.
    character.skeleton.reference_pose(naive.span());
    CY_REQUIRE(animation
                   .sample(time, JointMask::all(character.skeleton.joint_count()), cursor,
                           naive.span(), stats)
                   .has_value());

    // Retargeted, the left foot swings forward and the right foot swings back — the clip's own
    // phase. Bound naively, the joint that moves is not a leg at all.
    const f32 left =
        model_of(character.skeleton, retargeted.span(), character.joint(RigJoint::LeftFoot))
            .translation.z;
    const f32 right =
        model_of(character.skeleton, retargeted.span(), character.joint(RigJoint::RightFoot))
            .translation.z;
    CY_CHECK_GT(std::fabs(left - right), 0.3F);

    f32 worst = 0.0F;
    for (u16 joint = 0; joint < character.skeleton.joint_count(); ++joint) {
        worst = math::max(
            worst, math::degrees(angle_between(retargeted[joint].rotation, naive[joint].rotation)));
    }
    CY_CHECK_GT(worst, 20.0F);
}

CY_TEST_CASE("retarget exports: the runtime path and the baked clip agree") {
    // "Retargeting SHALL be performable offline (baking a retargeted clip at cook time) and at
    // runtime (mapping poses each evaluation)". Both are the same mapping, so a pose mapped live
    // and the same pose read out of the bake differ only by the clip codec's own error budget.
    ExportedRig character(allocator());
    ExportedRig source(allocator());
    const SourceExport exported = reordered_export();
    CY_REQUIRE(build_export(character, character_shape()).has_value());
    CY_REQUIRE(build_export(source, exported.rig).has_value());

    Clip animation(allocator());
    CY_REQUIRE(author_clip(animation, source, Name::intern("run"), exported.animation).has_value());
    RetargetProfile profile(allocator());
    RetargetBuildReport report;
    Clip baked(allocator());
    CY_REQUIRE(cross_onto(source, animation, character, profile, report, baked).has_value());

    Array<Transform> source_local(allocator());
    Array<Transform> live(allocator());
    Array<Transform> from_bake(allocator());
    CY_REQUIRE(source_local.resize(source.skeleton.joint_count()).has_value());
    CY_REQUIRE(live.resize(character.skeleton.joint_count()).has_value());
    CY_REQUIRE(from_bake.resize(character.skeleton.joint_count()).has_value());
    ClipCursor source_cursor(allocator());
    ClipCursor baked_cursor(allocator());
    SampleStats stats;

    f32 worst = 0.0F;
    for (u32 frame = 0; frame < 12U; ++frame) {
        const f32 time = animation.duration() * (static_cast<f32>(frame) / 11.0F);
        source.skeleton.reference_pose(source_local.span());
        CY_REQUIRE(animation
                       .sample(time, JointMask::all(source.skeleton.joint_count()), source_cursor,
                               source_local.span(), stats)
                       .has_value());
        character.skeleton.reference_pose(live.span());
        CY_REQUIRE(profile
                       .retarget_pose(source.skeleton, character.skeleton, source_local.span(),
                                      live.span())
                       .has_value());

        character.skeleton.reference_pose(from_bake.span());
        CY_REQUIRE(baked
                       .sample(time, JointMask::all(character.skeleton.joint_count()), baked_cursor,
                               from_bake.span(), stats)
                       .has_value());
        for (u16 joint = 0; joint < character.skeleton.joint_count(); ++joint) {
            worst = math::max(worst, math::degrees(angle_between(live[joint].rotation,
                                                                 from_bake[joint].rotation)));
        }
    }
    // The bake resamples at 30 Hz and recompresses; a degree is well inside what the codec's own
    // report calls its worst case, and well outside "the two paths disagree about the mapping".
    CY_CHECK_LT(worst, 1.0F);
}

CY_TEST_CASE("retarget exports: a retargeted clip drives the character through the real runtime") {
    // Sampling a clip is not the same as the runtime accepting it. `AnimationRig::bind` compares
    // the compiled program's joint count against the skeleton's, the instance carries a clock per
    // time parameter, and `evaluate()` copies out only the joints pose dependency analysis says are
    // written. A clip that crossed from another export has to survive all three, so the last case
    // here runs one through the path a game runs.
    ExportedRig character(allocator());
    ExportedRig source(allocator());
    const SourceExport exported = reordered_export();
    CY_REQUIRE(build_export(character, character_shape()).has_value());
    CY_REQUIRE(build_export(source, exported.rig).has_value());

    Clip animation(allocator());
    CY_REQUIRE(author_clip(animation, source, Name::intern("run"), exported.animation).has_value());
    RetargetProfile profile(allocator());
    RetargetBuildReport report;
    Clip baked(allocator());
    CY_REQUIRE(cross_onto(source, animation, character, profile, report, baked).has_value());

    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(graph::pose::register_pose_nodes(registry).has_value());
    graph::Graph graph(allocator(), Name::intern("locomotion"));
    graph::Literal clip_name;
    clip_name.type = Name::intern("name");
    clip_name.text = baked.name();
    graph::Literal time_parameter;
    time_parameter.type = Name::intern("name");
    time_parameter.text = Name::intern("run_time");
    CY_REQUIRE(graph.add_node(1, Name::intern("pose.clip")).has_value());
    CY_REQUIRE(graph.set_property(1, Name::intern("clip"), clip_name).has_value());
    CY_REQUIRE(graph.set_property(1, Name::intern("time_parameter"), time_parameter).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("pose.state")).has_value());
    CY_REQUIRE(graph.connect(1, Name::intern("pose"), 2, Name::intern("pose")).has_value());
    graph.resolve(registry);

    graph::DiagnosticSink sink(allocator());
    auto program =
        graph::pose::compile_pose(graph, registry, character.skeleton.joint_count(), sink);
    CY_REQUIRE(program.has_value());

    Array<const Clip*> table(allocator());
    CY_REQUIRE(table.resize(program.value().clips().size()).has_value());
    for (usize index = 0; index < table.size(); ++index) {
        table[index] = program.value().clips()[index].name == baked.name() ? &baked : nullptr;
    }
    AnimationRig rig(allocator());
    CY_REQUIRE(rig.bind(character.skeleton, program.value(), table.span()).has_value());
    AnimationInstance instance(allocator());
    CY_REQUIRE(instance.prepare(rig).has_value());
    PoseScratch scratch(allocator());
    CY_REQUIRE(scratch.prepare(rig).has_value());

    Array<Transform> pose(allocator());
    CY_REQUIRE(pose.resize(character.skeleton.joint_count()).has_value());
    CY_REQUIRE(advance(rig, instance, exported.animation.duration * 0.25F, nullptr).has_value());
    character.skeleton.reference_pose(pose.span());
    EvaluationStats stats;
    CY_REQUIRE(evaluate(rig, instance, 0, scratch, pose.span(), stats).has_value());

    CY_CHECK_EQ(stats.clips_sampled, 1U);
    f32 departure = 0.0F;
    for (u16 joint = 0; joint < character.skeleton.joint_count(); ++joint) {
        CY_CHECK(math::is_finite(pose[joint].translation.y));
        departure = math::max(
            departure,
            math::degrees(angle_between(pose[joint].rotation,
                                        character.skeleton.joints()[joint].bind_local.rotation)));
    }
    CY_CHECK_GT(departure, 10.0F);
}
