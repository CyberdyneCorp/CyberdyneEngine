#pragma once
// The ECS's diagnostics, on the M0 trace. Task 2.12.
//
// `ecs-core` — "ECS diagnostics": development builds expose archetype and chunk counts with fill
// ratios, per-system execution time and entity counts, query match statistics, structural-change
// counts per frame, and detection of archetype thrash.
//
// ONE TRACE, MANY PRODUCERS. Everything here goes onto `<cy/core/diagnostics/trace.h>` — the same
// timeline the memory layer, the job system and the asset layer emit onto. A subsystem with a
// counter format of its own is the defect that requirement exists to prevent, and the reason there
// is nothing here to stop it is that everything it could want is already there.
//
// `cy::core-diagnostics` is a PRIVATE dependency of this module (CMakeLists.txt). Including a
// component header must not drag the trace in behind it, so this header names no diagnostics type:
// the report is a call, and the report's *shape* is the struct below.

#include <cy/core/base/types.h>
#include <cy/ecs/query.h>
#include <cy/ecs/system.h>
#include <cy/ecs/world.h>

namespace cy::ecs {

/// When an entity is moving between archetypes often enough to be worth naming.
struct ThrashPolicy {
    /// Transitions by one entity within one window before it is reported. Eight is two components
    /// toggled every frame at 60 Hz sampled once a second, which is the shape the requirement's
    /// scenario describes and well above anything an ordinary spawn or state change produces.
    u32 transitions_per_window = 8;
    /// The window, in nanoseconds. A second, as the specification states the threshold in.
    u64 window_ns = 1'000'000'000ULL;
};

/// What the last thrash check found. Empty when nothing crossed the threshold.
struct ThrashReport {
    Entity entity;
    ComponentTypeId component = kInvalidComponent;
    const char* component_name = "";
    u32 transitions = 0;
    [[nodiscard]] bool detected() const noexcept { return transitions != 0; }
};

/// The ECS's reporter. Holds no state a world does not already have except the thrash window.
class EcsDiagnostics {
public:
    explicit EcsDiagnostics(World& world, const ThrashPolicy& policy = {}) noexcept
        : world_(&world), policy_(policy) {}

    /// Emit the world's shape: archetype and chunk counts, fill ratio, committed bytes, and the
    /// structural counters. Verbose channel — a counter snapshot is background detail, and under
    /// buffer pressure it is exactly what should be dropped before a tick boundary is.
    void report_world() noexcept;

    /// Emit one schedule's per-system execution times and command counts.
    void report_systems(const Schedule& schedule) noexcept;

    /// Emit one query's match statistics: archetypes matched, chunks visited and skipped, entities
    /// seen. What answers "why is this query slow" with numbers rather than a guess.
    void report_query(const char* name, const QueryStats& stats) noexcept;

    /// Check the thrash window. Call once per frame with the monotonic clock; the window is closed
    /// and the counters reset when `policy.window_ns` has passed. Returns what it found, and emits
    /// it — with the component named and the advice `ecs-core` asks for — when it found something.
    ThrashReport check_thrash(u64 now_ns) noexcept;

    [[nodiscard]] const ThrashReport& last_thrash() const noexcept { return last_; }

private:
    World* world_;
    ThrashPolicy policy_;
    u64 window_started_ns_ = 0;
    ThrashReport last_;
};

}  // namespace cy::ecs
