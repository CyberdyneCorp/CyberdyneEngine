// SPDX-License-Identifier: MIT
// `FixedCrowd`: navigation's crowd solver instantiated over `Fixed`. See cy/movement/crowd.h.
// Integer arithmetic only: every operation below is a detmath one.

#include <cy/core/detmath/functions.h>
#include <cy/core/memory/hash.h>
#include <cy/movement/crowd.h>
#include <cy/movement/mover.h>
#include <cy/navigation/crowd_solver_impl.h>

#include <initializer_list>

namespace cy::navigation {
template class BasicCrowd<movement::FixedCrowdPolicy>;
}  // namespace cy::navigation

namespace cy::movement {

WideFixed FixedCrowdPolicy::length_squared(FixedVec2 v) noexcept {
    return detmath::length_squared(v);
}

WideFixed FixedCrowdPolicy::dot(FixedVec2 a, FixedVec2 b) noexcept {
    return detmath::dot(a, b);
}

WideFixed FixedCrowdPolicy::square(Fixed s) noexcept {
    return WideFixed::product(s, s);
}

WideFixed FixedCrowdPolicy::product(WideFixed a, WideFixed b) noexcept {
    return WideFixed::product(a.saturating_narrow(), b.saturating_narrow());
}

Fixed FixedCrowdPolicy::sqrt(WideFixed w) noexcept {
    return detmath::sqrt(w);
}

Fixed FixedCrowdPolicy::length(FixedVec2 v) noexcept {
    return detmath::length(v);
}

FixedVec2 FixedCrowdPolicy::clamp_speed(FixedVec2 v, Fixed max_speed) noexcept {
    return detmath::clamp_length(v, max_speed);
}

FixedVec2 FixedCrowdPolicy::lattice(u32 spoke, u32 count, Fixed magnitude) noexcept {
    // spoke / count of a turn, truncated to the 2^-32 turn: exact for every power-of-two count.
    const auto turn = static_cast<u32>((static_cast<u64>(spoke) << 32U) / count);
    const detmath::SinCos direction = detmath::sincos(detmath::Angle::from_raw(turn));
    return FixedVec2{direction.cos * magnitude, direction.sin * magnitude};
}

Fixed FixedCrowdPolicy::ratio(u32 numerator, u32 denominator) noexcept {
    return Fixed::from_int(static_cast<i32>(numerator)) /
           Fixed::from_int(static_cast<i32>(denominator));
}

i32 FixedCrowdPolicy::cell(Fixed coordinate, Fixed size) noexcept {
    // floor(coordinate / size) of the raws, exactly: a quotient of two Q32.32 values is a ratio of
    // integers, and `/` on `Fixed` would truncate toward zero, putting -0.1 m in cell 0.
    const i64 quotient = coordinate.raw / size.raw;
    const bool below = (coordinate.raw % size.raw != 0) && ((coordinate.raw < 0) != (size.raw < 0));
    return static_cast<i32>(below ? quotient - 1 : quotient);
}

i32 FixedCrowdPolicy::cell_span(Fixed range, Fixed size) noexcept {
    // ceil(range / size) for a non-negative range and a positive size.
    const i64 quotient = range.raw / size.raw;
    return static_cast<i32>(range.raw % size.raw != 0 ? quotient + 1 : quotient);
}

Fixed FixedCrowdPolicy::responsibility(u8 mine, u8 theirs) noexcept {
    if (mine == theirs) {
        return Fixed::half();
    }
    return Fixed::from_raw(((mine < theirs) ? 85 : 15) * (Fixed::kOneRaw / 100));
}

u64 crowd_state_hash(const FixedCrowd& crowd) noexcept {
    u64 fold = hash_combine(0xC20'0D5'7A7EULL, crowd.capacity());
    for (const FixedCrowdAgent& agent : crowd.agents()) {
        fold = hash_combine(fold, agent.active ? 1U : 0U);
        for (const FixedVec2 v : {agent.position, agent.velocity, agent.desired_velocity}) {
            fold = hash_combine(fold, static_cast<u64>(v.x.raw));
            fold = hash_combine(fold, static_cast<u64>(v.y.raw));
        }
    }
    return fold;
}

Status avoid(KinematicMover& mover, FixedCrowd& crowd, const FixedAvoidanceParams& defaults,
             navigation::CrowdReport& report) noexcept {
    while (crowd.capacity() < mover.size()) {
        // `add` reuses the lowest free slot, so a crowd that has removed an agent would put the new
        // unit's agent at someone else's index.
        if (crowd.size() != crowd.capacity()) {
            return make_unexpected(Error{ErrorCode::InvalidArgument,
                                         "a unit was added to the mover after another stopped"});
        }
        const u32 unit = crowd.capacity();
        FixedAvoidanceParams params = defaults;
        params.radius = mover.radius(unit);
        params.max_speed = mover.max_speed(unit);
        if (auto added = crowd.add(mover.position(unit), params); !added) {
            return make_unexpected(added.error());
        }
    }
    for (u32 unit = 0; unit < mover.size(); ++unit) {
        FixedCrowdAgent* agent = crowd.agent(unit);
        if (agent == nullptr) {
            continue;
        }
        // A unit the mover stopped neither moves nor is avoided, as in the mover's own kernel.
        if (!mover.active(unit)) {
            if (Status removed = crowd.remove(unit); !removed) {
                return removed;
            }
            continue;
        }
        agent->position = mover.position(unit);
        agent->velocity = mover.velocity(unit);
        agent->desired_velocity = mover.desired_velocity(unit);
    }
    if (Status stepped = crowd.step(mover.dt(), report); !stepped) {
        return stepped;
    }
    for (u32 unit = 0; unit < mover.size(); ++unit) {
        if (const FixedCrowdAgent* agent = crowd.agent(unit); agent != nullptr) {
            mover.set_desired_velocity(unit, agent->velocity);
        }
    }
    return ok();
}

}  // namespace cy::movement
