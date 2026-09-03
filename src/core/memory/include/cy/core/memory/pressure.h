#pragma once
// Memory pressure: the level, its hysteresis, and the response a subsystem declares. Task 2.3.
//
// `core-memory-and-containers` — "Memory pressure levels": Normal, Elevated or Critical, derived
// from budget utilisation and from platform-reported memory conditions, and broadcast to
// subsystems. Pressure is THE coordination mechanism for memory, in the same way the renderer's
// budget arbiter coordinates GPU time: subsystems respond to the declared level rather than each
// polling the platform, so one cache does not free memory that another immediately consumes.
//
// | Level      | Expected response                                                        |
// |------------|--------------------------------------------------------------------------|
// | `Normal`   | Prefetch and cache freely within budget                                  |
// | `Elevated` | Trim caches, reduce prefetch distance, evict unreferenced data           |
// | `Critical` | Drop optional caches, force streaming quality down, defer non-essential work |
//
// WHAT IS NOT HERE. The residency layer that weighs paged subsystems — virtual geometry, virtual
// textures, virtual shadows, illumination caches — against each other by importance is `residency`,
// and it lands at M6. It will be one responder on this monitor rather than a second mechanism
// beside it, which is why `PressureResponder` carries a name and a priority: the residency layer
// registers ahead of the subsystems it arbitrates for.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/budget.h>
#include <cy/core/memory/domain.h>

namespace cy {

enum class PressureLevel : u8 {
    Normal = 0,
    Elevated = 1,
    Critical = 2,
};

[[nodiscard]] const char* pressure_level_name(PressureLevel level) noexcept;

/// The utilisation at which the level rises, and the lower one at which it falls back.
///
/// THE GAP IS THE POINT. A single threshold makes a subsystem hovering at it trim, refill, trim and
/// refill; the level rises at `rise` and only falls again at `fall`, so the refill has to undo
/// materially more than the trim before anything changes back.
struct PressureThresholds {
    f64 elevated_rise = 0.85;
    f64 elevated_fall = 0.75;
    f64 critical_rise = 0.95;
    f64 critical_fall = 0.88;
};

/// One recorded change of level, kept so that a crash or a report can say pressure preceded the
/// failure rather than asserting that it must have.
struct PressureTransition {
    PressureLevel from = PressureLevel::Normal;
    PressureLevel to = PressureLevel::Normal;
    MemoryDomain cause = MemoryDomain::Engine;  // the domain whose utilisation drove it
    f64 utilisation = 0.0;
    u64 sequence = 0;  // monotonic, so a reader can tell two transitions apart
};

/// What a subsystem implements to take part. Declaring the response is the requirement — a cache
/// that polls platform memory itself is the pattern this replaces.
class PressureResponder {
public:
    virtual ~PressureResponder() = default;

    /// For a report. Never null.
    [[nodiscard]] virtual const char* responder_name() const noexcept = 0;

    /// Called on every change of level, on the thread that called `evaluate()`. `previous` is given
    /// so a responder can tell a rise from a fall without keeping its own copy.
    virtual void on_pressure(PressureLevel level, PressureLevel previous) noexcept = 0;

private:
    // The monitor threads its subscriber list through the responders themselves, so subscribing
    // allocates nothing and a responder can register during startup before any allocator is
    // configured. Only PressureMonitor touches these.
    friend class PressureMonitor;
    PressureResponder* next_responder_ = nullptr;
    bool subscribed_ = false;
};

inline constexpr u32 kPressureHistoryDepth = 32;

class PressureMonitor {
public:
    PressureMonitor() noexcept = default;

    PressureMonitor(const PressureMonitor&) = delete;
    PressureMonitor& operator=(const PressureMonitor&) = delete;

    Status subscribe(PressureResponder& responder) noexcept;
    Status unsubscribe(PressureResponder& responder) noexcept;
    [[nodiscard]] u32 subscriber_count() const noexcept { return subscriber_count_; }

    void set_thresholds(const PressureThresholds& thresholds) noexcept;
    [[nodiscard]] const PressureThresholds& thresholds() const noexcept { return thresholds_; }

    /// The platform's own opinion — a low-memory notification, a foreground/background transition.
    /// Folded into the level as a floor: the engine never reports less pressure than the platform.
    void report_platform_level(PressureLevel level) noexcept;

    /// Recompute from `budgets` and broadcast if the level changed. Returns the level in force.
    PressureLevel evaluate(const BudgetTree& budgets) noexcept;

    [[nodiscard]] PressureLevel level() const noexcept { return level_; }
    [[nodiscard]] u64 transition_count() const noexcept { return sequence_; }

    /// The most recent transitions, oldest first. Returns how many were written.
    u32 history(PressureTransition* out, u32 capacity) const noexcept;

    /// Force a level and broadcast it. For a test, and for a platform whose notification is the
    /// only signal available; `evaluate()` will override it on the next call.
    void force(PressureLevel level, MemoryDomain cause) noexcept;

    void reset() noexcept;

private:
    [[nodiscard]] PressureLevel level_for(f64 utilisation) const noexcept;
    void broadcast(PressureLevel next, MemoryDomain cause, f64 utilisation) noexcept;

    PressureThresholds thresholds_;
    PressureResponder* responders_ = nullptr;
    u32 subscriber_count_ = 0;
    PressureLevel level_ = PressureLevel::Normal;
    PressureLevel platform_level_ = PressureLevel::Normal;
    u64 sequence_ = 0;
    u32 history_count_ = 0;
    u32 history_next_ = 0;
    PressureTransition history_[kPressureHistoryDepth] = {};
};

/// The process's pressure monitor. One, for the same reason there is one budget tree.
[[nodiscard]] PressureMonitor& default_pressure_monitor() noexcept;

/// Recompute pressure from the default budget tree and broadcast. What a frame calls once.
PressureLevel update_memory_pressure() noexcept;

}  // namespace cy
