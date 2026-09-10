// Smart objects and their reservations. See cy/ai/smart_object.h.

#include <cy/ai/smart_object.h>

#include <cmath>

namespace cy::ai {

SmartObjectRegistry::SmartObjectRegistry(Allocator& allocator) noexcept
    : slots_(allocator), reservations_(allocator) {}

Expected<SlotId, Error> SmartObjectRegistry::advertise(const Affordance& affordance) noexcept {
    if (affordance.capability.is_empty() || affordance.capacity == 0) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "an affordance needs a capability name and room for at least "
                                     "one agent"});
    }
    for (usize index = 0; index < slots_.size(); ++index) {
        if (!slots_[index].live) {
            slots_[index].affordance = affordance;
            slots_[index].live = true;
            return static_cast<SlotId>(index);
        }
    }
    Entry entry;
    entry.affordance = affordance;
    entry.live = true;
    if (Status pushed = slots_.push_back(entry); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<SlotId>(slots_.size() - 1);
}

Status SmartObjectRegistry::withdraw(SlotId slot) noexcept {
    if (slot >= slots_.size() || !slots_[slot].live) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such affordance slot"});
    }
    slots_[slot].live = false;
    // Every claim on it goes with it: an object that is gone cannot be sat on, and a reservation
    // that outlived its slot would hold a seat nobody can reach.
    usize index = 0;
    while (index < reservations_.size()) {
        if (reservations_[index].slot == slot) {
            reservations_.remove_unordered(index);
            continue;
        }
        ++index;
    }
    return ok();
}

const Affordance* SmartObjectRegistry::affordance(SlotId slot) const noexcept {
    if (slot >= slots_.size() || !slots_[slot].live) {
        return nullptr;
    }
    return &slots_[slot].affordance;
}

u32 SmartObjectRegistry::size() const noexcept {
    u32 live = 0;
    for (const Entry& entry : slots_.span()) {
        live += entry.live ? 1u : 0u;
    }
    return live;
}

u16 SmartObjectRegistry::free_capacity(SlotId slot) const noexcept {
    if (slot >= slots_.size() || !slots_[slot].live) {
        return 0;
    }
    u16 taken = 0;
    for (const Reservation& reservation : reservations_.span()) {
        taken += (reservation.slot == slot) ? 1u : 0u;
    }
    const u16 capacity = slots_[slot].affordance.capacity;
    return (taken >= capacity) ? 0u : static_cast<u16>(capacity - taken);
}

Status SmartObjectRegistry::find(Name capability, Vec3 from, f32 radius, u64 agent_capabilities,
                                 Array<AffordanceCandidate>& out) const noexcept {
    out.clear();
    for (usize index = 0; index < slots_.size(); ++index) {
        const Entry& entry = slots_[index];
        if (!entry.live || entry.affordance.capability != capability) {
            continue;
        }
        // `ai-system` gates a link on a capability the same way `navigation` gates an off-mesh
        // link: every bit the affordance requires must be set on the agent.
        if ((entry.affordance.requires_capabilities & ~agent_capabilities) != 0) {
            continue;
        }
        const Vec3 offset = entry.affordance.interaction_point - from;
        const f32 distance_sq = (offset.x * offset.x) + (offset.z * offset.z);
        if (distance_sq > radius * radius) {
            continue;
        }
        const u16 free = free_capacity(static_cast<SlotId>(index));
        if (free == 0) {
            continue;
        }
        AffordanceCandidate candidate;
        candidate.slot = static_cast<SlotId>(index);
        candidate.provider = entry.affordance.provider;
        candidate.interaction_point = entry.affordance.interaction_point;
        candidate.distance = std::sqrt(distance_sq);
        candidate.free_capacity = free;
        if (Status pushed = out.push_back(candidate); !pushed) {
            return pushed;
        }
    }

    // Nearest first, and the tie-break is the slot id — so two agents asking the same question get
    // the same ordering, and the contention scenario has a defined winner rather than a race.
    for (usize index = 1; index < out.size(); ++index) {
        const AffordanceCandidate candidate = out[index];
        usize position = index;
        while (position > 0) {
            const AffordanceCandidate& previous = out[position - 1];
            const bool after =
                previous.distance > candidate.distance ||
                (previous.distance == candidate.distance && previous.slot > candidate.slot);
            if (!after) {
                break;
            }
            out[position] = previous;
            --position;
        }
        out[position] = candidate;
    }
    return ok();
}

Status SmartObjectRegistry::reserve(SlotId slot, Entity holder, u32 tick) noexcept {
    if (slot >= slots_.size() || !slots_[slot].live) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such affordance slot"});
    }
    for (const Reservation& reservation : reservations_.span()) {
        if (reservation.holder == holder) {
            return make_unexpected(Error{ErrorCode::AlreadyExists,
                                         "that agent already holds a reservation; release it "
                                         "before claiming another"});
        }
    }
    if (free_capacity(slot) == 0) {
        return make_unexpected(
            Error{ErrorCode::AlreadyExists, "that slot is full; take the next candidate"});
    }
    Reservation reservation;
    reservation.holder = holder;
    reservation.slot = slot;
    reservation.tick = tick;
    return reservations_.push_back(reservation);
}

Status SmartObjectRegistry::release(Entity holder) noexcept {
    for (usize index = 0; index < reservations_.size(); ++index) {
        if (reservations_[index].holder == holder) {
            reservations_.remove_unordered(index);
            return ok();
        }
    }
    return make_unexpected(Error{ErrorCode::NotFound, "that agent holds no reservation"});
}

u32 SmartObjectRegistry::release_missing(bool (*alive)(Entity, void*) noexcept,
                                         void* user) noexcept {
    if (alive == nullptr) {
        return 0;
    }
    u32 released = 0;
    usize index = 0;
    while (index < reservations_.size()) {
        if (!alive(reservations_[index].holder, user)) {
            reservations_.remove_unordered(index);
            ++released;
            continue;
        }
        ++index;
    }
    return released;
}

const Reservation* SmartObjectRegistry::reservation_of(Entity holder) const noexcept {
    for (const Reservation& reservation : reservations_.span()) {
        if (reservation.holder == holder) {
            return &reservation;
        }
    }
    return nullptr;
}

}  // namespace cy::ai
