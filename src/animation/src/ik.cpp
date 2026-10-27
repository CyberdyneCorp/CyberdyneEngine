#include <cy/animation/ik.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::animation {
namespace {

/// The model-space placement of one joint, walked up the parent chain. Used by the solvers, which
/// receive a local pose rather than a model one so a caller does not have to resolve the whole
/// skeleton to fade one constraint in.
[[nodiscard]] Transform model_of(const Skeleton& skeleton, u16 joint,
                                 Span<const Transform> local) noexcept {
    Transform accumulated = Transform::identity();
    u16 cursor = joint;
    while (cursor != kInvalidJoint && cursor < local.size()) {
        accumulated = local[cursor] * accumulated;
        cursor = skeleton.joints()[cursor].parent;
    }
    return accumulated;
}

[[nodiscard]] f32 safe_acos(f32 value) noexcept {
    return std::acos(math::clamp(value, -1.0F, 1.0F));
}

/// The lowest joint a mask holds, so a conflict points at one joint rather than at a mask.
[[nodiscard]] u16 lowest_joint(const JointMask& mask) noexcept {
    for (u32 index = 0; index < kMaxJoints; ++index) {
        if (mask.test(index)) {
            return static_cast<u16>(index);
        }
    }
    return kInvalidJoint;
}

}  // namespace

Status detect_conflicts(Span<const ConstraintDecl> constraints,
                        Array<ConstraintConflict>& conflicts, ConflictReport& report) noexcept {
    report = ConflictReport{};
    report.examined = static_cast<u32>(constraints.size());
    for (usize first = 0; first < constraints.size(); ++first) {
        for (usize second = first + 1; second < constraints.size(); ++second) {
            if (constraints[first].order != constraints[second].order) {
                continue;
            }
            const JointMask both =
                constraints[first].writes.intersected(constraints[second].writes);
            if (both.empty()) {
                continue;
            }
            if (Status pushed = conflicts.push_back(ConstraintConflict{
                    constraints[first].name, constraints[second].name, lowest_joint(both)});
                !pushed) {
                return pushed;
            }
            ++report.conflicts;
        }
    }
    return ok();
}

Expected<bool, Error> solve_two_bone(const Skeleton& skeleton, const IkChain& chain, Vec3 target,
                                     f32 weight, Span<Transform> local) noexcept {
    if (!chain.valid()) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a two-bone chain names three joints", 0});
    }
    const u16 count = skeleton.joint_count();
    if (chain.root >= count || chain.mid >= count || chain.tip >= count ||
        local.size() < static_cast<usize>(count)) {
        return make_unexpected(Error{ErrorCode::OutOfRange,
                                     "an inverse-kinematics chain names a joint the skeleton "
                                     "does not have",
                                     0});
    }
    if (skeleton.joints()[chain.mid].parent != chain.root ||
        skeleton.joints()[chain.tip].parent != chain.mid) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "a two-bone chain is root, its child and its grandchild", 0});
    }
    const f32 blend = math::clamp(weight, 0.0F, 1.0F);
    if (blend <= 0.0F) {
        return false;
    }

    const u16 parent = skeleton.joints()[chain.root].parent;
    const Transform parent_model =
        parent == kInvalidJoint ? Transform::identity() : model_of(skeleton, parent, local);
    const Transform root_model = parent_model * local[chain.root];
    const Transform mid_model = root_model * local[chain.mid];
    const Transform tip_model = mid_model * local[chain.tip];

    const Vec3 root = root_model.translation;
    const Vec3 mid = mid_model.translation;
    const Vec3 tip = tip_model.translation;
    const f32 upper = length(mid - root);
    const f32 lower = length(tip - mid);
    const Vec3 to_target = target - root;
    const f32 distance = length(to_target);
    if (distance <= math::kSmallLength || upper <= math::kSmallLength ||
        lower <= math::kSmallLength) {
        return false;
    }
    const bool reachable = distance < (upper + lower);
    const f32 longest = (upper + lower) * 0.9999F;
    const f32 shortest = math::max(std::fabs(upper - lower) * 1.0001F, math::kSmallLength);
    const f32 reach = math::clamp(distance, shortest, longest);

    // WHERE THE KNEE GOES, solved directly rather than as two angle deltas. The knee lies on the
    // circle the two bone lengths allow around the root-to-target axis; the pole picks the point
    // of that circle, and the knee's current offset breaks the tie when the pole is on the axis.
    const Vec3 axis = to_target * (1.0F / distance);
    const f32 along = ((upper * upper) - (lower * lower) + (reach * reach)) / (2.0F * reach);
    const f32 out = std::sqrt(math::max((upper * upper) - (along * along), 0.0F));
    Vec3 bend = chain.pole - (axis * dot(chain.pole, axis));
    if (length_squared(bend) <= math::kSmallLength) {
        const Vec3 knee = mid - root;
        bend = knee - (axis * dot(knee, axis));
    }
    if (length_squared(bend) <= math::kSmallLength) {
        bend = cross(axis, Vec3{1.0F, 0.0F, 0.0F});
    }
    if (length_squared(bend) <= math::kSmallLength) {
        bend = cross(axis, Vec3{0.0F, 0.0F, 1.0F});
    }
    const Vec3 knee = root + (axis * along) + (normalize(bend) * out);
    const Vec3 reached = root + (axis * reach);

    // Two minimal rotations in model space, written back through each parent's inverse, so a chain
    // under a rotated parent is solved the same way as one at the top and keeps its twist.
    const Quat swing_root = Quat::from_to(normalize(mid - root), normalize(knee - root));
    const Quat root_rotation = normalize(swing_root * root_model.rotation);
    const Vec3 tip_after = knee + (swing_root * (tip - mid));
    const Quat swing_mid = Quat::from_to(normalize(tip_after - knee), normalize(reached - knee));
    const Quat mid_rotation = normalize(swing_mid * swing_root * mid_model.rotation);

    const Quat solved_root = normalize(conjugate(normalize(parent_model.rotation)) * root_rotation);
    const Quat solved_mid = normalize(conjugate(root_rotation) * mid_rotation);
    local[chain.root].rotation = slerp(local[chain.root].rotation, solved_root, blend);
    local[chain.mid].rotation = slerp(local[chain.mid].rotation, solved_mid, blend);
    return reachable;
}

