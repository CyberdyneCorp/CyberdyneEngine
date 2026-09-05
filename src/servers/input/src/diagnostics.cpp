// The input inspector's storage. Task 4.1.6.

#include <cy/servers/input/diagnostics.h>

namespace cy::input {

const char* action_outcome_name(ActionOutcome outcome) noexcept {
    switch (outcome) {
        case ActionOutcome::Triggered:
            return "Triggered";
        case ActionOutcome::NoBindingInActiveContext:
            return "NoBindingInActiveContext";
        case ActionOutcome::BelowThreshold:
            return "BelowThreshold";
        case ActionOutcome::ConsumedByHigherContext:
            return "ConsumedByHigherContext";
        case ActionOutcome::TriggerConditionsUnmet:
            return "TriggerConditionsUnmet";
        case ActionOutcome::SuppressedByFocus:
            return "SuppressedByFocus";
        case ActionOutcome::DeviceUnassigned:
            return "DeviceUnassigned";
        case ActionOutcome::Count:
            break;
    }
    return "NoBindingInActiveContext";
}

Status InputTrace::enable(u32 action_count, u32 event_capacity) noexcept {
    if (Status sized = actions_.resize(action_count); !sized) {
        return sized;
    }
    if (Status sized = events_.reserve(event_capacity); !sized) {
        return sized;
    }
    event_capacity_ = event_capacity;
    event_write_ = 0;
    events_.clear();
    latency_.clear();
    enabled_ = true;
    return ok();
}

void InputTrace::disable() noexcept {
    enabled_ = false;
    events_.clear();
    latency_.clear();
}

void InputTrace::begin_tick(u64 tick) noexcept {
    tick_ = tick;
    if (!enabled_) {
        return;
    }
    for (usize index = 0; index < actions_.size(); ++index) {
        ActionTrace& entry = actions_[index];
        entry = ActionTrace{};
        entry.action = static_cast<ActionId>(index);
        // "Not bound in an active context" is the right default rather than "triggered": an action
        // nothing touched this tick was, in fact, not bound in anything that ran.
        entry.outcome = ActionOutcome::NoBindingInActiveContext;
    }
    latency_.clear();
}

ActionTrace* InputTrace::action(ActionId action) noexcept {
    if (!enabled_ || action >= actions_.size()) {
        return nullptr;
    }
    return &actions_[action];
}

const ActionTrace* InputTrace::action(ActionId action) const noexcept {
    if (!enabled_ || action >= actions_.size()) {
        return nullptr;
    }
    return &actions_[action];
}

void InputTrace::record_event(const DeviceEvent& event) noexcept {
    if (!enabled_ || event_capacity_ == 0) {
        return;
    }
    if (events_.size() < event_capacity_) {
        (void)events_.push_back(event);
        event_write_ = events_.size() % event_capacity_;
        return;
    }
    // A ring, so a session that runs for an hour holds the last `event_capacity_` events rather
    // than an hour of them. A trace is a window on recent history; a log that grows without bound
    // is a different tool and would be a different decision.
    events_[event_write_] = event;
    event_write_ = (event_write_ + 1) % event_capacity_;
}

void InputTrace::record_latency(const LatencySample& sample) noexcept {
    if (!enabled_) {
        return;
    }
    (void)latency_.push_back(sample);
}

}  // namespace cy::input
