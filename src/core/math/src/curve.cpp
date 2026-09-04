// Curve, Curve2D, Curve3D and Gradient. Task 3.1.5. See include/cy/core/math/curve.h.

#include <cy/core/math/curve.h>

#include <cy/core/base/assert.h>

#include <algorithm>
#include <cmath>
#include <functional>

namespace cy {
namespace {

/// Cubic Hermite basis on [0, 1]. `m0` and `m1` are tangents already scaled by the segment's
/// duration, which is what makes the basis itself independent of the time scale.
[[nodiscard]] f32 hermite(f32 p0, f32 m0, f32 p1, f32 m1, f32 t) noexcept {
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    return (((2.0f * t3) - (3.0f * t2) + 1.0f) * p0) + ((t3 - (2.0f * t2) + t) * m0) +
           (((-2.0f * t3) + (3.0f * t2)) * p1) + ((t3 - t2) * m1);
}

/// The derivative of `hermite` with respect to its parameter.
[[nodiscard]] f32 hermite_derivative(f32 p0, f32 m0, f32 p1, f32 m1, f32 t) noexcept {
    const f32 t2 = t * t;
    return (((6.0f * t2) - (6.0f * t)) * p0) + (((3.0f * t2) - (4.0f * t) + 1.0f) * m0) +
           (((-6.0f * t2) + (6.0f * t)) * p1) + (((3.0f * t2) - (2.0f * t)) * m1);
}

/// The finite-difference step used for `Curve3D::tangent`, in spline-parameter units.
///
/// The centripetal Catmull-Rom evaluator is a Barry-Goldman pyramid rather than a polynomial in a
/// single variable, so its analytic derivative is a page of algebra for a quantity every caller
/// normalises anyway. A central difference is two evaluations and is accurate to O(h²), which is
/// far below what the direction is used for.
constexpr f32 kTangentStep = 1e-3f;

/// The exponent that makes a Catmull-Rom spline *centripetal*. 0.5 is the value that removes cusps
/// and self-intersections near closely spaced control points; 0 gives the uniform spline that has
/// them, and 1 the chordal one that overshoots.
constexpr f32 kCentripetalAlpha = 0.5f;

}  // namespace

// --- Curve
// ------------------------------------------------------------------------------------------

usize Curve::add_key(const CurveKey& key) {
    const auto position =
        std::ranges::upper_bound(keys_, key.time, std::ranges::less{}, &CurveKey::time);
    const usize index = static_cast<usize>(position - keys_.begin());
    keys_.insert(position, key);
    return index;
}

void Curve::set_keys(const CurveKey* keys, usize count) {
    keys_.assign(keys, keys + count);
    std::ranges::stable_sort(
        keys_, [](const CurveKey& a, const CurveKey& b) noexcept { return a.time < b.time; });
}

bool Curve::wrap_time(f32& time) const noexcept {
    if (keys_.size() < 2) {
        return false;
    }
    const f32 first = keys_.front().time;
    const f32 last = keys_.back().time;
    const f32 span = last - first;
    if (span <= 0.0f) {
        return false;
    }
    const CurveExtrapolation mode = time < first ? before_ : after_;
    if (time >= first && time <= last) {
        return true;
    }
    switch (mode) {
        case CurveExtrapolation::Clamp:
        case CurveExtrapolation::Linear:
            // Both are handled by the caller: Clamp holds the boundary value and Linear continues
            // along the boundary tangent, and neither is a remapping of the time.
            return false;
        case CurveExtrapolation::Repeat: {
            const f32 offset = std::fmod(time - first, span);
            time = first + (offset < 0.0f ? offset + span : offset);
            return true;
        }
        case CurveExtrapolation::PingPong: {
            const f32 doubled = span * 2.0f;
            f32 offset = std::fmod(time - first, doubled);
            if (offset < 0.0f) {
                offset += doubled;
            }
            time = first + (offset <= span ? offset : doubled - offset);
            return true;
        }
    }
    return false;
}

f32 Curve::evaluate(f32 time) const noexcept {
    if (keys_.empty()) {
        return 0.0f;
    }
    if (keys_.size() == 1) {
        return keys_.front().value;
    }

    f32 sample_time = time;
    if (!wrap_time(sample_time)) {
        // Outside the key range under Clamp or Linear.
        if (sample_time < keys_.front().time) {
            const CurveKey& key = keys_.front();
            return before_ == CurveExtrapolation::Linear
                       ? key.value + (key.in_tangent * (sample_time - key.time))
                       : key.value;
        }
        if (sample_time > keys_.back().time) {
            const CurveKey& key = keys_.back();
            return after_ == CurveExtrapolation::Linear
                       ? key.value + (key.out_tangent * (sample_time - key.time))
                       : key.value;
        }
    }

    const auto upper =
        std::ranges::upper_bound(keys_, sample_time, std::ranges::less{}, &CurveKey::time);
    if (upper == keys_.begin()) {
        return keys_.front().value;
    }
    if (upper == keys_.end()) {
        return keys_.back().value;
    }
    const CurveKey& a = *(upper - 1);
    const CurveKey& b = *upper;
    const f32 duration = b.time - a.time;
    if (duration <= 0.0f) {
        // Two keys at the same time: a step. The later key wins, which is what "the value changes
        // here" means.
        return b.value;
    }
    const f32 local = (sample_time - a.time) / duration;
    return hermite(a.value, a.out_tangent * duration, b.value, b.in_tangent * duration, local);
}

f32 Curve::derivative(f32 time) const noexcept {
    if (keys_.size() < 2) {
        return 0.0f;
    }
    f32 sample_time = time;
    if (!wrap_time(sample_time)) {
        if (sample_time < keys_.front().time) {
            return before_ == CurveExtrapolation::Linear ? keys_.front().in_tangent : 0.0f;
        }
        if (sample_time > keys_.back().time) {
            return after_ == CurveExtrapolation::Linear ? keys_.back().out_tangent : 0.0f;
        }
    }
    auto upper = std::ranges::upper_bound(keys_, sample_time, std::ranges::less{}, &CurveKey::time);
    if (upper == keys_.begin()) {
        return 0.0f;
    }
    if (upper == keys_.end()) {
        // Exactly on the last key. `upper_bound` finds nothing beyond it, so the segment that ends
        // there is the one to differentiate — at its far end. Without this the derivative drops to
        // zero at the final key while the value stays correct, which reads as the curve suddenly
        // stopping and is wrong for anything driving a velocity from a track.
        --upper;
    }
    const CurveKey& a = *(upper - 1);
    const CurveKey& b = *upper;
    const f32 duration = b.time - a.time;
    if (duration <= 0.0f) {
        return 0.0f;
    }
    const f32 local = (sample_time - a.time) / duration;
    // The chain rule: the basis is parameterised on [0, 1], so its derivative is per unit of
    // `local` and has to be divided by the segment's duration to be per unit of time.
    return hermite_derivative(a.value, a.out_tangent * duration, b.value, b.in_tangent * duration,
                              local) /
           duration;
}

// --- Curve3D
// ----------------------------------------------------------------------------------------

void Curve3D::add_point(Vec3 point) {
    points_.push_back(point);
    // Any change invalidates the table. Clearing it rather than trying to patch it is what makes
    // `is_baked()` an honest answer.
    arc_lengths_.clear();
    arc_parameters_.clear();
}

void Curve3D::set_points(const Vec3* points, usize count) {
    points_.assign(points, points + count);
    arc_lengths_.clear();
    arc_parameters_.clear();
}

void Curve3D::clear() noexcept {
    points_.clear();
    arc_lengths_.clear();
    arc_parameters_.clear();
}

void Curve3D::segment_for(f32 t, usize& segment, f32& local) const noexcept {
    const usize segment_count = points_.size() - 1;
    const f32 scaled = math::saturate(t) * static_cast<f32>(segment_count);
    f32 index = std::floor(scaled);
    if (index >= static_cast<f32>(segment_count)) {
        index = static_cast<f32>(segment_count - 1);
    }
    segment = static_cast<usize>(index);
    local = scaled - index;
}

Vec3 Curve3D::sample(f32 t) const noexcept {
    if (points_.empty()) {
        return Vec3{};
    }
    if (points_.size() == 1) {
        return points_[0];
    }
    if (points_.size() == 2) {
        return lerp(points_[0], points_[1], math::saturate(t));
    }

    usize segment = 0;
    f32 local = 0.0f;
    segment_for(t, segment, local);

    // The four control points of this segment, with the ends duplicated so that the first and last
    // segments are defined without a phantom point outside the curve.
    const usize last = points_.size() - 1;
    const Vec3 p0 = points_[segment > 0 ? segment - 1 : 0];
    const Vec3 p1 = points_[segment];
    const Vec3 p2 = points_[segment + 1];
    const Vec3 p3 = points_[segment + 2 <= last ? segment + 2 : last];

    // Barry-Goldman: three nested linear interpolations over a non-uniform knot vector. The knot
    // spacing is the chord length raised to alpha, which is what "centripetal" means.
    const auto knot = [](f32 previous, Vec3 a, Vec3 b) noexcept {
        return previous + std::pow(math::max(distance(a, b), 1e-6f), kCentripetalAlpha);
    };
    const f32 t0 = 0.0f;
    const f32 t1 = knot(t0, p0, p1);
    const f32 t2 = knot(t1, p1, p2);
    const f32 t3 = knot(t2, p2, p3);
    const f32 t_at = math::lerp(t1, t2, local);

    const Vec3 a1 = p0 * ((t1 - t_at) / (t1 - t0)) + p1 * ((t_at - t0) / (t1 - t0));
    const Vec3 a2 = p1 * ((t2 - t_at) / (t2 - t1)) + p2 * ((t_at - t1) / (t2 - t1));
    const Vec3 a3 = p2 * ((t3 - t_at) / (t3 - t2)) + p3 * ((t_at - t2) / (t3 - t2));
    const Vec3 b1 = a1 * ((t2 - t_at) / (t2 - t0)) + a2 * ((t_at - t0) / (t2 - t0));
    const Vec3 b2 = a2 * ((t3 - t_at) / (t3 - t1)) + a3 * ((t_at - t1) / (t3 - t1));
    return b1 * ((t2 - t_at) / (t2 - t1)) + b2 * ((t_at - t1) / (t2 - t1));
}

Vec3 Curve3D::tangent(f32 t) const noexcept {
    if (points_.size() < 2) {
        return Vec3{};
    }
    const f32 low = math::max(0.0f, t - kTangentStep);
    const f32 high = math::min(1.0f, t + kTangentStep);
    if (high <= low) {
        return Vec3{};
    }
    return (sample(high) - sample(low)) / (high - low);
}

Expected<void, Error> Curve3D::bake(u32 samples_per_segment) {
    arc_lengths_.clear();
    arc_parameters_.clear();
    if (points_.size() < 2) {
        return cy::fail(ErrorCode::InvalidArgument, "Curve3D::bake(): a curve needs two points");
    }
    if (samples_per_segment == 0) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "Curve3D::bake(): samples_per_segment must be positive");
    }

    const usize segments = points_.size() - 1;
    const usize sample_count = (segments * samples_per_segment) + 1;
    arc_lengths_.reserve(sample_count);
    arc_parameters_.reserve(sample_count);

    // Piecewise-linear approximation of the arc length: the sum of chord lengths between samples.
    // It underestimates by O(h²) per chord, which at sixteen samples a segment is far below the
    // precision a follower can perceive.
    f32 accumulated = 0.0f;
    Vec3 previous = sample(0.0f);
    arc_lengths_.push_back(0.0f);
    arc_parameters_.push_back(0.0f);
    for (usize i = 1; i < sample_count; ++i) {
        const f32 parameter = static_cast<f32>(i) / static_cast<f32>(sample_count - 1);
        const Vec3 current = sample(parameter);
        accumulated += distance(previous, current);
        arc_lengths_.push_back(accumulated);
        arc_parameters_.push_back(parameter);
        previous = current;
    }
    return {};
}

