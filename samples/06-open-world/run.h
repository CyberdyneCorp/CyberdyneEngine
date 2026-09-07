#pragma once
// samples/06-open-world — the run: the world, the route, the pages, and the save.
//
// ONE SESSION OWNS FIVE SUBSYSTEMS AND THE ORDER THEY ARE DESTROYED IN IS PART OF THE ARTEFACT.
// M6 creates and destroys worlds continuously, and the defect M5.5's gate found — a worker still
// writing into storage a destructor had already freed — is the failure mode every one of these has.
// So `Session`'s members are declared in the order they must be torn down (the ECS world outlives
// the streaming that publishes into it, the residency server outlives the texture system that
// reports to it), `stop()` withdraws before it frees, and `--abort-at <tick>` exists so that the
// driver can tear a session down IN MID-FLIGHT rather than only at the end of a settled route.
//
// WHAT THE ROUTE MEASURES, AND WHAT IT ONLY REPORTS. The modelled work per tick is deterministic —
// it is what the streaming budget is spent against — and the run FAILS when a tick exceeds it. The
// CPU time per tick is what a player feels minus what the rest of the machine is doing: it is
// measured against this thread's own CPU clock, for the reason tests/harness/src/budget.cpp gives,
// reported as a distribution, and checked against a threshold with an order of magnitude of
// headroom. A wall clock here would make the artefact fail on a busy build machine and pass on an
// idle one, which is a measurement of the machine.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/world.h>
#include <cy/save/archive.h>
#include <cy/save/storage.h>
#include <cy/servers/render/virtual_texturing/system.h>
#include <cy/servers/residency/server.h>
#include <cy/world/streaming.h>

#include "content.h"

namespace cy::sample::openworld {

/// How the route is walked. Fixed in space: `ticks` decides how far along it the run gets, never
/// how fast the traveller moves, so a short run and a long one traverse the same cells in the same
/// order and their reports are comparable.
struct RouteOptions {
    u32 ticks = 6000;
    /// Metres per 60 Hz tick. 1.2 is 72 m/s — a vehicle, not a walk.
    f32 metres_per_tick = 1.2F;
    f32 radius = 320.0F;
    u32 production_workers = 2;
    /// Tear the session down at this tick, with cells preparing and pages in production. Zero runs
    /// the route to its end.
    u32 abort_at = 0;
    /// The ceiling one tick's measured CPU time may reach before the run reports a hitch.
    f64 hitch_threshold_ms = 8.0;
};

/// What one traversal did. Every number is printed, and the driver reads the printed form.
struct Telemetry {
    // The route.
    u32 ticks = 0;
    f64 metres_travelled = 0.0;
    f64 metres_per_second = 0.0;
    u64 cells_activated = 0;
    u64 cells_deactivated = 0;
    u64 cells_evicted = 0;
    u64 cells_made_resident = 0;
    u64 io_bytes = 0;
    /// The largest amount by which the staged-row budget was exceeded after eviction. Non-zero is a
    /// reported shortfall — everything left was required — and never a silent overrun.
    u64 memory_shortfall_peak = 0;
    u32 published_peak = 0;
    u32 ticks_over_budget = 0;
    Nanoseconds worst_modelled_tick = 0;
    f32 worst_cost_divergence = 0.0F;
    f64 median_tick_us = 0.0;
    f64 p99_tick_us = 0.0;
    f64 worst_tick_us = 0.0;
    u32 hitches = 0;

    // The pages.
    u64 feedback_recorded = 0;
    u64 feedback_dropped = 0;
    u64 page_requests = 0;
    u64 pages_produced = 0;
    u64 page_evictions = 0;
    u64 resident_tiles_peak = 0;
    u64 missing_samples = 0;
    f64 fallback_rate = 0.0;

    // Residency against activation: cells whose bytes are in memory with nothing simulating.
    u32 resident_not_activated = 0;

