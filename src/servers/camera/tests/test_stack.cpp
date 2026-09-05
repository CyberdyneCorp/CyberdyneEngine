// The camera stack: priorities, blends, additive contributions and the report. Task 4.3.2.
//
// `camera-system` — "Camera stack and blending". The two cases that carry the requirement are the
// cinematic that takes over and returns without a snap, and the report that says which contribution
// moved the camera — the second is what turns "the camera is not where I expect" from a debugging
// session into a reading.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/camera/stack.h>
#include <cy/test/test.h>

using cy::f32;
using namespace cy::camera;

namespace {

cy::Allocator& allocator() {
    return cy::system_allocator(cy::MemoryDomain::Engine);
}

[[nodiscard]] EvaluatedCamera camera_at(f32 x) {
    EvaluatedCamera camera;
    camera.pose.translation = cy::Vec3{x, 0.0F, 0.0F};
    return camera;
}

[[nodiscard]] StackEntry entry(RigHandle rig, ContributionKind kind, cy::i32 priority) {
    StackEntry result;
    result.rig = rig;
    result.kind = kind;
    result.priority = priority;
    result.blend_in.duration_seconds = 0.0F;  // instant unless a case says otherwise
    result.blend_out.duration_seconds = 0.0F;
    return result;
}

const RigHandle kRigA = RigHandle::from_slot(0, 1);
const RigHandle kRigB = RigHandle::from_slot(1, 1);
const RigHandle kRigC = RigHandle::from_slot(2, 1);

}  // namespace

CY_TEST_CASE("an entry naming no rig is refused") {
    CameraStack stack(allocator());
    CY_CHECK_FALSE(static_cast<bool>(stack.push(entry(RigHandle{}, ContributionKind::Base, 0))));
}

CY_TEST_CASE("entries resolve by priority, and ties by push order") {
    // `simulation-and-determinism` requires anything ordered to be ordered by something stated. Two
    // entries at one priority resolve in the order they were pushed and never in a container's.
    CameraStack stack(allocator());
    CY_REQUIRE(static_cast<bool>(stack.push(entry(kRigC, ContributionKind::Cinematic, 10))));
    CY_REQUIRE(static_cast<bool>(stack.push(entry(kRigA, ContributionKind::Base, 0))));
    CY_REQUIRE(static_cast<bool>(stack.push(entry(kRigB, ContributionKind::Effect, 0))));

    CY_REQUIRE_EQ(stack.size(), 3U);
    CY_CHECK(stack.entry_at(0).rig == kRigA);
    CY_CHECK(stack.entry_at(1).rig == kRigB);
    CY_CHECK(stack.entry_at(2).rig == kRigC);
}

CY_TEST_CASE("blend refuses a mismatched number of evaluated cameras") {
    // A caller that lost track of which camera belongs to which entry would otherwise blend the
    // wrong pose at the right weight, and nothing would look wrong enough to notice.
    CameraStack stack(allocator());
    CY_REQUIRE(static_cast<bool>(stack.push(entry(kRigA, ContributionKind::Base, 0))));
    CY_REQUIRE(static_cast<bool>(stack.push(entry(kRigB, ContributionKind::Cinematic, 10))));

    const EvaluatedCamera one[] = {camera_at(0.0F)};
    EvaluatedCamera out;
    CY_CHECK_FALSE(
        static_cast<bool>(stack.blend(cy::Span<const EvaluatedCamera>(one, 1), out, nullptr)));
}

