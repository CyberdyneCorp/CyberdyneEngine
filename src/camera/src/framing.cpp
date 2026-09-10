// Framing and the constraints a rail camera is made of. M8.b task 7.3.

#include <cy/camera/framing.h>

#include <algorithm>
#include <cmath>

namespace cy::camera {
namespace {

[[nodiscard]] f32 clampf(f32 value, f32 low, f32 high) noexcept {
    return std::clamp(value, low, high);
}

/// Half the vertical and horizontal extent a sample occupies, from its bounds.
[[nodiscard]] Vec2 half_extent_of(const Aabb& bounds) noexcept {
    const Vec3 size = bounds.max - bounds.min;
    const f32 half_height = std::fabs(size.y) * 0.5F;
    const f32 half_width = std::fabs(size.x) * 0.5F;
    return Vec2{(half_width > 0.0F) ? half_width : 0.5F, (half_height > 0.0F) ? half_height : 0.5F};
}

}  // namespace

Status resolve_targets(Span<const TargetBinding> bindings, TargetResolver* resolver,
                       Array<TargetSample>& out) noexcept {
    out.clear();
    for (const TargetBinding& binding : bindings) {
        TargetSample sample;
        sample.stable_id = binding.stable_id;
        switch (binding.kind) {
            case TargetKind::Position:
            case TargetKind::Bounds:
                // Answered from the binding. The resolver is never asked a question whose answer is
                // already in the question.
                sample.transform = Transform::from_translation(binding.position);
                sample.bounds = binding.bounds;
                sample.valid = true;
                break;
            case TargetKind::None:
                break;
            default:
                if (resolver != nullptr) {
                    resolver->resolve(binding, sample);
                }
                break;
        }
        if (Status pushed = out.push_back(sample); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Vec3 rebase(const world::PartitionConfig& partition, const world::WorldPosition& position,
            world::CellCoord frame) noexcept {
    return world::to_simulation_local(partition, position, frame);
}

Status solve_framing(const FramingRequest& request, FramingSolution& out) noexcept {
    if (request.bindings.size() != request.samples.size()) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "framing: a sample per binding is required", 0});
    }
    out = FramingSolution{};
    out.pose = request.current;
    out.anchor = request.current.translation;

    const f32 fov = clampf(request.lens.vertical_fov_radians(), 0.05F, 3.0F);
    const f32 tan_half_y = std::tan(fov * 0.5F);
    const f32 tan_half_x = tan_half_y * ((request.aspect > 0.0F) ? request.aspect : 1.0F);

    Vec3 centroid{0.0F, 0.0F, 0.0F};
    f32 total_weight = 0.0F;
    Vec3 lo{0.0F, 0.0F, 0.0F};
    Vec3 hi{0.0F, 0.0F, 0.0F};
    f32 nearest_permitted = request.maximum_distance;
    bool first = true;
    for (usize index = 0; index < request.bindings.size(); ++index) {
        const TargetSample& sample = request.samples[index];
        if (!sample.valid) {
            continue;
        }
        const TargetBinding& binding = request.bindings[index];
        const Vec3 point = sample.transform.translation;
        const f32 weight = (binding.weight > 0.0F) ? binding.weight : 0.0F;
        centroid = centroid + (point * weight);
        total_weight += weight;

        const Vec2 half = half_extent_of(sample.bounds);
        const Vec3 extent{half.x, half.y, half.x};
        const Vec3 point_lo = point - extent;
        const Vec3 point_hi = point + extent;
        if (first) {
            lo = point_lo;
            hi = point_hi;
            first = false;
        } else {
            lo = Vec3{(point_lo.x < lo.x) ? point_lo.x : lo.x,
                      (point_lo.y < lo.y) ? point_lo.y : lo.y,
                      (point_lo.z < lo.z) ? point_lo.z : lo.z};
            hi = Vec3{(point_hi.x > hi.x) ? point_hi.x : hi.x,
                      (point_hi.y > hi.y) ? point_hi.y : hi.y,
                      (point_hi.z > hi.z) ? point_hi.z : hi.z};
        }

        // A subject of half-height h subtends a fraction s of the view at distance d when
        // h / (d · tan(fov/2)) = s. A MINIMUM screen size is therefore a MAXIMUM distance.
        if (binding.min_screen_size > 0.0F) {
            nearest_permitted =
                std::min(nearest_permitted, half.y / (binding.min_screen_size * tan_half_y));
        }
        ++out.contributors;
    }

    if (out.contributors == 0) {
        return ok();
    }
    centroid = (total_weight > 0.0F) ? (centroid * (1.0F / total_weight)) : ((lo + hi) * 0.5F);

    // THE FIT: how far back the whole set has to be to sit inside the view with the composition's
    // padding around it. This is the bound that grows when two combatants separate.
    const Vec3 size = hi - lo;
    const f32 pad = 1.0F + clampf(request.composition.padding, 0.0F, 2.0F);
    const f32 fit_vertical = (size.y * 0.5F * pad) / tan_half_y;
    const f32 fit_horizontal = (size.x * 0.5F * pad) / tan_half_x;
    f32 distance = std::max({fit_vertical, fit_horizontal, request.minimum_distance});
    if (distance > nearest_permitted) {
        // The two bounds conflict: framing everything makes something smaller than it asked to be.
        // KEEPING EVERY TARGET VISIBLE WINS — a framing that drops a target has failed at the thing
        // it is for — and the yield is reported rather than swallowed.
        if (nearest_permitted >= request.minimum_distance) {
            out.screen_size_yielded = true;
        }
    }
    distance = clampf(distance, request.minimum_distance, request.maximum_distance);
    out.distance = distance;
    out.anchor = centroid;

    const Vec3 camera = request.current.translation;
    const Vec3 to_anchor = centroid - camera;
    const Vec3 forward = request.current.rotation * Vec3{0.0F, 0.0F, -1.0F};

    // THE DEAD ZONE. Movement inside it produces no camera movement at all — not damped movement,
    // none — which is what stops a camera drifting around a standing character.
    const f32 along = dot(to_anchor, forward);
    if (along > 1e-3F) {
        const Vec3 right =
            normalized_or(cross(forward, Vec3{0.0F, 1.0F, 0.0F}), Vec3{1.0F, 0.0F, 0.0F});
        const Vec3 up = cross(right, forward);
        const f32 screen_x = dot(to_anchor, right) / (along * tan_half_x);
        const f32 screen_y = dot(to_anchor, up) / (along * tan_half_y);
        if (std::fabs(screen_x - request.composition.screen_offset.x) <=
                request.composition.dead_zone.x &&
            std::fabs(screen_y - request.composition.screen_offset.y) <=
                request.composition.dead_zone.y) {
            out.inside_dead_zone = true;
            out.pose = request.current;
            return ok();
        }
    }

    const Vec3 direction = normalized_or(to_anchor, forward);
    Vec3 position = centroid - (direction * distance);
    // THE COMPOSITION OFFSET: place the subject at a screen fraction by sliding the camera the
    // other way, which keeps its distance rather than rotating away from it.
    if (request.composition.screen_offset.x != 0.0F ||
        request.composition.screen_offset.y != 0.0F) {
        const Vec3 right =
            normalized_or(cross(direction, Vec3{0.0F, 1.0F, 0.0F}), Vec3{1.0F, 0.0F, 0.0F});
        const Vec3 up = cross(right, direction);
        position =
            position - (right * (request.composition.screen_offset.x * distance * tan_half_x));
        position = position - (up * (request.composition.screen_offset.y * distance * tan_half_y));
    }

    out.pose.translation = position;
    const Vec3 look = normalized_or(centroid - position, direction);
    out.pose.rotation = Quat::look_rotation(look, Vec3{0.0F, 1.0F, 0.0F});
    if (request.composition.level_horizon) {
        const Vec3 euler = out.pose.rotation.to_euler_yxz();
        out.pose.rotation = Quat::from_euler_yxz(Vec3{euler.x, euler.y, 0.0F});
    }
    return ok();
}

Vec3 apply_constraints(Span<const Constraint> constraints, Vec3 desired, Vec3 anchor) noexcept {
    Vec3 result = desired;
    for (const Constraint& constraint : constraints) {
        switch (constraint.kind) {
            case ConstraintKind::Plane: {
                const Vec3 normal = normalized_or(constraint.plane_normal, Vec3{0.0F, 0.0F, 1.0F});
                result = result - (normal * (dot(result, normal) - constraint.plane_offset));
                break;
            }
            case ConstraintKind::Orbit: {
                const Vec3 delta = result - anchor;
                const f32 radius = length(delta);
                if (radius <= 1e-4F) {
                    break;
                }
                const f32 yaw = clampf(std::atan2(delta.x, delta.z), constraint.yaw_range.x,
                                       constraint.yaw_range.y);
                const f32 pitch = clampf(std::asin(clampf(delta.y / radius, -1.0F, 1.0F)),
                                         constraint.pitch_range.x, constraint.pitch_range.y);
                const f32 horizontal = radius * std::cos(pitch);
                result = anchor + Vec3{horizontal * std::sin(yaw), radius * std::sin(pitch),
                                       horizontal * std::cos(yaw)};
                break;
            }
            case ConstraintKind::Spline: {
                if (constraint.spline.size() < 2) {
                    break;
                }
                Vec3 best = constraint.spline[0];
                f32 best_distance = length_squared(result - best);
                for (usize index = 0; index + 1 < constraint.spline.size(); ++index) {
                    const Vec3 a = constraint.spline[index];
                    const Vec3 b = constraint.spline[index + 1];
                    const Vec3 segment = b - a;
                    const f32 length_sq = length_squared(segment);
                    f32 t = 0.0F;
                    if (length_sq > 1e-8F) {
                        t = clampf(dot(result - a, segment) / length_sq, 0.0F, 1.0F);
                    }
                    const Vec3 point = a + (segment * t);
                    const f32 candidate = length_squared(result - point);
                    if (candidate < best_distance) {
                        best_distance = candidate;
                        best = point;
                    }
                }
                result = best;
                break;
            }
            case ConstraintKind::Count:
                break;
        }
    }
    return result;
}

}  // namespace cy::camera