Status solve_look_at(const Skeleton& skeleton, u16 joint, Vec3 target, f32 weight,
                     Span<Transform> local) noexcept {
    if (joint >= skeleton.joint_count() || local.size() < skeleton.joint_count()) {
        return fail(ErrorCode::OutOfRange, "no such joint");
    }
    const f32 blend = math::clamp(weight, 0.0F, 1.0F);
    if (blend <= 0.0F) {
        return ok();
    }
    const Transform model = model_of(skeleton, joint, local);
    const Vec3 to_target = target - model.translation;
    if (length_squared(to_target) <= math::kSmallLength) {
        return ok();
    }
    const Quat wanted = Quat::look_rotation(normalize(to_target));
    const Transform parent = skeleton.joints()[joint].parent == kInvalidJoint
                                 ? Transform::identity()
                                 : model_of(skeleton, skeleton.joints()[joint].parent, local);
    const Quat in_parent = normalize(conjugate(normalize(parent.rotation)) * wanted);
    local[joint].rotation = slerp(local[joint].rotation, in_parent, blend);
    return ok();
}

Transform joint_model(const Skeleton& skeleton, u16 joint, Span<const Transform> local) noexcept {
    return model_of(skeleton, joint, local);
}

namespace {

/// Turn joint `joint` by `turn` in model space, about its own position: its descendants follow.
void turn_in_model(const Skeleton& skeleton, u16 joint, const Quat& turn,
                   Span<Transform> local) noexcept {
    const u16 parent = skeleton.joints()[joint].parent;
    const Quat parent_rotation = parent == kInvalidJoint
                                     ? Quat::identity()
                                     : normalize(model_of(skeleton, parent, local).rotation);
    const Quat model = normalize(parent_rotation * local[joint].rotation);
    local[joint].rotation = normalize(conjugate(parent_rotation) * (turn * model));
}

/// `turn` with its angle capped at `limit` radians.
[[nodiscard]] Quat capped(const Quat& turn, f32 limit) noexcept {
    const f32 angle = 2.0F * safe_acos(std::fabs(normalize(turn).w));
    if (angle <= limit || angle <= math::kSmallLength) {
        return turn;
    }
    return slerp(Quat::identity(), turn, limit / angle);
}

}  // namespace

