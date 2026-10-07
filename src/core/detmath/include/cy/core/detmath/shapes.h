// SPDX-License-Identifier: MIT
#pragma once
// The shapes the kinematic mover and the fixed-point navigation need. Design §6 and §8.
//
// Every test here decides on an EXACT quantity: squared distances and dot products in Q64.64, never
// a rounded length. Two peers therefore agree on "do these overlap" even for shapes that touch to
// the last bit, which is the case a rounded comparison would get wrong on one of them.
//
// Touching is not overlapping: a circle pair at exactly the sum of the radii does not overlap, so a
// separation step that pushed two units apart to exactly that distance leaves them at rest.
//
// RANGE. The Q64.64 squares hold a difference of up to 2^31.5 metres per axis, far beyond any map;
// beyond it a squared distance wraps (WideFixed arithmetic is modulo 2^128).

#include <cy/core/detmath/config.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/vec.h>

namespace cy::detmath::inline CY_DETMATH_VARIANT {

/// `numerator / denominator` clamped to [0, 1], truncated to 2^-32. Zero when `numerator <= 0` or
/// `denominator <= 0`; one when `numerator >= denominator`. Computed by a 32-step long division of
/// the exact values, so it is the same answer however large they are.
[[nodiscard]] Fixed ratio_unit(WideFixed numerator, WideFixed denominator) noexcept;

/// An axis-aligned box. `min` is at or below `max` on every axis.
struct FixedAabb {
    FixedVec3 min;
    FixedVec3 max;

    /// Whether `point` lies inside or on the boundary.
    [[nodiscard]] constexpr bool contains(FixedVec3 point) const noexcept {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }
    /// Whether the two boxes share interior. Boxes that only touch do not overlap.
    [[nodiscard]] constexpr bool overlaps(const FixedAabb& other) const noexcept {
        return min.x < other.max.x && other.min.x < max.x && min.y < other.max.y &&
               other.min.y < max.y && min.z < other.max.z && other.min.z < max.z;
    }

    friend constexpr bool operator==(const FixedAabb&, const FixedAabb&) = default;
};

/// A circle on the plane.
struct FixedCircle {
    FixedVec2 centre;
    Fixed radius;

    /// Whether `point` lies inside or on the circle.
    [[nodiscard]] bool contains(FixedVec2 point) const noexcept {
        return distance_squared(centre, point) <= WideFixed::product(radius, radius);
    }
    /// Whether the two circles share interior. Exactly touching circles do not.
    [[nodiscard]] bool overlaps(const FixedCircle& other) const noexcept {
        const Fixed reach = radius + other.radius;
        return distance_squared(centre, other.centre) < WideFixed::product(reach, reach);
    }

    friend constexpr bool operator==(const FixedCircle&, const FixedCircle&) = default;
};

/// The point of segment `a`-`b` nearest `point`. The parameter is `ratio_unit` of the exact
/// projection, truncated to 2^-32 toward `a`, so the answer is within |b - a| ulps of the exact
/// nearest point; a degenerate segment answers `a`.
[[nodiscard]] FixedVec2 closest_point_on_segment(FixedVec2 a, FixedVec2 b,
                                                 FixedVec2 point) noexcept;

/// A capsule on the plane: every point within `radius` of the segment `a`-`b`. A wall, a cliff
/// edge, or a unit longer than it is wide.
struct FixedCapsule2D {
    FixedVec2 a;
    FixedVec2 b;
    Fixed radius;

    /// The point of the core segment nearest `point`.
    [[nodiscard]] FixedVec2 closest_point(FixedVec2 point) const noexcept {
        return closest_point_on_segment(a, b, point);
    }
    /// Whether `point` lies inside or on the capsule.
    [[nodiscard]] bool contains(FixedVec2 point) const noexcept {
        return distance_squared(closest_point(point), point) <= WideFixed::product(radius, radius);
    }
    /// Whether the capsule and `circle` share interior.
    [[nodiscard]] bool overlaps(const FixedCircle& circle) const noexcept {
        const Fixed reach = radius + circle.radius;
        return distance_squared(closest_point(circle.centre), circle.centre) <
               WideFixed::product(reach, reach);
    }

    friend constexpr bool operator==(const FixedCapsule2D&, const FixedCapsule2D&) = default;
};

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
