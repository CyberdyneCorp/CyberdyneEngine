// SPDX-License-Identifier: MIT
// Foot placement. See cy/animation/foot_placement.h for what the solve does and why the ground is
// asked for rather than queried.

#include <cy/animation/foot_placement.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::animation {
namespace {

constexpr Vec3 kUp{0.0F, 1.0F, 0.0F};

/// `turn` with its angle capped at `limit` radians.
[[nodiscard]] Quat capped_turn(const Quat& turn, f32 limit) noexcept {
    const f32 w = math::clamp(std::fabs(normalize(turn).w), 0.0F, 1.0F);
    const f32 angle = 2.0F * std::acos(w);
    if (angle <= limit || angle <= math::kSmallLength) {
        return turn;
    }
    return slerp(Quat::identity(), turn, limit / angle);
}

/// The ground normal a foot is turned to: `normal` (model space), no steeper than `limit`.
[[nodiscard]] Vec3 tilted_normal(Vec3 normal, f32 limit) noexcept {
    return normalize(capped_turn(Quat::from_to(kUp, normal), limit) * kUp);
}

/// How far a foot's ankle moves vertically to stand on ground at height `ground` (model space)
/// facing `normal`: the ground's height, plus what the tilt adds to the ankle's height over the
/// sole. An ankle `ankle_height` above a plane along its normal is `ankle_height / normal.y` above
/// it vertically.
[[nodiscard]] f32 ankle_offset(f32 ground, Vec3 normal, f32 ankle_height) noexcept {
    const f32 upright = math::max(normal.y, 0.2F);
    return ground + (ankle_height * ((1.0F / upright) - 1.0F));
}

/// A first-order follower: `alpha` of the way to the target, so a time constant `smoothing` over
/// `elapsed` seconds is frame-rate independent.
[[nodiscard]] f32 follow_alpha(const FootPlacementState& state, f32 smoothing,
                               f32 elapsed) noexcept {
    if (!state.primed || smoothing <= 0.0F) {
        return 1.0F;
    }
    if (elapsed <= 0.0F) {
        return 0.0F;
    }
    return 1.0F - std::exp(-elapsed / smoothing);
}

[[nodiscard]] Status check_setup(const Skeleton& skeleton, const FootPlacementSetup& setup,
                                 Span<Transform> local) noexcept {
    const u16 count = skeleton.joint_count();
    if (!setup.valid() || setup.pelvis >= count || local.size() < count) {
        return fail(ErrorCode::InvalidArgument,
                    "foot placement names a pelvis and two legs of the skeleton it is solved on");
    }
    for (const IkChain& leg : setup.legs) {
        if (leg.root >= count || leg.mid >= count || leg.tip >= count ||
            skeleton.joints()[leg.mid].parent != leg.root ||
            skeleton.joints()[leg.tip].parent != leg.mid) {
            return fail(ErrorCode::InvalidArgument,
                        "a foot placement leg is a hip, its knee and the knee's ankle");
        }
    }
    return ok();
}

/// The rotation of `joint`'s parent in model space, or the identity at the root.
[[nodiscard]] Transform parent_model(const Skeleton& skeleton, u16 joint,
                                     Span<const Transform> local) noexcept {
    const u16 parent = skeleton.joints()[joint].parent;
    return parent == kInvalidJoint ? Transform::identity() : joint_model(skeleton, parent, local);
}

/// The targets of one solve, before smoothing: per foot the offset and the normal (model space),
/// and what the probe answered.
struct Targets {
    f32 offset[2] = {0.0F, 0.0F};
    Vec3 normal[2] = {kUp, kUp};
    f32 pelvis = 0.0F;
};

[[nodiscard]] Targets probe_targets(const FootPlacementSetup& setup,
                                    const FootPlacementContext& context, const GroundProbe& probe,
                                    const Vec3 (&ankles)[2], FootPlacementReport& report) noexcept {
    Targets targets;
    const Transform& placement = context.model_to_world;
    const Quat to_model = conjugate(normalize(placement.rotation));
    const Transform world_to_model = inverse(placement);
    for (u32 leg = 0; leg < 2; ++leg) {
        const Vec3 ankle_world = placement.transform_point(ankles[leg]);
        GroundQuery query;
        query.origin = ankle_world + Vec3{0.0F, setup.probe_above, 0.0F};
        query.length = setup.probe_above + setup.probe_below;
        query.owner = context.owner;
        const GroundHit hit = probe.cast_down(query);
        report.hit[leg] = hit.hit;
        if (!hit.hit) {
            continue;
        }
        report.ground[leg] = hit.point;
        report.normal[leg] = hit.normal;
        const Vec3 ground = world_to_model.transform_point(hit.point);
        const Vec3 normal = tilted_normal(normalize(to_model * hit.normal), setup.max_tilt);
        targets.normal[leg] = normal;
        targets.offset[leg] = math::clamp(ankle_offset(ground.y, normal, setup.ankle_height),
                                          -setup.max_pelvis_drop, setup.max_foot_raise);
    }
    targets.pelvis = math::clamp(math::min(targets.offset[0], targets.offset[1]),
                                 -setup.max_pelvis_drop, setup.max_foot_raise);
    return targets;
}

void follow(FootPlacementState& state, const Targets& targets, f32 alpha) noexcept {
    state.pelvis += (targets.pelvis - state.pelvis) * alpha;
    for (u32 leg = 0; leg < 2; ++leg) {
        state.offset[leg] += (targets.offset[leg] - state.offset[leg]) * alpha;
        const Vec3 blended =
            state.normal[leg] + ((targets.normal[leg] - state.normal[leg]) * alpha);
        state.normal[leg] = length_squared(blended) > math::kSmallLength ? normalize(blended) : kUp;
    }
    state.primed = true;
}

/// Move the pelvis by `rise` metres along the model's up, through its parent's frame.
void raise_pelvis(const Skeleton& skeleton, u16 pelvis, f32 rise, Span<Transform> local) noexcept {
    const Transform parent = parent_model(skeleton, pelvis, local);
    const Vec3 in_parent = conjugate(normalize(parent.rotation)) * Vec3{0.0F, rise, 0.0F};
    const Vec3 scale = parent.scale;
    local[pelvis].translation =
        local[pelvis].translation +
        Vec3{in_parent.x / scale.x, in_parent.y / scale.y, in_parent.z / scale.z};
}

/// Give the ankle the model-space orientation the ANIMATION gave it, `animated`, turned from the
/// model's up towards `normal`, weighted. The leg solve turned the knee, and the foot below it
/// would otherwise turn with the shin rather than stay flat on the ground.
void tilt_foot(const Skeleton& skeleton, u16 ankle, const Quat& animated, Vec3 normal, f32 weight,
               Span<Transform> local) noexcept {
    const Quat tilt = slerp(Quat::identity(), Quat::from_to(kUp, normal), weight);
    const Quat parent_rotation = normalize(parent_model(skeleton, ankle, local).rotation);
    const Quat wanted = normalize(tilt * animated);
    const Quat solved = normalize(conjugate(parent_rotation) * wanted);
    local[ankle].rotation = slerp(local[ankle].rotation, solved, weight);
}

}  // namespace