    // The world changing under the player.
    u32 landmarks_passed = 0;
    u32 entities_removed = 0;
    u32 entities_modified = 0;
    u32 overlay_cells = 0;
};

/// What a resumed run found. The claim "it resumes in the same state", as numbers.
struct ResumeReport {
    u32 generation = 0;
    u64 saved_content_digest = 0;
    u64 installed_content_digest = 0;
    u32 tick = 0;
    u32 regions = 0;
    u32 cells_reactivated = 0;
    u32 removals_honoured = 0;
    u32 overrides_matched = 0;
    u32 mismatches = 0;
    u32 landmarks_passed = 0;
};

/// The world, the streaming, the paging and the save, for one run of the sample.
class Session {
public:
    Session(Allocator& allocator, const WorldContent& content) noexcept;
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    /// Register the components, cook every cell into the index, and configure the texture caches.
    [[nodiscard]] Status start(const RouteOptions& options) noexcept;

    /// Walk the route. Streams cells, pages textures, and records what the player changed.
    [[nodiscard]] Status traverse(const RouteOptions& options, Telemetry& out) noexcept;

    /// Write the overlay to a save archive under `directory`, atomically.
    [[nodiscard]] Expected<u32, Error> checkpoint(const char* directory,
                                                  const Telemetry& telemetry) noexcept;

    /// Load a save, reactivate exactly the cells it holds state for, and check the world against
    /// it.
    [[nodiscard]] Status resume(const char* directory, ResumeReport& out) noexcept;

    /// Withdraw every published cell and quiesce every worker, in that order. Called by the
    /// destructor; callable directly, and safe with cells preparing and pages in production.
    void stop() noexcept;

private:
    [[nodiscard]] Status cook_world() noexcept;
    [[nodiscard]] Status configure_texturing(const RouteOptions& options) noexcept;
    [[nodiscard]] world::WorldPosition along_route(const RouteOptions& options,
                                                   u32 tick) const noexcept;
    void page_textures(const RouteOptions& options, world::CellCoord here, u32 tick) noexcept;
    /// Ask for a region the route never reaches, with `activates` false, and count what came
    /// resident without anything simulating. The M6 exit criterion "a test holds bytes resident
    /// with simulation off", reached from the gameplay API rather than from a test.
    [[nodiscard]] u32 probe_prefetch() noexcept;
    /// Copy the world's overlay into a save overlay, and back. See the note in run.cpp: there are
    /// two overlay models in this tree and this conversion is where the sample pays for that.
    [[nodiscard]] Status to_save_overlay(save::Overlay& out) const noexcept;
    [[nodiscard]] Status from_save_overlay(const save::Overlay& saved,
                                           ResumeReport& report) noexcept;
    [[nodiscard]] Status verify_region(const save::Region& region, ResumeReport& report) noexcept;
    [[nodiscard]] Status pass_landmark(const Landmark& landmark, Telemetry& telemetry) noexcept;
    [[nodiscard]] u32 count_resident_not_activated() const noexcept;

    Allocator* allocator_;
    const WorldContent* content_;
    Components components_;

    // DECLARATION ORDER IS TEARDOWN ORDER, REVERSED. `ecs_` is first so it is destroyed last: the
    // streaming below publishes entities into it and withdraws them in its own destructor.
    ecs::World ecs_;
    world::HierarchicalGrid grid_;
    world::PersistenceOverlay overlay_;
    world::WorldStreaming streaming_;
    // The residency server is declared before the texture system for the same reason: the system
    // reports admissions and releases to the server, so the server must outlive it.
    residency::ResidencyServer residency_;
    render::vt::VirtualTextureSystem texturing_;

    world::SourceId traveller_ = world::kInvalidSource;
    u64 landmark_seen_ = 0;
    bool started_ = false;
};

/// Print a telemetry block. The run is the evidence, so this is the evidence.
void print_telemetry(const WorldContent& content, const Telemetry& telemetry);
void print_resume(const ResumeReport& report);

}  // namespace cy::sample::openworld