namespace {

[[nodiscard]] Status check_aim(const Skeleton& skeleton, const AimChain& chain,
                               Span<Transform> local) noexcept {
    if (!chain.valid()) {
        return fail(ErrorCode::InvalidArgument, "an aim chain names one to four joints");
    }
    if (local.size() < skeleton.joint_count()) {
        return fail(ErrorCode::InvalidArgument, "the pose is smaller than the skeleton");
    }
    for (u32 index = 0; index < chain.count; ++index) {
        if (chain.joints[index] >= skeleton.joint_count()) {
            return fail(ErrorCode::OutOfRange, "an aim chain names a joint the skeleton lacks");
        }
    }
    return ok();
}

/// One pass: the turn that points the chain's last joint at `target`, capped at `budget`, shared
/// equally — n turns by one rotation about one axis compose to the whole. Returns the angle used.
[[nodiscard]] f32 aim_pass(const Skeleton& skeleton, const AimChain& chain, Vec3 target, f32 budget,
                           Span<Transform> local) noexcept {
    const Transform model = model_of(skeleton, chain.joints[chain.count - 1], local);
    const Vec3 forward = normalize(model.rotation * chain.axis);
    const Vec3 to_target = target - model.translation;
    if (length_squared(to_target) <= math::kSmallLength) {
        return budget;
    }
    const Quat turn = capped(Quat::from_to(forward, normalize(to_target)), budget);
    const Quat share = slerp(Quat::identity(), turn, 1.0F / static_cast<f32>(chain.count));
    for (u32 index = 0; index < chain.count; ++index) {
        turn_in_model(skeleton, chain.joints[index], share, local);
    }
    return 2.0F * safe_acos(std::fabs(normalize(turn).w));
}

}  // namespace

Status solve_aim(const Skeleton& skeleton, const AimChain& chain, Vec3 target, f32 weight,
                 Span<Transform> local) noexcept {
    if (Status checked = check_aim(skeleton, chain, local); !checked) {
        return checked;
    }
    const f32 blend = math::clamp(weight, 0.0F, 1.0F);
    if (blend <= 0.0F) {
        return ok();
    }
    Quat original[kMaxAimJoints];
    for (u32 index = 0; index < chain.count; ++index) {
        original[index] = local[chain.joints[index]].rotation;
    }
    // Twice: the pointing joint's own position moves as the joints above it turn, and the second
    // pass removes the parallax the first one left.
    f32 budget = chain.max_angle;
    for (u32 pass = 0; pass < 2 && budget > 0.0F; ++pass) {
        budget -= aim_pass(skeleton, chain, target, budget, local);
    }
    for (u32 index = 0; index < chain.count && blend < 1.0F; ++index) {
        Transform& joint = local[chain.joints[index]];
        joint.rotation = slerp(original[index], joint.rotation, blend);
    }
    return ok();
}

Status solve_unimplemented(UnimplementedSolver solver) noexcept {
    switch (solver) {
        case UnimplementedSolver::Fabrik:
            return fail(ErrorCode::NotImplemented, "the FABRIK solver is not in M8.b");
        case UnimplementedSolver::Ccd:
            return fail(ErrorCode::NotImplemented,
                        "the cyclic-coordinate-descent solver is not in "
                        "M8.b");
        case UnimplementedSolver::SplineIk:
            return fail(ErrorCode::NotImplemented,
                        "the spline inverse-kinematics solver is not in "
                        "M8.b");
        case UnimplementedSolver::FullBodyIk:
            return fail(ErrorCode::NotImplemented,
                        "full-body inverse kinematics solves several effectors as one pose problem "
                        "and is not in M8.b; a chain solver is not it wearing its name");
        case UnimplementedSolver::SpringBones:
            return fail(ErrorCode::NotImplemented, "spring bones are not in M8.b");
        case UnimplementedSolver::FootPlacement:
            // Built since issue #92, behind a different entry point: the ground is the host's, so
            // the solver takes a `GroundProbe` and cannot be called through this one.
            return fail(ErrorCode::NotImplemented,
                        "foot placement asks the host for the ground: call solve_foot_placement "
                        "in foot_placement.h with a GroundProbe");
    }
    return fail(ErrorCode::NotImplemented, "no such solver");
}

}  // namespace cy::animation
