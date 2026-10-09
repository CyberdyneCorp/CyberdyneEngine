// Local avoidance and the crowd. See cy/navigation/crowd.h for the argument. The solver and its
// grid are written once, over a scalar policy, in cy/navigation/crowd_solver_impl.h; this file is
// the f32 policy, the f32 instantiation, and the float-only conveniences around it.

#include <cy/core/base/assert.h>
#include <cy/navigation/crowd.h>
#include <cy/navigation/crowd_solver_impl.h>
#include <cy/navigation/follow.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cy::navigation {

// --- FloatCrowdPolicy: the solver's f32 arithmetic, spelled as it always was --------------------

Vec3 FloatCrowdPolicy::flatten(Vec3 v) noexcept {
    return Vec3{v.x, 0.0F, v.z};
}

f32 FloatCrowdPolicy::length_squared(Vec3 v) noexcept {
    return (v.x * v.x) + (v.z * v.z);
}

f32 FloatCrowdPolicy::dot(Vec3 a, Vec3 b) noexcept {
    return (a.x * b.x) + (a.z * b.z);
}

f32 FloatCrowdPolicy::sqrt(f32 w) noexcept {
    return std::sqrt(w);
}

f32 FloatCrowdPolicy::length(Vec3 v) noexcept {
    return cy::length(v);
}

Vec3 FloatCrowdPolicy::clamp_speed(Vec3 v, f32 max_speed) noexcept {
    const f32 speed_squared = (v.x * v.x) + (v.z * v.z);
    if (speed_squared <= max_speed * max_speed || speed_squared < 1e-12F) {
        return v;
    }
    const f32 scale = max_speed / std::sqrt(speed_squared);
    return Vec3{v.x * scale, v.y, v.z * scale};
}

Vec3 FloatCrowdPolicy::perpendicular(Vec3 v) noexcept {
    return Vec3{v.z, 0.0F, -v.x};
}

Vec3 FloatCrowdPolicy::lattice(u32 spoke, u32 count, f32 magnitude) noexcept {
    const f32 angle =
        (2.0F * std::numbers::pi_v<f32> * static_cast<f32>(spoke)) / static_cast<f32>(count);
    return Vec3{std::cos(angle) * magnitude, 0.0F, std::sin(angle) * magnitude};
}

f32 FloatCrowdPolicy::ratio(u32 numerator, u32 denominator) noexcept {
    return static_cast<f32>(numerator) / static_cast<f32>(denominator);
}

f32 FloatCrowdPolicy::lesser(f32 a, f32 b) noexcept {
    return std::fmin(a, b);
}

f32 FloatCrowdPolicy::greater(f32 a, f32 b) noexcept {
    return std::fmax(a, b);
}

i32 FloatCrowdPolicy::cell(f32 coordinate, f32 size) noexcept {
    return static_cast<i32>(std::floor(coordinate / size));
}

i32 FloatCrowdPolicy::cell_span(f32 range, f32 size) noexcept {
    return static_cast<i32>(std::ceil(range / size));
}

/// How much of the avoidance this agent takes. Half between equals, tilted by priority so the
/// lower-priority agent yields, and never 0 or 1 — see the header.
f32 FloatCrowdPolicy::responsibility(u8 mine, u8 theirs) noexcept {
    if (mine == theirs) {
        return 0.5F;
    }
    return (mine < theirs) ? 0.85F : 0.15F;
}

f32 FloatCrowdPolicy::infinity() noexcept {
    return math::kInfinity;
}

template class BasicCrowd<FloatCrowdPolicy>;

const char* crowd_tier_name(CrowdTier tier) noexcept {
    switch (tier) {
        case CrowdTier::Full:
            return "Full";
        case CrowdTier::Reduced:
            return "Reduced";
        case CrowdTier::Minimal:
            return "Minimal";
        case CrowdTier::Count:
            break;
    }
    return "unknown";
}

void Crowd::steer_towards(CrowdAgentId id, Vec3 target, f32 arrival_distance) noexcept {
    CrowdAgent* subject = agent(id);
    if (subject == nullptr) {
        return;
    }
    const Vec3 offset = FloatCrowdPolicy::flatten(target - subject->position);
    const f32 distance = length(offset);
    if (distance <= arrival_distance || distance < 1e-5F) {
        subject->desired_velocity = Vec3{};
        return;
    }
    // Slow into the arrival radius rather than stopping dead on the boundary.
    const f32 speed = std::fmin(subject->params.max_speed,
                                subject->params.max_speed * (distance / (arrival_distance * 4.0F)));
    subject->desired_velocity = offset * (std::fmax(speed, 0.05F) / distance);
}

namespace {

/// `follow_points`' float instantiation: the XZ offset, its length against the arrival distance,
/// and full speed along it.
struct FloatFollowPolicy {
    using Vec = Vec3;
    using Scalar = f32;

    [[nodiscard]] static Vec3 position(const PathPoint& point) noexcept { return point.position; }
    [[nodiscard]] static Vec3 offset(Vec3 to, Vec3 from) noexcept {
        return Vec3{to.x - from.x, 0.0F, to.z - from.z};
    }
    [[nodiscard]] static bool beyond(Vec3 offset, f32 arrival) noexcept {
        return length(offset) > arrival;
    }
    [[nodiscard]] static Vec3 toward(Vec3 offset, f32 speed) noexcept {
        return offset * (speed / std::fmax(length(offset), 1e-5F));
    }
};

}  // namespace

Vec3 follow_path(Span<const PathPoint> path, Vec3 position, f32 speed, f32 arrival_distance,
                 u32& cursor) noexcept {
    return follow_points(FloatFollowPolicy{}, path, position, speed, arrival_distance, cursor);
}

Vec3 follow_field(const FlowField& field, Vec3 position, f32 speed) noexcept {
    const Vec3 direction = field.direction_at(position);
    return direction * speed;
}

}  // namespace cy::navigation
