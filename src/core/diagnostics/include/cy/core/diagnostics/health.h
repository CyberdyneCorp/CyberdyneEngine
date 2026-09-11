#pragma once
// The health model: one place to look when a session behaves badly.
//
// `diagnostics-profiling-and-crash` — "Assertions and health":
//
//   The engine SHALL maintain a **health model** with severity levels, aggregating conditions that
//   subsystems report: frame and tick budget overruns, GPU overruns, memory pressure, streaming
//   deadline misses, packet loss, rollback frequency, task starvation, and determinism divergence.
//
//   Health SHALL be observable in one place, in every build type, so that "something is wrong" is
//   answerable without attaching a tool.
//
// WHY IT IS NOT A LOG LEVEL AND NOT A COUNTER. A log says a thing happened; a counter says how
// often. Neither answers "what is wrong NOW, and since when", which is the question a player's
// report and a server's operator both ask. A condition here is a LEVEL that persists until the
// subsystem says otherwise, with the instant it entered that level, so the answer is one read.
//
// THREE PROPERTIES THE IMPLEMENTATION IS BUILT FOR.
//
//   * EVERY BUILD TYPE. There is no `#if` around this file. A shipping build reports health, which
//     is the build where "something is wrong" is hardest to answer any other way.
//   * READABLE FROM A SIGNAL HANDLER. `health_active()` reads a fixed array of atomics and takes no
//     lock, so the crash artefact carries the health state the process died in.
//   * A REPORT THAT CHANGES NOTHING COSTS ONE COMPARE. A subsystem reporting `Nominal` every frame
//     for a condition already `Nominal` performs one relaxed load and returns; only a TRANSITION
//     writes, emits onto the trace, and notifies the observer.
//
// The observer is how `capture.h` turns a transition to Critical into a capture artefact, which is
// the "triggerable automatically by declared conditions" half of the rolling-buffer requirement.

#include <cy/core/diagnostics/prelude.h>

namespace cy::diag {

/// The conditions the specification lists, plus the three the engine's own capabilities already
/// report through other means and had nowhere to aggregate. Fixed and small: a condition is a
/// vocabulary entry that a viewer, a crash artefact and a telemetry export all share, so adding one
/// is a deliberate act rather than a string somebody passed.
enum class HealthCondition : u8 {
    FrameBudgetOverrun = 0,
    TickBudgetOverrun = 1,
    GpuBudgetOverrun = 2,
    MemoryPressure = 3,
    StreamingDeadlineMiss = 4,
    PacketLoss = 5,
    RollbackFrequency = 6,
    TaskStarvation = 7,
    DeterminismDivergence = 8,
    SaveFailure = 9,
    DiagnosticsLoss = 10,
};

inline constexpr u32 kHealthConditionCount = 11;

/// Three levels, because a fourth would be a judgement rather than a state. `Degraded` is "worse
/// than it should be and still working"; `Critical` is "the session is not doing what it promised".
enum class HealthSeverity : u8 {
    Nominal = 0,
    Degraded = 1,
    Critical = 2,
};

struct HealthEntry {
    HealthSeverity severity = HealthSeverity::Nominal;
    /// The monotonic instant this condition entered `severity`. Zero while it has never left
    /// Nominal — "since when" is the half of the answer a level alone does not carry.
    u64 since_ns = 0;
    /// How many times the level changed. A condition flapping between Degraded and Nominal is a
    /// different problem from one that went Critical and stayed, and the count is what separates
    /// them.
    u64 transitions = 0;
    /// How many times a subsystem reported it at all, at any level.
    u64 reports = 0;
    /// The subsystem's own number at the last report: the overrun in nanoseconds, the bytes over
    /// budget, the packets lost. Uninterpreted here on purpose.
    u64 detail = 0;
};

struct HealthSnapshot {
    HealthEntry conditions[kHealthConditionCount] = {};
    HealthSeverity worst = HealthSeverity::Nominal;
    u32 active = 0;  // conditions above Nominal
    u64 captured_ns = 0;
};

/// Report a condition's current level. Cheap and idempotent: reporting the level a condition
/// already holds updates `detail` and `reports` and does nothing else.
void health_report(HealthCondition condition, HealthSeverity severity, u64 detail) noexcept;

/// Everything, in one read. What "observable in one place" means.
HealthSnapshot health_snapshot() noexcept;

/// The worst level any condition currently holds. One relaxed load per condition.
HealthSeverity health_worst() noexcept;

/// Fill `out` with every condition above Nominal, newest transition last. Async-signal-safe: a
/// fixed array, relaxed loads, no lock and no allocation, so the crash handler may call it.
/// Returns how many were written.
u32 health_active(HealthCondition* out, HealthSeverity* severity, u64* since_ns,
                  u32 capacity) noexcept;

/// Return every condition to Nominal and forget its history. For tests and for a session boundary;
/// a running session has no reason to.
void health_reset() noexcept;

const char* health_condition_name(HealthCondition condition) noexcept;
const char* health_severity_name(HealthSeverity severity) noexcept;

/// Called on a TRANSITION only, on the thread that reported it, after the model is updated. This is
/// the seam `capture.h` uses to freeze a window around a condition going Critical; it is a single
/// function pointer rather than a list because two subscribers would be two policies.
using HealthObserver = void (*)(void* user, HealthCondition condition, HealthSeverity from,
                                HealthSeverity to) noexcept;

/// Install an observer and return the previous one, so a caller can chain or restore it.
HealthObserver set_health_observer(HealthObserver observer, void* user) noexcept;

}  // namespace cy::diag
