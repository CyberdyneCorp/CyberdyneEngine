// SPDX-License-Identifier: MIT
// The lockstep adapter. See cy/game_backend/lockstep_backend.h.

#include <cy/abi/errors.h>
#include <cy/core/detmath/version.h>
#include <cy/game_backend/lockstep_backend.h>

namespace cy::game_backend {
namespace {

/// A session refusal, reported with its own sentence and mapped to the ABI's code.
[[nodiscard]] CyResult refuse(const Error& error) noexcept {
    return abi::report(error);
}

}  // namespace

CyResult LockstepAdapter::enlist(const CyLockstepUnitDesc& desc, u32& out_unit) noexcept {
    LockstepUnitSpec spec;
    spec.group = desc.group;
    spec.entity = desc.entity;
    spec.position = FixedVec2{Fixed::from_raw(desc.position.x), Fixed::from_raw(desc.position.y)};
    spec.radius = Fixed::from_raw(desc.radius);
    spec.max_speed = Fixed::from_raw(desc.max_speed);
    auto enlisted = issuer_.enlist(spec);
    if (!enlisted) {
        return refuse(enlisted.error());
    }
    if (follower_ != nullptr) {
        if (auto mirrored = follower_->enlist(spec); !mirrored) {
            return refuse(mirrored.error());
        }
    }
    out_unit = *enlisted;
    return CY_RESULT_OK;
}

CyResult LockstepAdapter::order(const CyLockstepOrder& order) noexcept {
    LockstepOrderPayload payload;
    payload.kind = order.kind;
    payload.group = order.group;
    payload.target_x = order.target.x;
    payload.target_z = order.target.y;
    if (Status recorded = issuer_.record(payload); !recorded) {
        return refuse(recorded.error());
    }
    return CY_RESULT_OK;
}

CyResult LockstepAdapter::unit(u32 index, CyLockstepUnit& out) const noexcept {
    auto state = issuer_.unit(index);
    if (!state) {
        return refuse(state.error());
    }
    out.group = state->group;
    out.entity = state->entity;
    out.position = CyFixedVec2{state->position.x.raw, state->position.y.raw};
    out.velocity = CyFixedVec2{state->velocity.x.raw, state->velocity.y.raw};
    out.height = state->height.raw;
    out.heading = state->heading.raw;
    out.flags = (state->moving ? CY_LOCKSTEP_UNIT_MOVING : 0U) |
                (state->just_arrived ? CY_LOCKSTEP_UNIT_ARRIVED : 0U);
    return CY_RESULT_OK;
}

CyResult LockstepAdapter::status(CyLockstepStatus& out) const noexcept {
    out.units = issuer_.units();
    out.tick = issuer_.tick();
    out.commands = issuer_.log().size();
    out.state_hash = issuer_.state_hash();
    out.digest = issuer_.digest();
    out.kernel_version = detmath::kKernelVersion;
    out.disagreements = disagreements_;
    return CY_RESULT_OK;
}

Status LockstepAdapter::tick(jobs::JobSystem* jobs) noexcept {
    if (Status advanced = issuer_.advance(jobs); !advanced) {
        return advanced;
    }
    if (follower_ == nullptr) {
        return ok();
    }
    // The follower runs the tick the issuer just ran, from the issuer's log alone.
    if (Status received = follower_->receive(issuer_.log(), follower_cursor_); !received) {
        return received;
    }
    if (Status advanced = follower_->advance(jobs); !advanced) {
        return advanced;
    }
    disagreements_ += follower_->state_hash() == issuer_.state_hash() ? 0U : 1U;
    return ok();
}

void bind_lockstep(abi::Host& host, LockstepAdapter* adapter) noexcept {
    host.game.lockstep = adapter;
}

}  // namespace cy::game_backend