Expected<f32, Error> Curve3D::length() const noexcept {
    if (!is_baked()) {
        return cy::fail(ErrorCode::Unavailable, "Curve3D::length(): bake() has not been called");
    }
    return arc_lengths_.back();
}

Expected<f32, Error> Curve3D::parameter_at_distance(f32 distance_along) const noexcept {
    if (!is_baked()) {
        return cy::fail(ErrorCode::Unavailable,
                        "Curve3D::parameter_at_distance(): bake() has not been called");
    }
    const f32 total = arc_lengths_.back();
    if (distance_along <= 0.0f) {
        return 0.0f;
    }
    if (distance_along >= total) {
        return 1.0f;
    }
    // Binary search over the cumulative table, then one linear interpolation inside the interval.
    // This is the O(1)-per-sample the specification's constant-speed scenario asks for; the
    // alternative is integrating the speed every frame, which is both slower and drift-prone.
    const auto upper = std::ranges::upper_bound(arc_lengths_, distance_along);
    const usize index = static_cast<usize>(upper - arc_lengths_.begin());
    const f32 low_length = arc_lengths_[index - 1];
    const f32 high_length = arc_lengths_[index];
    const f32 span = high_length - low_length;
    const f32 fraction = span > 0.0f ? (distance_along - low_length) / span : 0.0f;
    return math::lerp(arc_parameters_[index - 1], arc_parameters_[index], fraction);
}

