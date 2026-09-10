// Typed gameplay events, delivery modes and tagged message channels. M8.b task 3.3.

#include <cy/gameplay/events.h>

#include <utility>

namespace cy::gameplay {

const char* delivery_name(Delivery delivery) noexcept {
    switch (delivery) {
        case Delivery::ImmediateLocal:
            return "ImmediateLocal";
        case Delivery::EndOfPhase:
            return "EndOfPhase";
        case Delivery::NextTick:
            return "NextTick";
        case Delivery::Networked:
            return "Networked";
        case Delivery::Count:
            break;
    }
    return "EndOfPhase";
}

EventBus::EventBus(Allocator& allocator) noexcept
    : allocator_(&allocator),
      declarations_(allocator),
      producers_(allocator),
      delivered_(allocator) {}

Expected<EventTypeId, Error> EventBus::declare(const EventDeclaration& declaration) noexcept {
    if (declaration.stable_id == 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "an event's stable identity is never zero", 0});
    }
    if (find(declaration.stable_id) != kInvalidEventType) {
        return make_unexpected(
            Error{ErrorCode::AlreadyExists, "that event identity is declared", 0});
    }
    if (Status pushed = declarations_.push_back(declaration); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<EventTypeId>(declarations_.size() - 1);
}

EventTypeId EventBus::find(u32 stable_id) const noexcept {
    for (usize index = 0; index < declarations_.size(); ++index) {
        if (declarations_[index].stable_id == stable_id) {
            return static_cast<EventTypeId>(index);
        }
    }
    return kInvalidEventType;
}

Expected<u32, Error> EventBus::open_producer(Name debug_name) noexcept {
    Producer producer{debug_name, Array<GameplayEvent>(*allocator_), 0};
    if (Status pushed = producers_.push_back(std::move(producer)); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<u32>(producers_.size() - 1);
}

Status EventBus::publish(u32 producer, const GameplayEvent& event) noexcept {
    if (producer >= producers_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such event producer", 0});
    }
    if (event.type >= declarations_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such event type", 0});
    }
    GameplayEvent stamped = event;
    stamped.sequence = producers_[producer].next_sequence++;
    if (declarations_[event.type].delivery == Delivery::ImmediateLocal) {
        return delivered_.push_back(stamped);
    }
    return producers_[producer].events.push_back(stamped);
}

u32 EventBus::deliver(Delivery mode) noexcept {
    // THE MERGE KEY IS (producer order, sequence). Producers are walked in registration order and
    // each producer's buffer is already in sequence order, so the delivered order is a property of
    // the commit rather than of which worker ran first.
    u32 moved = 0;
    for (Producer& producer : producers_) {
        usize index = 0;
        while (index < producer.events.size()) {
            const GameplayEvent& event = producer.events[index];
            if (declarations_[event.type].delivery != mode) {
                ++index;
                continue;
            }
            if (Status pushed = delivered_.push_back(event); !pushed) {
                return moved;
            }
            producer.events.erase(index);
            ++moved;
        }
    }
    return moved;
}

u32 EventBus::pending_count() const noexcept {
    u32 total = 0;
    for (const Producer& producer : producers_) {
        total += static_cast<u32>(producer.events.size());
    }
    return total;
}

u32 EventBus::observe(const TagRegistry& registry, TagId channel, GameplayEvent* out,
                      u32 capacity) const noexcept {
    u32 found = 0;
    for (const GameplayEvent& event : delivered_) {
        const TagId declared = declarations_[event.type].channel;
        if (declared == kInvalidTag || !registry.matches(channel, declared)) {
            continue;
        }
        if (out != nullptr && found < capacity) {
            out[found] = event;
        }
        ++found;
    }
    return found;
}

u32 EventBus::addressed_to_entity(ecs::Entity entity, GameplayEvent* out,
                                  u32 capacity) const noexcept {
    u32 found = 0;
    for (const GameplayEvent& event : delivered_) {
        if (event.target != EventTarget::Entity || event.entity != entity) {
            continue;
        }
        if (out != nullptr && found < capacity) {
            out[found] = event;
        }
        ++found;
    }
    return found;
}

bool EventBus::network_relevant(EventTypeId type) const noexcept {
    return type < declarations_.size() && declarations_[type].network_relevant;
}

bool EventBus::replay_relevant(EventTypeId type) const noexcept {
    return type < declarations_.size() && declarations_[type].replay_relevant;
}

}  // namespace cy::gameplay
