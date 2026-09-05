// Fixed-step integration on the simulation clock, and the interpolation pair. Task 4.2.3.

#include <cy/servers/physics/stepper.h>

#include <cy/core/math/scalar.h>

namespace cy::physics {

PhysicsStepper::PhysicsStepper(PhysicsServer& server, WorldHandle world,
                               Allocator& allocator) noexcept
    : server_(&server), world_(world), index_(allocator), bodies_(allocator), records_(allocator) {}

Status PhysicsStepper::track(BodyHandle body) noexcept {
    if (body.is_null()) {
        return fail(ErrorCode::InvalidArgument, "stepper: cannot track a null body");
    }
    if (index_.contains(body.bits())) {
        return ok();
    }
    const Expected<BodyState, Error> state = server_->body_state(body);
    if (!state) {
        return make_unexpected(state.error());
    }
    InterpolationRecord record;
    // Both halves of the pair start at the current transform, so a body created between two ticks
    // interpolates to a standstill rather than from the origin — which is what a `Transform{}`
    // previous would produce, and it looks exactly like a teleport from the world origin.
    record.previous = state->transform;
    record.current = state->transform;
    record.tick = last_tick_;
    if (Status reserved = records_.push_back(record); !reserved) {
        return reserved;
    }
    if (Status reserved = bodies_.push_back(body); !reserved) {
        records_.pop_back();
        return reserved;
    }
    const Expected<u32*, Error> inserted =
        index_.insert(body.bits(), static_cast<u32>(records_.size() - 1));
    if (!inserted) {
        records_.pop_back();
        bodies_.pop_back();
        return make_unexpected(inserted.error());
    }
    return ok();
}

void PhysicsStepper::untrack(BodyHandle body) noexcept {
    const u32* slot = index_.find(body.bits());
    if (slot == nullptr) {
        return;
    }
    const u32 removed = *slot;
    const u32 last = static_cast<u32>(records_.size() - 1);
    if (removed != last) {
        // Swap-and-pop, and the moved entry's index is repaired. The order of `records_` is not
        // meaningful — the walk that must be stable is the SERVER's hash order, which is by handle
        // and is not this array.
        records_[removed] = records_[last];
        bodies_[removed] = bodies_[last];
        if (u32* moved = index_.find(bodies_[removed].bits()); moved != nullptr) {
            *moved = removed;
        }
    }
    records_.pop_back();
    bodies_.pop_back();
    (void)index_.remove(body.bits());
}

Status PhysicsStepper::tick(const determinism::SimulationClock& clock,
                            TransformSink* sink) noexcept {
    StepInput input;
    // THE ONLY SOURCE OF A DELTA IN THIS FILE. `SimulationClock` cannot read a wall clock — see
    // its header — so neither can anything downstream of this line.
    input.delta_seconds = clock.delta_seconds();
    input.tick = clock.tick();

    if (Status stepped = server_->step(world_, input); !stepped) {
        return stepped;
    }
    ++steps_;
    last_tick_ = input.tick;

    for (usize slot = 0; slot < records_.size(); ++slot) {
        const BodyHandle body = bodies_[slot];
        const Expected<BodyState, Error> state = server_->body_state(body);
        if (!state) {
            // A body destroyed without being untracked. Skipped rather than failing the whole tick:
            // physics has already advanced, and a caller that loses one interpolation record has a
            // bookkeeping bug, not a simulation one.
            continue;
        }
        InterpolationRecord& record = records_[slot];
        record.previous = record.current;
        record.current = state->transform;
        record.tick = input.tick;
        record.teleported = state->teleported;
        if (sink != nullptr) {
            const Expected<UserData, Error> user_data = server_->body_user_data(body);
            sink->publish(body, user_data ? *user_data : 0, record.current, record.teleported);
        }
    }
    return ok();
}

const InterpolationRecord* PhysicsStepper::record(BodyHandle body) const noexcept {
    const u32* slot = index_.find(body.bits());
    return slot == nullptr ? nullptr : &records_[*slot];
}

Expected<Transform, Error> PhysicsStepper::interpolate(BodyHandle body, f32 alpha) const noexcept {
    const InterpolationRecord* found = record(body);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "stepper: the body is not tracked for interpolation");
    }
    // `physics` — "Teleport suppresses interpolation": the current transform, unblended, "avoiding
    // a smear across the world". Checked before the alpha is clamped, because a teleport at alpha 0
    // would otherwise return the PREVIOUS transform, which is the position the body was teleported
    // away from.
    if (found->teleported) {
        return found->current;
    }
    const f32 t = math::clamp(alpha, 0.0f, 1.0f);
    // Qualified: the member `interpolate` would otherwise hide the free function.
    return cy::interpolate(found->previous, found->current, t);
}

}  // namespace cy::physics
