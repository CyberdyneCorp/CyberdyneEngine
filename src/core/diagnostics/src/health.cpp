// The health model. One fixed array, one atomic per field, no allocation, no lock.
//
// The shape is chosen so that `health_active()` is callable from a signal handler: every read is a
// relaxed load of a lock-free atomic out of a function-local static that is constructed before any
// subsystem can report. A mutex here would make the crash artefact's health section unreachable
// from the one place it matters most.

#include <cy/core/diagnostics/health.h>

#include "internal.h"

#include <cy/core/diagnostics/field.h>
#include <cy/core/diagnostics/trace.h>

#include <algorithm>
#include <atomic>

namespace cy::diag {
namespace {

CY_TRACE_CATEGORY(health_category, "health")
CY_TRACE_NAME(health_transition, "health.transition")
CY_TRACE_FIELD(health_condition_field, id, cy::Privacy::Public)
CY_TRACE_FIELD(health_from, id, cy::Privacy::Public)
CY_TRACE_FIELD(health_to, id, cy::Privacy::Public)
CY_TRACE_FIELD(health_detail, u64, cy::Privacy::Public)

struct Condition {
    std::atomic<u8> severity{static_cast<u8>(HealthSeverity::Nominal)};
    std::atomic<u64> since_ns{0};
    std::atomic<u64> transitions{0};
    std::atomic<u64> reports{0};
    std::atomic<u64> detail{0};
};

struct Model {
    Condition conditions[kHealthConditionCount];
    std::atomic<HealthObserver> observer{nullptr};
    std::atomic<void*> observer_user{nullptr};
};

Model& model() noexcept {
    static Model instance;
    return instance;
}

bool in_range(HealthCondition condition) noexcept {
    return static_cast<u32>(condition) < kHealthConditionCount;
}

}  // namespace

void health_report(HealthCondition condition, HealthSeverity severity, u64 detail) noexcept {
    if (!in_range(condition)) {
        return;
    }
    Condition& entry = model().conditions[static_cast<u32>(condition)];
    entry.reports.fetch_add(1, std::memory_order_relaxed);
    entry.detail.store(detail, std::memory_order_relaxed);

    const u8 wanted = static_cast<u8>(severity);
    const u8 previous = entry.severity.exchange(wanted, std::memory_order_relaxed);
    if (previous == wanted) {
        return;  // the common case: a subsystem restating what is already true
    }
    entry.since_ns.store(monotonic_now_ns(), std::memory_order_relaxed);
    entry.transitions.fetch_add(1, std::memory_order_relaxed);

    // A transition is a record on the one timeline, so a capture shows health beside the frame that
    // caused it rather than in a second place a reader has to correlate by hand.
    const FieldValue fields[] = {
        field_u64(health_condition_field(), static_cast<u64>(condition)),
        field_u64(health_from(), previous),
        field_u64(health_to(), wanted),
        field_u64(health_detail(), detail),
    };
    trace_instant(health_transition(), health_category(), Channel::Critical, fields, 4);

    const HealthObserver observer = model().observer.load(std::memory_order_acquire);
    if (observer != nullptr) {
        observer(model().observer_user.load(std::memory_order_relaxed), condition,
                 static_cast<HealthSeverity>(previous), severity);
    }
}

HealthSnapshot health_snapshot() noexcept {
    HealthSnapshot snapshot;
    snapshot.captured_ns = monotonic_now_ns();
    Model& state = model();
    for (u32 index = 0; index < kHealthConditionCount; ++index) {
        const Condition& entry = state.conditions[index];
        HealthEntry& out = snapshot.conditions[index];
        out.severity = static_cast<HealthSeverity>(entry.severity.load(std::memory_order_relaxed));
        out.since_ns = entry.since_ns.load(std::memory_order_relaxed);
        out.transitions = entry.transitions.load(std::memory_order_relaxed);
        out.reports = entry.reports.load(std::memory_order_relaxed);
        out.detail = entry.detail.load(std::memory_order_relaxed);
        if (out.severity != HealthSeverity::Nominal) {
            ++snapshot.active;
            if (static_cast<u8>(out.severity) > static_cast<u8>(snapshot.worst)) {
                snapshot.worst = out.severity;
            }
        }
    }
    return snapshot;
}

HealthSeverity health_worst() noexcept {
    u8 worst = static_cast<u8>(HealthSeverity::Nominal);
    Model& state = model();
    for (const Condition& condition : state.conditions) {
        worst = std::max(condition.severity.load(std::memory_order_relaxed), worst);
    }
    return static_cast<HealthSeverity>(worst);
}

u32 health_active(HealthCondition* out, HealthSeverity* severity, u64* since_ns,
                  u32 capacity) noexcept {
    if (out == nullptr || capacity == 0) {
        return 0;
    }
    Model& state = model();
    u32 written = 0;
    for (u32 index = 0; index < kHealthConditionCount && written < capacity; ++index) {
        const u8 level = state.conditions[index].severity.load(std::memory_order_relaxed);
        if (level == static_cast<u8>(HealthSeverity::Nominal)) {
            continue;
        }
        out[written] = static_cast<HealthCondition>(index);
        if (severity != nullptr) {
            severity[written] = static_cast<HealthSeverity>(level);
        }
        if (since_ns != nullptr) {
            since_ns[written] = state.conditions[index].since_ns.load(std::memory_order_relaxed);
        }
        ++written;
    }
    return written;
}

void health_reset() noexcept {
    Model& state = model();
    for (Condition& entry : state.conditions) {
        entry.severity.store(static_cast<u8>(HealthSeverity::Nominal), std::memory_order_relaxed);
        entry.since_ns.store(0, std::memory_order_relaxed);
        entry.transitions.store(0, std::memory_order_relaxed);
        entry.reports.store(0, std::memory_order_relaxed);
        entry.detail.store(0, std::memory_order_relaxed);
    }
}

HealthObserver set_health_observer(HealthObserver observer, void* user) noexcept {
    Model& state = model();
    state.observer_user.store(user, std::memory_order_relaxed);
    return state.observer.exchange(observer, std::memory_order_acq_rel);
}

const char* health_condition_name(HealthCondition condition) noexcept {
    switch (condition) {
        case HealthCondition::FrameBudgetOverrun:
            return "frame_budget_overrun";
        case HealthCondition::TickBudgetOverrun:
            return "tick_budget_overrun";
        case HealthCondition::GpuBudgetOverrun:
            return "gpu_budget_overrun";
        case HealthCondition::MemoryPressure:
            return "memory_pressure";
        case HealthCondition::StreamingDeadlineMiss:
            return "streaming_deadline_miss";
        case HealthCondition::PacketLoss:
            return "packet_loss";
        case HealthCondition::RollbackFrequency:
            return "rollback_frequency";
        case HealthCondition::TaskStarvation:
            return "task_starvation";
        case HealthCondition::DeterminismDivergence:
            return "determinism_divergence";
        case HealthCondition::SaveFailure:
            return "save_failure";
        case HealthCondition::DiagnosticsLoss:
            return "diagnostics_loss";
    }
    return "unknown";
}

const char* health_severity_name(HealthSeverity severity) noexcept {
    switch (severity) {
        case HealthSeverity::Nominal:
            return "nominal";
        case HealthSeverity::Degraded:
            return "degraded";
        case HealthSeverity::Critical:
            return "critical";
    }
    return "unknown";
}

}  // namespace cy::diag
