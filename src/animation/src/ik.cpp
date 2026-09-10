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

/// The interior angle at `b` of a triangle with the given side lengths, or zero when the triangle
/// is degenerate.
[[nodiscard]] f32 triangle_angle(f32 adjacent_a, f32 adjacent_b, f32 opposite) noexcept {
    const f32 denominator = 2.0F * adjacent_a * adjacent_b;
    if (denominator <= math::kSmallLength) {
        return 0.0F;
    }
    const f32 cosine =
        ((adjacent_a * adjacent_a) + (adjacent_b * adjacent_b) - (opposite * opposite)) /
        denominator;
    return safe_acos(cosine);
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

    const Transform root_model = model_of(skeleton, chain.root, local);
    const Transform mid_model = model_of(skeleton, chain.mid, local);
    const Transform tip_model = model_of(skeleton, chain.tip, local);

    const f32 upper = length(mid_model.translation - root_model.translation);
    const f32 lower = length(tip_model.translation - mid_model.translation);
    const Vec3 to_target = target - root_model.translation;
    const f32 distance = length(to_target);
    if (distance <= math::kSmallLength || upper <= math::kSmallLength ||
        lower <= math::kSmallLength) {
        return false;
    }
    const bool reachable = distance < (upper + lower);
    const f32 reach = math::min(distance, (upper + lower) * 0.999F);

    // The two angles the law of cosines gives, against the two the chain has now. Working in
    // deltas keeps whatever twist the animation authored on the chain.
    const f32 current_root =
        triangle_angle(upper, length(tip_model.translation - root_model.translation), lower);
    const f32 wanted_root = triangle_angle(upper, reach, lower);
    const f32 current_mid =
        triangle_angle(upper, lower, length(tip_model.translation - root_model.translation));
    const f32 wanted_mid = triangle_angle(upper, lower, reach);

    const Vec3 chain_axis = normalize(mid_model.translation - root_model.translation);
    Vec3 bend_axis = cross(chain_axis, chain.pole);
    if (length_squared(bend_axis) <= math::kSmallLength) {
        bend_axis = cross(chain_axis, Vec3{0.0F, 1.0F, 0.0F});
    }
    if (length_squared(bend_axis) <= math::kSmallLength) {
        return false;
    }
    bend_axis = normalize(bend_axis);

    const Quat root_bend = Quat::from_axis_angle(bend_axis, (wanted_root - current_root) * blend);
    const Quat mid_bend = Quat::from_axis_angle(bend_axis, (wanted_mid - current_mid) * blend);

    // Rotate the root so the chain points at the target, then apply the two bends. Both are applied
    // in model space and written back through the parent's inverse, so a chain deep in a hierarchy
    // is solved the same way as one at the top.
    const Vec3 current_direction = normalize(tip_model.translation - root_model.translation);
    const Vec3 wanted_direction = normalize(to_target);
    const Quat swing = Quat::from_to(current_direction, wanted_direction);
    const Quat root_delta = normalize(slerp(Quat::identity(), swing, blend) * root_bend);

    const Transform root_parent =
        skeleton.joints()[chain.root].parent == kInvalidJoint
            ? Transform::identity()
            : model_of(skeleton, skeleton.joints()[chain.root].parent, local);
    const Quat root_parent_inverse = conjugate(normalize(root_parent.rotation));
    local[chain.root].rotation = normalize(root_parent_inverse * root_delta * root_model.rotation);
    local[chain.mid].rotation = normalize(local[chain.mid].rotation * mid_bend);
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
            return fail(ErrorCode::NotImplemented,
                        "foot placement raycasts the ground, which this module may not do; it is "
                        "not in M8.b");
    }
    return fail(ErrorCode::NotImplemented, "no such solver");
}

}  // namespace cy::animation