CY_TEST_CASE("a cinematic takes over and returns without a snap") {
    // `camera-system`'s own scenario. The gameplay camera keeps evaluating throughout, which is why
    // there is something to return to.
    CameraStack stack(allocator());
    CY_REQUIRE(static_cast<bool>(stack.push(entry(kRigA, ContributionKind::Base, 0))));

    StackEntry cinematic = entry(kRigB, ContributionKind::Cinematic, 100);
    cinematic.blend_in.duration_seconds = 1.0F;
    cinematic.blend_in.curve = BlendCurve::Linear;
    cinematic.blend_out.duration_seconds = 1.0F;
    cinematic.blend_out.curve = BlendCurve::Linear;
    const auto id = stack.push(cinematic);
    CY_REQUIRE(static_cast<bool>(id));

    const EvaluatedCamera evaluated[] = {camera_at(0.0F), camera_at(10.0F)};
    const cy::Span<const EvaluatedCamera> span(evaluated, 2);
    EvaluatedCamera out;

    // At zero elapsed the cinematic has no weight: the result is the gameplay camera.
    CY_REQUIRE(static_cast<bool>(stack.blend(span, out, nullptr)));
    CY_CHECK_NEAR(out.pose.translation.x, 0.0F, 1e-5F);

    stack.advance(0.5F);
    CY_REQUIRE(static_cast<bool>(stack.blend(span, out, nullptr)));
    CY_CHECK_NEAR(out.pose.translation.x, 5.0F, 1e-4F);

    stack.advance(0.5F);
    CY_REQUIRE(static_cast<bool>(stack.blend(span, out, nullptr)));
    CY_CHECK_NEAR(out.pose.translation.x, 10.0F, 1e-4F);

    // Released: it blends back out, and the entry is gone when it reaches zero.
    CY_REQUIRE(static_cast<bool>(stack.release(*id)));
    stack.advance(0.5F);
    CY_REQUIRE(static_cast<bool>(stack.blend(span, out, nullptr)));
    CY_CHECK_NEAR(out.pose.translation.x, 5.0F, 1e-4F);

    stack.advance(0.6F);
    CY_CHECK_EQ(stack.size(), 1U);
}

CY_TEST_CASE("a contribution released mid blend-in fades from where it was") {
    CameraStack stack(allocator());
    CY_REQUIRE(static_cast<bool>(stack.push(entry(kRigA, ContributionKind::Base, 0))));
    StackEntry effect = entry(kRigB, ContributionKind::Effect, 5);
    effect.blend_in.duration_seconds = 1.0F;
    effect.blend_in.curve = BlendCurve::Linear;
    effect.blend_out.duration_seconds = 1.0F;
    effect.blend_out.curve = BlendCurve::Linear;
    const auto id = stack.push(effect);
    CY_REQUIRE(static_cast<bool>(id));

    stack.advance(0.25F);
    CY_CHECK_NEAR(stack.weight_at(1), 0.25F, 1e-4F);
    CY_REQUIRE(static_cast<bool>(stack.release(*id)));
    // Immediately after release it is still at 0.25, not at 1.0.
    CY_CHECK_NEAR(stack.weight_at(1), 0.25F, 1e-4F);
    stack.advance(0.5F);
    CY_CHECK_NEAR(stack.weight_at(1), 0.125F, 1e-3F);
}

CY_TEST_CASE("an additive contribution offsets the base rather than replacing it") {
    // Blending toward a small offset would put the camera near the world origin every time a weapon
    // fired, which is what makes `Additive` a different operation rather than a different weight.
    CameraStack stack(allocator());
    CY_REQUIRE(static_cast<bool>(stack.push(entry(kRigA, ContributionKind::Base, 0))));
    CY_REQUIRE(static_cast<bool>(stack.push(entry(kRigB, ContributionKind::Additive, 5))));

    const EvaluatedCamera evaluated[] = {camera_at(100.0F), camera_at(0.5F)};
    EvaluatedCamera out;
    CY_REQUIRE(static_cast<bool>(
        stack.blend(cy::Span<const EvaluatedCamera>(evaluated, 2), out, nullptr)));
    CY_CHECK_NEAR(out.pose.translation.x, 100.5F, 1e-4F);
}

CY_TEST_CASE("a per-channel policy blends the lens without moving the camera") {
    CameraStack stack(allocator());
    CY_REQUIRE(static_cast<bool>(stack.push(entry(kRigA, ContributionKind::Base, 0))));
    StackEntry lens_only = entry(kRigB, ContributionKind::Volume, 5);
    lens_only.blend_in.position = false;
    lens_only.blend_in.rotation = false;
    lens_only.blend_in.lens = true;
    CY_REQUIRE(static_cast<bool>(stack.push(lens_only)));

    EvaluatedCamera base = camera_at(0.0F);
    base.lens.gameplay.vertical_fov_radians = 1.0F;
    EvaluatedCamera volume = camera_at(50.0F);
    volume.lens.gameplay.vertical_fov_radians = 2.0F;
    const EvaluatedCamera evaluated[] = {base, volume};

    EvaluatedCamera out;
    CY_REQUIRE(static_cast<bool>(
        stack.blend(cy::Span<const EvaluatedCamera>(evaluated, 2), out, nullptr)));
    CY_CHECK_NEAR(out.pose.translation.x, 0.0F, 1e-5F);
    CY_CHECK_NEAR(out.lens.vertical_fov_radians(), 2.0F, 1e-5F);
}

