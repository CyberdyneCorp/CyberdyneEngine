#pragma once
// Typed gameplay events, their delivery modes, and tagged message channels. M8.b task 3.3.
//
// `gameplay-framework` — "Events and messages": events are typed and accumulated in per-worker
// buffers and committed deterministically; delivery mode is declared — "immediate local, end of
// phase, next tick, or networked. **Phase buffered SHALL be the default**; arbitrary synchronous
// cascades through gameplay code SHALL be discouraged and SHALL NOT be the default"; events are
// targetable — global, entity, participant, team, or world region; a **message channel identified
// by a gameplay tag** provides loose coupling; and an event declares whether it is replay relevant,
// network relevant, or telemetry relevant, because "raw event streams SHALL NOT automatically
// become network or replay streams."
//
// ================================================================================================
// COMMANDS AND EVENTS ARE NOT THE SAME THING, AND THIS FILE IS WHERE THAT IS ENFORCED
// ================================================================================================
//
// "**Commands and events SHALL NOT be conflated**: a command asks for something to happen, an event
// reports that something happened." It is on the forbidden-patterns list too. So `GameplayEvent`
// and `Command` are unrelated types with no conversion between them in either direction, and
// `tests/test_events.cpp` asserts exactly that with a type trait — because the way the two become
// one is not a decision anybody makes, it is a helper somebody adds.
//
// ================================================================================================
// WHY THE DEFAULT DELIVERY IS THE BUFFERED ONE
// ================================================================================================
//
// The alternative is a call. Damage applies, which raises an event, which a health system handles
// synchronously, which raises a death event, which a score system handles synchronously — and now
// the order of a match's scoring depends on the order two systems happened to register. Buffering
// to the end of the phase makes the order a property of the commit rather than of the call stack,
// which is the same argument `command.h` makes for the command merge and for the same reason.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>
#include <cy/gameplay/context.h>
#include <cy/gameplay/tags.h>
#include <cy/gameplay/teams.h>

#include <cstring>

namespace cy::gameplay {

/// When an event reaches its readers.
enum class Delivery : u8 {
    /// Delivered as it is published. For the rare case that genuinely cannot wait; never a default.
    ImmediateLocal = 0,
    /// THE DEFAULT. Delivered when the phase closes.
    EndOfPhase,
    NextTick,
    /// Delivered locally at the end of the phase and marked for transmission.
    Networked,
    Count,
};

const char* delivery_name(Delivery delivery) noexcept;

/// Who an event is addressed to.
enum class EventTarget : u8 {
    Global = 0,
    Entity,
    Participant,
    Team,
    /// A world region, named rather than resolved here: this module has no world.
    Region,
    Count,
};

using EventTypeId = u32;
inline constexpr EventTypeId kInvalidEventType = 0xFFFFFFFFU;

/// What an event type is, as declared. The three relevance flags are separate because an event
/// being interesting to telemetry says nothing about whether a replay needs it.
struct EventDeclaration {
    Name name;
    u32 stable_id = 0;
    u16 schema_version = 1;
    Delivery delivery = Delivery::EndOfPhase;
    bool replay_relevant = false;
    bool network_relevant = false;
    bool telemetry_relevant = false;
    /// The message channel, as a gameplay tag. The interface subscribes to the tag and never names
    /// the system that publishes on it.
    TagId channel = kInvalidTag;
};

inline constexpr u16 kMaxEventPayload = 32;

/// One event. A value: no pointer, no allocation, and small enough that a buffer of them is an
/// array rather than a list of objects.
struct GameplayEvent {
    EventTypeId type = kInvalidEventType;
    EventTarget target = EventTarget::Global;
    u64 tick = 0;
    ecs::Entity entity;
    ParticipantId participant;
    TeamId team = kNoTeam;
    Name region;
    u32 sequence = 0;
    u16 payload_size = 0;
    u8 payload[kMaxEventPayload] = {};

    template <class T>
    [[nodiscard]] bool set_payload(const T& value) noexcept {
        static_assert(sizeof(T) <= kMaxEventPayload, "an event reports, it does not carry content");
        static_assert(__is_trivially_copyable(T), "an event payload must be a POD");
        payload_size = static_cast<u16>(sizeof(T));
        std::memcpy(static_cast<void*>(payload), static_cast<const void*>(&value), sizeof(T));
        return true;
    }

    template <class T>
    [[nodiscard]] bool read_payload(T& out) const noexcept {
        static_assert(__is_trivially_copyable(T), "an event payload must be a POD");
        if (payload_size != sizeof(T)) {
            return false;
        }
        std::memcpy(static_cast<void*>(&out), static_cast<const void*>(payload), sizeof(T));
        return true;
    }
};

/// Declared types, per-producer buffers, and the deterministic delivery.
///
/// The buffer-and-merge shape is `CommandStream`'s, for the same reason: a producer records with no
/// lock and the merge key is `(producer order, sequence)` — never a thread identity, which
/// `simulation-and-determinism` forbids in an ordering key.
class EventBus {
public:
    explicit EventBus(Allocator& allocator) noexcept;

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    [[nodiscard]] Expected<EventTypeId, Error> declare(
        const EventDeclaration& declaration) noexcept;
    [[nodiscard]] EventTypeId find(u32 stable_id) const noexcept;
    [[nodiscard]] const EventDeclaration& declaration(EventTypeId type) const noexcept {
        return declarations_[type];
    }
    [[nodiscard]] u32 type_count() const noexcept { return static_cast<u32>(declarations_.size()); }

    [[nodiscard]] Expected<u32, Error> open_producer(Name debug_name) noexcept;
    [[nodiscard]] u32 producer_count() const noexcept {
        return static_cast<u32>(producers_.size());
    }

    /// Publish. An `ImmediateLocal` event is delivered here; everything else waits for `deliver()`.
    [[nodiscard]] Status publish(u32 producer, const GameplayEvent& event) noexcept;

    /// Deliver everything whose declared mode is `mode`, in `(producer, sequence)` order. The
    /// events move to `delivered()`; a reader consumes them and calls `clear_delivered()`.
    u32 deliver(Delivery mode) noexcept;

    [[nodiscard]] u32 delivered_count() const noexcept {
        return static_cast<u32>(delivered_.size());
    }
    [[nodiscard]] const GameplayEvent& delivered(u32 index) const noexcept {
        return delivered_[index];
    }
    void clear_delivered() noexcept { delivered_.clear(); }
    [[nodiscard]] u32 pending_count() const noexcept;

    /// Every delivered event whose declared channel matches `channel`, hierarchically. The
    /// interface's door: it names a tag, never a system.
    [[nodiscard]] u32 observe(const TagRegistry& registry, TagId channel, GameplayEvent* out,
                              u32 capacity) const noexcept;

    /// The delivered events addressed to that target. A reader filters here rather than each system
    /// scanning the whole buffer for its own.
    [[nodiscard]] u32 addressed_to_entity(ecs::Entity entity, GameplayEvent* out,
                                          u32 capacity) const noexcept;

    /// The events that go on the wire, and the ones a replay records. Derived from the declaration,
    /// never set per event: "raw event streams SHALL NOT automatically become network or replay
    /// streams."
    [[nodiscard]] bool network_relevant(EventTypeId type) const noexcept;
    [[nodiscard]] bool replay_relevant(EventTypeId type) const noexcept;

private:
    struct Producer {
        Name debug_name;
        Array<GameplayEvent> events;
        u32 next_sequence = 0;
    };

    Allocator* allocator_;
    Array<EventDeclaration> declarations_;
    Array<Producer> producers_;
    Array<GameplayEvent> delivered_;
};

}  // namespace cy::gameplay