PlaneGround::PlaneGround(Vec3 point, Vec3 normal) noexcept
    : point_(point), normal_(length_squared(normal) > 0.0F ? normalize(normal) : kUp) {}

GroundHit PlaneGround::cast_down(const GroundQuery& query) const noexcept {
    GroundHit hit;
    // The ray is straight down: it meets the plane where the plane's height under the origin is.
    if (std::fabs(normal_.y) <= math::kSmallLength) {
        return hit;
    }
    const f32 height =
        point_.y -
        (((normal_.x * (query.origin.x - point_.x)) + (normal_.z * (query.origin.z - point_.z))) /
         normal_.y);
    const f32 drop = query.origin.y - height;
    if (drop < 0.0F || drop > query.length) {
        return hit;
    }
    hit.hit = true;
    hit.point = Vec3{query.origin.x, height, query.origin.z};
    hit.normal = normal_.y > 0.0F ? normal_ : normal_ * -1.0F;
    return hit;
}

Status solve_foot_placement(const Skeleton& skeleton, const FootPlacementSetup& setup,
                            const FootPlacementContext& context, const GroundProbe& probe,
                            f32 weight, FootPlacementState& state, Span<Transform> local,
                            FootPlacementReport* report) noexcept {
    if (Status checked = check_setup(skeleton, setup, local); !checked) {
        return checked;
    }
    FootPlacementReport scratch;
    FootPlacementReport& out = report != nullptr ? *report : scratch;
    out = FootPlacementReport{};
    const f32 blend = math::clamp(weight, 0.0F, 1.0F);
    if (blend <= 0.0F) {
        // OFF IS THE POSE BEFORE, bit for bit; the follower starts again from the ground when the
        // solve is faded back in.
        state.primed = false;
        return ok();
    }

    const Transform animated[2] = {joint_model(skeleton, setup.legs[0].tip, local),
                                   joint_model(skeleton, setup.legs[1].tip, local)};
    const Vec3 ankles[2] = {animated[0].translation, animated[1].translation};
    const Targets targets = probe_targets(setup, context, probe, ankles, out);
    follow(state, targets, follow_alpha(state, setup.smoothing, context.elapsed));

    raise_pelvis(skeleton, setup.pelvis, state.pelvis * blend, local);
    for (u32 leg = 0; leg < 2; ++leg) {
        const Vec3 target = ankles[leg] + Vec3{0.0F, state.offset[leg] * blend, 0.0F};
        Expected<bool, Error> solved =
            solve_two_bone(skeleton, setup.legs[leg], target, 1.0F, local);
        if (!solved) {
            return Status{make_unexpected(solved.error())};
        }
        out.reached[leg] = *solved;
        tilt_foot(skeleton, setup.legs[leg].tip, normalize(animated[leg].rotation),
                  state.normal[leg], blend, local);
        out.offset[leg] = state.offset[leg] * blend;
    }
    out.pelvis = state.pelvis * blend;
    out.solved = true;
    return ok();
}

}  // namespace cy::animation