Expected<Vec3, Error> Curve3D::sample_by_distance(f32 distance_along) const noexcept {
    const Expected<f32, Error> parameter = parameter_at_distance(distance_along);
    if (!parameter) {
        return cy::make_unexpected(parameter.error());
    }
    return sample(*parameter);
}

// --- Curve2D
// ----------------------------------------------------------------------------------------

void Curve2D::add_point(Vec2 point) {
    curve_.add_point(Vec3{point.x, point.y, 0.0f});
}

void Curve2D::set_points(const Vec2* points, usize count) {
    curve_.clear();
    for (usize i = 0; i < count; ++i) {
        curve_.add_point(Vec3{points[i].x, points[i].y, 0.0f});
    }
}

void Curve2D::clear() noexcept {
    curve_.clear();
}

Vec2 Curve2D::point(usize index) const noexcept {
    return curve_.point(index).xy();
}
Vec2 Curve2D::sample(f32 t) const noexcept {
    return curve_.sample(t).xy();
}
Vec2 Curve2D::tangent(f32 t) const noexcept {
    return curve_.tangent(t).xy();
}

Expected<Vec2, Error> Curve2D::sample_by_distance(f32 distance_along) const noexcept {
    const Expected<Vec3, Error> point3 = curve_.sample_by_distance(distance_along);
    if (!point3) {
        return cy::make_unexpected(point3.error());
    }
    return point3->xy();
}