CY_TEST_CASE("the report says which contribution moved the camera and by how much") {
    // `camera-system`: "**WHEN** the camera is not where a developer expects **THEN** the stack
    // SHALL show each contribution and its weight."
    CameraStack stack(allocator());
    CY_REQUIRE(static_cast<bool>(stack.push(entry(kRigA, ContributionKind::Base, 0))));
    StackEntry cinematic = entry(kRigB, ContributionKind::Cinematic, 100);
    cinematic.target_weight = 0.5F;
    CY_REQUIRE(static_cast<bool>(stack.push(cinematic)));

    const EvaluatedCamera evaluated[] = {camera_at(0.0F), camera_at(10.0F)};
    cy::Array<StackContribution> report(allocator());
    EvaluatedCamera out;
    CY_REQUIRE(static_cast<bool>(
        stack.blend(cy::Span<const EvaluatedCamera>(evaluated, 2), out, &report)));

    CY_REQUIRE_EQ(report.size(), 2U);
    CY_CHECK_EQ(report[0].kind, ContributionKind::Base);
    CY_CHECK_NEAR(report[0].weight, 1.0F, 1e-5F);
    CY_CHECK_EQ(report[1].kind, ContributionKind::Cinematic);
    CY_CHECK_NEAR(report[1].weight, 0.5F, 1e-5F);
    CY_CHECK_NEAR(report[1].position_delta, 5.0F, 1e-4F);
    CY_CHECK_NEAR(out.pose.translation.x, 5.0F, 1e-4F);
}

CY_TEST_CASE("the identity and the cut travel with the entry in control") {
    // Averaging two history identities would name a third view that never existed.
    CameraStack stack(allocator());
    CY_REQUIRE(static_cast<bool>(stack.push(entry(kRigA, ContributionKind::Base, 0))));
    CY_REQUIRE(static_cast<bool>(stack.push(entry(kRigB, ContributionKind::Cinematic, 100))));

    EvaluatedCamera base = camera_at(0.0F);
    base.history_id = 11;
    EvaluatedCamera cinematic = camera_at(10.0F);
    cinematic.history_id = 22;
    cinematic.cut_epoch = 3;
    cinematic.last_cut.reason = CutReason::CinematicStart;
    const EvaluatedCamera evaluated[] = {base, cinematic};

    EvaluatedCamera out;
    CY_REQUIRE(static_cast<bool>(
        stack.blend(cy::Span<const EvaluatedCamera>(evaluated, 2), out, nullptr)));
    CY_CHECK_EQ(out.history_id, 22U);
    CY_CHECK_EQ(out.cut_epoch, 3U);
    CY_CHECK_EQ(out.last_cut.reason, CutReason::CinematicStart);
}

CY_TEST_CASE("the blend curves have the shapes their names claim") {
    CY_CHECK_NEAR(apply_curve(BlendCurve::Linear, 0.5F), 0.5F, 1e-6F);
    CY_CHECK_NEAR(apply_curve(BlendCurve::EaseIn, 0.5F), 0.25F, 1e-6F);
    CY_CHECK_NEAR(apply_curve(BlendCurve::EaseOut, 0.5F), 0.75F, 1e-6F);
    CY_CHECK_NEAR(apply_curve(BlendCurve::EaseInOut, 0.5F), 0.5F, 1e-6F);
    // A step curve is a cut spelled as a transition, so it is at full weight from the first frame.
    CY_CHECK_NEAR(apply_curve(BlendCurve::Step, 0.01F), 1.0F, 1e-6F);
    CY_CHECK_NEAR(apply_curve(BlendCurve::Step, 0.0F), 0.0F, 1e-6F);
    // And every curve is clamped at both ends.
    CY_CHECK_NEAR(apply_curve(BlendCurve::EaseInOut, -1.0F), 0.0F, 1e-6F);
    CY_CHECK_NEAR(apply_curve(BlendCurve::EaseInOut, 2.0F), 1.0F, 1e-6F);
}
