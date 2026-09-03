// The pressure monitor: derivation, hysteresis and broadcast. Task 2.3.

#include <cy/core/memory/pressure.h>

#include <cy/core/memory/diagnostics.h>

namespace cy {

const char* pressure_level_name(PressureLevel level) noexcept {
    switch (level) {
        case PressureLevel::Normal:
            return "normal";
        case PressureLevel::Elevated:
            return "elevated";
        case PressureLevel::Critical:
            return "critical";
    }
    return "unknown";
}

Status PressureMonitor::subscribe(PressureResponder& responder) noexcept {
    if (responder.subscribed_) {
        return fail(ErrorCode::AlreadyExists, "responder is already subscribed to this monitor");
    }
    responder.next_responder_ = responders_;
    responder.subscribed_ = true;
    responders_ = &responder;
    ++subscriber_count_;
    return ok();
}

Status PressureMonitor::unsubscribe(PressureResponder& responder) noexcept {
    PressureResponder** link = &responders_;
    while (*link != nullptr) {
        if (*link == &responder) {
            *link = responder.next_responder_;
            responder.next_responder_ = nullptr;
            responder.subscribed_ = false;
            --subscriber_count_;
            return ok();
        }
        link = &(*link)->next_responder_;
    }
    return fail(ErrorCode::NotFound, "responder is not subscribed to this monitor");
}

void PressureMonitor::set_thresholds(const PressureThresholds& thresholds) noexcept {
    CY_ASSERT_MSG(
        thresholds.elevated_fall <= thresholds.elevated_rise &&
            thresholds.critical_fall <= thresholds.critical_rise,
        "a fall threshold above its rise threshold is not hysteresis, it is an inversion");
    thresholds_ = thresholds;
}

void PressureMonitor::report_platform_level(PressureLevel level) noexcept {
    platform_level_ = level;
}

/// The level `utilisation` implies, given where the level already is. This is the whole of the
/// hysteresis: a rise uses the rise thresholds, and staying at a level uses the lower fall ones, so
/// utilisation between the two changes nothing in either direction.
PressureLevel PressureMonitor::level_for(f64 utilisation) const noexcept {
    if (level_ == PressureLevel::Critical) {
        if (utilisation >= thresholds_.critical_fall) {
            return PressureLevel::Critical;
        }
        return (utilisation >= thresholds_.elevated_fall) ? PressureLevel::Elevated
                                                          : PressureLevel::Normal;
    }
    if (utilisation >= thresholds_.critical_rise) {
        return PressureLevel::Critical;
    }
    if (level_ == PressureLevel::Elevated) {
        return (utilisation >= thresholds_.elevated_fall) ? PressureLevel::Elevated
                                                          : PressureLevel::Normal;
    }
    return (utilisation >= thresholds_.elevated_rise) ? PressureLevel::Elevated
                                                      : PressureLevel::Normal;
}

PressureLevel PressureMonitor::evaluate(const BudgetTree& budgets) noexcept {
    MemoryDomain worst = MemoryDomain::Engine;
    const f64 utilisation = budgets.peak_utilisation(worst);
    PressureLevel next = level_for(utilisation);

    // The platform's opinion is a floor, never a ceiling: the operating system knows about memory
    // the engine's budgets do not describe, and it is never right to report less than it does.
    if (static_cast<u8>(platform_level_) > static_cast<u8>(next)) {
        next = platform_level_;
    }

    if (next != level_) {
        broadcast(next, worst, utilisation);
    }
    return level_;
}

void PressureMonitor::force(PressureLevel level, MemoryDomain cause) noexcept {
    if (level != level_) {
        broadcast(level, cause, 0.0);
    }
}

void PressureMonitor::broadcast(PressureLevel next, MemoryDomain cause, f64 utilisation) noexcept {
    const PressureLevel previous = level_;
    level_ = next;

    PressureTransition& record = history_[history_next_];
    record.from = previous;
    record.to = next;
    record.cause = cause;
    record.utilisation = utilisation;
    record.sequence = ++sequence_;
    history_next_ = (history_next_ + 1) % kPressureHistoryDepth;
    if (history_count_ < kPressureHistoryDepth) {
        ++history_count_;
    }

    // The list is walked from a local so that a responder unsubscribing itself inside its own
    // callback — which is what a subsystem shutting down under Critical pressure does — does not
    // cut the walk short. `next_responder_` is read before the callback for the same reason.
    PressureResponder* responder = responders_;
    while (responder != nullptr) {
        PressureResponder* following = responder->next_responder_;
        responder->on_pressure(next, previous);
        responder = following;
    }
}

u32 PressureMonitor::history(PressureTransition* out, u32 capacity) const noexcept {
    const u32 count = (history_count_ < capacity) ? history_count_ : capacity;
    // history_next_ is one past the newest; the oldest of `count` entries is that many behind it.
    const u32 first = (history_next_ + kPressureHistoryDepth - count) % kPressureHistoryDepth;
    for (u32 index = 0; index < count; ++index) {
        out[index] = history_[(first + index) % kPressureHistoryDepth];
    }
    return count;
}

void PressureMonitor::reset() noexcept {
    level_ = PressureLevel::Normal;
    platform_level_ = PressureLevel::Normal;
    sequence_ = 0;
    history_count_ = 0;
    history_next_ = 0;
}

PressureMonitor& default_pressure_monitor() noexcept {
    static PressureMonitor monitor;
    return monitor;
}

PressureLevel update_memory_pressure() noexcept {
    PressureMonitor& monitor = default_pressure_monitor();
    const u64 before = monitor.transition_count();
    const PressureLevel level = monitor.evaluate(default_budget_tree());
    if (monitor.transition_count() == before) {
        return level;
    }
    // The transition just recorded is the newest one in the history, and it carries the domain and
    // the utilisation that caused it — which is why the trace record is emitted from here rather
    // than from broadcast(): the monitor itself has no dependency on the trace, and adding one
    // would put the diagnostics headers behind every include of `pressure.h`.
    PressureTransition newest;
    if (monitor.history(&newest, 1) == 1) {
        memory_trace_pressure(newest.to, newest.from, newest.cause, newest.utilisation);
    }
    return level;
}

}  // namespace cy
