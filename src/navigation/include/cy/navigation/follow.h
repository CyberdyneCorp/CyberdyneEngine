#pragma once
// Path following, written once for both kinds of navigation arithmetic. openspec/changes/
// add-deterministic-math, navigation delta: "Path following, local avoidance, and crowd steering
// SHALL be written once and instantiated for both kinds of arithmetic, rather than maintained as
// two diverging implementations."
//
// The algorithm is the loop: skip every point the agent is already within the arrival distance of,
// in order, and head for the first one it is not. What differs between the arithmetics is only what
// "within" and "head for" compute, and that is the policy's:
//
//   policy.position(point)        the point's position, in the policy's vector type
//   policy.offset(to, from)       the vector from `from` to `to` on the walking plane
//   policy.beyond(offset, arrival)  whether `offset` is longer than `arrival`
//   policy.toward(offset, speed)  the velocity heading along `offset`
//
// `cy::navigation::follow_path` instantiates it over `f32` and `PathPoint` (crowd.cpp), and
// `cy::movement::follow_path` over `Fixed` and the converted world's points (src/movement/), where
// "beyond" is decided on exact squares.
//
// Header-only and arithmetic-free: it names no float and no fixed-point type, so including it
// commits a module to neither.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::navigation {

template <class Policy, class Point>
[[nodiscard]] auto follow_points(const Policy& policy, Span<const Point> path,
                                 typename Policy::Vec position, typename Policy::Scalar speed,
                                 typename Policy::Scalar arrival, u32& cursor) noexcept
    -> Policy::Vec {
    while (cursor < path.size()) {
        const typename Policy::Vec offset = policy.offset(policy.position(path[cursor]), position);
        if (policy.beyond(offset, arrival)) {
            return policy.toward(offset, speed);
        }
        ++cursor;
    }
    return typename Policy::Vec{};
}

}  // namespace cy::navigation
