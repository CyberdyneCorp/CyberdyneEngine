#pragma once
// Smart objects: world objects that advertise what can be done with them, and the reservations that
// stop two agents claiming one seat. M8.b task 6.4.
//
// ================================================================================================
// AN AGENT ASKS FOR AN AFFORDANCE, NEVER FOR A CLASS
// ================================================================================================
//
// `ai-system`: "Agents SHALL query for an affordance (`Recharge`, `Cover`, `Seat`) rather than for
// an object class", and its scenario states what that buys: "WHEN a new charging station type is
// added advertising `Recharge` THEN existing agents SHALL use it WITH NO CHANGE TO ANY AI GRAPH."
//
// So the registry is keyed on a `Name` and nothing here knows what a charging station is. An
// affordance carries what an agent needs in order to decide — where to stand, which way to face,
// what it requires and what it gives — and a behaviour fragment naming the interaction, which is a
// graph the agent runs and not code this module calls.
//
// ================================================================================================
// A RESERVATION IS A CLAIM WITH AN OWNER, AND IT IS RELEASED THREE WAYS
// ================================================================================================
//
// `ai-system`: "The system SHALL manage SLOT RESERVATION, so two agents do not claim the same
// single-occupancy slot, with reservations released on COMPLETION, FAILURE, OR AGENT DESTRUCTION."
//
// All three are one operation from this module's side — `release(agent)` — and the third is the one
// that decides the data layout: an agent that is destroyed cannot release anything itself, so the
// registry indexes reservations BY AGENT and `release_missing()` sweeps the ones whose holder is no
// longer alive. A reservation table that could only be unwound by its holder would leak a seat
// every time an agent died in it.

#include <cy/core/base/expected.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>

namespace cy::ai {

using ecs::Entity;

using SlotId = u32;
inline constexpr SlotId kInvalidSlotId = 0xFFFFFFFFu;

/// What an object advertises. `ai-system`'s list: "named capabilities with slots, requirements,
/// effects, an interaction location and approach direction, and an optional behaviour fragment
/// describing the interaction".
struct Affordance {
    /// `Recharge`, `Cover`, `Seat`. The only thing an agent asks for.
    Name capability;
    /// The object providing it.
    Entity provider;
    /// Where the agent stands, and which way it faces.
    Vec3 interaction_point;
    Vec3 approach_direction{0.0F, 0.0F, 1.0F};
    /// What the agent must have. A bit set the caller assigns meaning to, tested by AND.
    u64 requires_capabilities = 0;
    /// What using it does, in the same terms the behaviour graph's world state uses — so a GOAP
    /// plan can name "Recharge" as an operator and read its effect from here.
    u64 sets_true = 0;
    u64 sets_false = 0;
    /// The behaviour fragment describing the interaction, or the empty name.
    Name fragment;
    /// How many agents may use it at once. One is a seat; several is a doorway.
    u16 capacity = 1;
};

/// One live claim.
struct Reservation {
    Entity holder;
    SlotId slot = kInvalidSlotId;
    u32 tick = 0;
};

/// A candidate answered to a query, with the distance that ranked it.
struct AffordanceCandidate {
    SlotId slot = kInvalidSlotId;
    Entity provider;
    Vec3 interaction_point;
    f32 distance = 0.0F;
    u16 free_capacity = 0;
};

/// Every affordance in one world, and every reservation against them.
class SmartObjectRegistry {
public:
    explicit SmartObjectRegistry(Allocator& allocator) noexcept;

    SmartObjectRegistry(const SmartObjectRegistry&) = delete;
    SmartObjectRegistry& operator=(const SmartObjectRegistry&) = delete;

    [[nodiscard]] Expected<SlotId, Error> advertise(const Affordance& affordance) noexcept;
    [[nodiscard]] Status withdraw(SlotId slot) noexcept;
    [[nodiscard]] const Affordance* affordance(SlotId slot) const noexcept;
    [[nodiscard]] u32 size() const noexcept;

    /// Every slot advertising `capability` within `radius` of `from` that the agent's capabilities
    /// admit and that has room, nearest first.
    ///
    /// It answers CANDIDATES rather than one slot, because `ai-system`'s contention scenario needs
    /// the loser to "receive the NEXT candidate" without asking again.
    [[nodiscard]] Status find(Name capability, Vec3 from, f32 radius, u64 agent_capabilities,
                              Array<AffordanceCandidate>& out) const noexcept;

    /// Claim a slot for `holder`. Fails with `AlreadyExists` when the slot is full, which is what
    /// makes "exactly one SHALL reserve it" a checkable property rather than a race.
    [[nodiscard]] Status reserve(SlotId slot, Entity holder, u32 tick) noexcept;
    /// Completion, failure, or an agent giving up. All three are this.
    [[nodiscard]] Status release(Entity holder) noexcept;
    /// Agent destruction. `alive` answers whether an entity still exists; every reservation whose
    /// holder it refuses is released. Returns how many.
    [[nodiscard]] u32 release_missing(bool (*alive)(Entity, void*) noexcept, void* user) noexcept;

    [[nodiscard]] u16 free_capacity(SlotId slot) const noexcept;
    [[nodiscard]] Span<const Reservation> reservations() const noexcept {
        return reservations_.span();
    }
    [[nodiscard]] const Reservation* reservation_of(Entity holder) const noexcept;

private:
    struct Entry {
        Affordance affordance;
        bool live = false;
    };

    Array<Entry> slots_;
    Array<Reservation> reservations_;
};

}  // namespace cy::ai