// --- Gradient
// ---------------------------------------------------------------------------------------

void Gradient::add_color_key(f32 position, const Color& color) {
    const ColorKey key{math::saturate(position), color};
    const auto at = std::ranges::upper_bound(color_keys_, key.position, std::ranges::less{},
                                             &ColorKey::position);
    color_keys_.insert(at, key);
}

void Gradient::add_alpha_key(f32 position, f32 alpha) {
    const AlphaKey key{math::saturate(position), alpha};
    const auto at = std::ranges::upper_bound(alpha_keys_, key.position, std::ranges::less{},
                                             &AlphaKey::position);
    alpha_keys_.insert(at, key);
}

void Gradient::clear() noexcept {
    color_keys_.clear();
    alpha_keys_.clear();
}

Color Gradient::evaluate(f32 position) const noexcept {
    const f32 at = math::saturate(position);

    Color result = colors::kWhite;
    if (!color_keys_.empty()) {
        if (at <= color_keys_.front().position) {
            result = color_keys_.front().color;
        } else if (at >= color_keys_.back().position) {
            result = color_keys_.back().color;
        } else {
            const auto upper =
                std::ranges::upper_bound(color_keys_, at, std::ranges::less{}, &ColorKey::position);
            const ColorKey& low = *(upper - 1);
            const ColorKey& high = *upper;
            const f32 span = high.position - low.position;
            result =
                span > 0.0f ? lerp(low.color, high.color, (at - low.position) / span) : high.color;
        }
    }

    if (!alpha_keys_.empty()) {
        f32 alpha = 1.0f;
        if (at <= alpha_keys_.front().position) {
            alpha = alpha_keys_.front().alpha;
        } else if (at >= alpha_keys_.back().position) {
            alpha = alpha_keys_.back().alpha;
        } else {
            const auto upper =
                std::ranges::upper_bound(alpha_keys_, at, std::ranges::less{}, &AlphaKey::position);
            const AlphaKey& low = *(upper - 1);
            const AlphaKey& high = *upper;
            const f32 span = high.position - low.position;
            alpha = span > 0.0f ? math::lerp(low.alpha, high.alpha, (at - low.position) / span)
                                : high.alpha;
        }
        result.a = alpha;
    }
    return result;
}

}  // namespace cy
