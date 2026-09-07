#pragma once
// `WorldStreaming` — the thing that ties partitioning, sources, channels, budgets, activation,
// layers, HLOD and the persistence overlay into one tick. Tasks 3.4, 3.5 and 3.8.
//
// --- WHAT ONE TICK DOES, AND WHY IN THIS ORDER --------------------------------------------------
//
//   1. Deferred requests made during the last tick's event consumption are taken up. Nothing is
//      processed re-entrantly: "WHEN a consumer reacts to activation by requesting another cell,
//      THEN the request SHALL be queued, not processed re-entrantly within the activation."
//   2. Every source states which cells it requires, at which priority, with which channels and by
//      when. Sources combine — a cell required by any source is required.
//   3. Hard dependencies are closed: a required cell's `RequireLoaded` targets are required too.
//   4. Work is ordered — critical, gameplay, visible, predicted, background, then by priority, then
//      by deadline, then by cell identifier so two machines agree.
//   5. The I/O budget buys residency, one channel delta at a time. A cell that is already resident
//      and has gained a channel pays for that channel and is NOT reloaded.
//   6. The activation budget buys preparation and publication. Preparation spans frames; each
//      publication is atomic.
//   7. Cells no longer required are withdrawn, then evicted under memory pressure, lowest priority
//      first, and the shortfall is reported.
//   8. HLOD visibility is recomputed — in this tick, so the proxy is replaced in the same tick the
//      cells are published.
//
// --- THE M6 EXIT CRITERION THIS FILE IS MEASURED AGAINST ----------------------------------------
//
// "Continuous traversal holds the frame budget with no hitch above threshold, measured over a fixed
// route." `tick()` therefore never does unbounded work: every loop is bounded by a budget, and
// `TickReport` says what it spent so a route test can MEASURE rather than assert. The one loop that
// is proportional to content is the requirement gather in step 2, which is bounded by the sources'
// radii and not by the size of the world.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/world/activation.h>
#include <cy/world/cell.h>
#include <cy/world/hlod.h>
#include <cy/world/layers.h>
#include <cy/world/overlay.h>
#include <cy/world/partition.h>
#include <cy/world/sources.h>

namespace cy::ecs {
class World;
}  // namespace cy::ecs

namespace cy::world {

/// The explicit budgets. `world-partition-and-streaming`: "The system SHALL hold explicit budgets:
/// I/O bandwidth, entity memory, and a per-frame ACTIVATION TIME BUDGET, and SHALL coordinate with
/// the asset system's residency budget rather than maintaining a competing one."
struct StreamingBudget {
    /// Bytes of cell data that may be brought resident in one tick.
    u64 io_bytes_per_tick = 4ull * 1024 * 1024;
    /// The ceiling on staged entity memory. Exceeding it evicts, lowest priority first.
    u64 entity_memory_bytes = 64ull * 1024 * 1024;
    /// How much preparation and publication one tick may do, charged against the cost model's
    /// estimate rather than a clock — see cell.h.
    Nanoseconds activation_time_per_tick = 2'000'000;  // 2 ms
};

/// What one tick did. Everything a route measurement needs, and everything the profiler view in
/// `world-partition-and-streaming`'s "Streaming diagnostics" asks for.
struct TickReport {
    u32 cells_requested = 0;
    u32 cells_made_resident = 0;
    u32 cells_activated = 0;
    u32 cells_deactivated = 0;
    u32 cells_evicted = 0;
    u32 channel_deltas = 0;
    u64 io_bytes_spent = 0;
    Nanoseconds activation_time_spent = 0;
    /// Requests that did not fit this tick. Queue depth.
    u32 deferred = 0;
    /// Bytes by which the memory budget was exceeded after eviction. Non-zero is the reported
    /// shortfall, not a silent overrun.
    u64 memory_shortfall = 0;
    /// The largest divergence this tick between a cell's estimated and measured activation time,
    /// as a ratio. Reported so the cost model does not silently drift.
    f32 worst_cost_divergence = 0.0f;
    CellId worst_cost_divergence_cell;
};

/// Cells by state, and memory by category.
struct StreamingStats {
    u32 by_state[7] = {};
    u64 staged_bytes = 0;
    u64 resident_io_bytes = 0;
    u32 published_entities = 0;
    u64 total_evictions = 0;
    u64 total_activations = 0;
};

/// Why a cell is in the state it is in. `world-partition-and-streaming`, "Streaming diagnostics":
/// "For any cell it SHALL answer WHY it is in its current state: which sources require it, its
/// computed priority, its predicted relevance, and what is blocking a transition."
struct CellExplanation {
    bool known = false;
    CellState state = CellState::Unloaded;
    StagingPhase phase = StagingPhase::Idle;
    bool requested = false;
    bool wanted_active = false;
    u32 requiring_sources = 0;
    /// The source that set the winning priority.
    SourceId leading_source = kInvalidSource;
    f32 priority = 0.0f;
    RequestClass klass = RequestClass::Background;
    Nanoseconds time_until_needed = 0;
    ChannelMask required_channels;
    ChannelMask resident_channels;
    /// "unrequested", "queued behind higher priority", "budget-blocked", "preparing" or "" when
    /// nothing is blocking. Never null.
    const char* blocking = "";
};

/// A cell's hard dependency closure, and what activating it forces resident.
struct DependencyReport {
    CellId cell;
    u32 forced_cells = 0;
    u64 forced_bytes = 0;
    bool over_threshold = false;
    /// The first link of the chain responsible, so the report is actionable rather than a number.
    CellId first_link;
};

/// The spatial and persistence layer at runtime. Never called `World` — see the module's
/// CMakeLists.txt for why that is the first requirement rather than a naming preference.
class WorldStreaming {
public:
    /// `partitioner` and `ecs` must outlive this object. The overlay is optional: a world with no
    /// save yet has none, and the activation path is the same either way.
    WorldStreaming(Allocator& allocator, const Partitioner& partitioner, ecs::World& ecs) noexcept;
    ~WorldStreaming();

    WorldStreaming(const WorldStreaming&) = delete;
    WorldStreaming& operator=(const WorldStreaming&) = delete;
    WorldStreaming(WorldStreaming&&) = delete;
    WorldStreaming& operator=(WorldStreaming&&) = delete;

    /// Withdraw every published cell and release every staging area. Called by the destructor, and
    /// callable directly — a caller that is about to destroy the ECS world should call it first.
    ///
    /// IT IS SAFE MID-ACTIVATION. A cell being prepared has published nothing, so tearing down
    /// during preparation withdraws nothing and frees staging; a cell published in the same tick is
    /// withdrawn whole. That is what makes "create and destroy worlds continuously" a supported
    /// operation rather than a race waiting to be found.
    void shutdown() noexcept;

    void set_profile(WorldProfile profile) noexcept;
    [[nodiscard]] WorldProfile profile() const noexcept { return profile_; }

    /// Attach a persistence overlay. It is applied during activation, so attaching it after cells
    /// are published affects the next activation and not the current one.
    void set_overlay(PersistenceOverlay* overlay) noexcept { overlay_ = overlay; }
    [[nodiscard]] PersistenceOverlay* overlay() const noexcept { return overlay_; }

    /// Add a cooked cell to the index. The cell is `Metadata` from here: its bounds, cost and
    /// dependencies are known and nothing has been loaded.
    [[nodiscard]] Status add_cell(CookedCell&& cell) noexcept;
    [[nodiscard]] bool has_cell(CellId cell) const noexcept;
    [[nodiscard]] usize cell_count() const noexcept { return cells_.size(); }

    [[nodiscard]] SourceRegistry& sources() noexcept { return sources_; }
    [[nodiscard]] const SourceRegistry& sources() const noexcept { return sources_; }
    [[nodiscard]] LayerTable& layers() noexcept { return layers_; }
    [[nodiscard]] const LayerTable& layers() const noexcept { return layers_; }
    [[nodiscard]] HlodRegistry& hlod() noexcept { return hlod_; }
    [[nodiscard]] const HlodRegistry& hlod() const noexcept { return hlod_; }
    [[nodiscard]] CellEventQueue& events() noexcept { return events_; }
    [[nodiscard]] DynamicIndex& dynamic_index() noexcept { return dynamic_; }
    [[nodiscard]] RepresentationTable& representations() noexcept { return representations_; }

    /// Set a layer's state, and apply it to every published cell as ONE operation over whole
    /// blocks. This is the scenario switch.
    [[nodiscard]] Status set_layer_state(LayerId layer, LayerState state) noexcept;

    /// Ask for a region around a point to be prefetched, at a priority, without naming a cell. The
    /// gameplay API: "requesting a prefetch around a location with a priority".
    ///
    /// It registers a source, so it combines with every other source by the ordinary rule rather
    /// than by a second mechanism. The returned identifier is how it is cancelled.
    [[nodiscard]] Expected<SourceId, Error> prefetch(const WorldPosition& centre, f32 radius,
                                                     RequestClass klass,
                                                     ChannelMask channels) noexcept;

    /// One tick. Bounded by `budget` in every loop.
    [[nodiscard]] Expected<TickReport, Error> tick(const StreamingBudget& budget) noexcept;

    /// Request a cell directly. For tools and tests; the ordinary path is a source. Queued rather
    /// than applied, so that calling it from inside event consumption is not re-entrant.
    [[nodiscard]] Status request(CellId cell, ChannelMask channels, bool activate) noexcept;

    [[nodiscard]] CellState state_of(CellId cell) const noexcept;
    [[nodiscard]] CellExplanation explain(CellId cell) const noexcept;
    [[nodiscard]] StreamingStats stats() const noexcept;

    /// The entities a published cell has in the ECS world. Empty for a cell that is not published.
    [[nodiscard]] Span<const ecs::Entity> entities_of(CellId cell) const noexcept;

    /// Compute the hard dependency closure of every cell and report the ones over `limit`. Cook-
    /// time work, exposed at runtime because the editor runs the same validation.
    [[nodiscard]] Status dependency_report(u32 cell_limit, Array<DependencyReport>& out) noexcept;

private:
    /// One cell's index entry and everything the planner decided about it.
    struct CellRuntime {
        CookedCell cooked;
        CellActivation activation;
        CellState state = CellState::Metadata;
        ChannelMask resident_channels;
        ChannelMask required_channels;
        bool required = false;
        bool wanted_active = false;
        u32 requiring_sources = 0;
        SourceId leading_source = kInvalidSource;
        f32 priority = 0.0f;
        RequestClass klass = RequestClass::Background;
        Nanoseconds time_until_needed = 0;
        const char* blocking = "";
        u64 resident_bytes = 0;
        /// A direct request, from a tool or from a consumer reacting to an event. It persists until
        /// it is cleared, unlike a source's requirement, which is recomputed every tick.
        bool pinned = false;
        bool pinned_activate = false;
        ChannelMask pinned_channels;

        CellRuntime(Allocator& allocator, CookedCell&& source) noexcept;
    };

    /// A request made outside the tick — by a tool, or by a consumer reacting to an event. Applied
    /// at the start of the next tick, never where it was made.
    struct DeferredRequest {
        CellId cell;
        ChannelMask channels;
        bool activate = false;
    };

    [[nodiscard]] CellRuntime* find_cell(CellId cell) noexcept;
    [[nodiscard]] const CellRuntime* find_cell(CellId cell) const noexcept;
    [[nodiscard]] Status gather_requirements() noexcept;
    [[nodiscard]] Status close_hard_dependencies() noexcept;
    void order_work() noexcept;
    [[nodiscard]] Status spend_io(const StreamingBudget& budget, TickReport& report) noexcept;
    [[nodiscard]] Status spend_activation(const StreamingBudget& budget,
                                          TickReport& report) noexcept;
    [[nodiscard]] Status withdraw_unrequired(TickReport& report) noexcept;
    [[nodiscard]] Status evict(const StreamingBudget& budget, TickReport& report) noexcept;
    [[nodiscard]] Status refresh_hlod() noexcept;
    [[nodiscard]] Status emit(CellEventKind kind, const CellRuntime& cell) noexcept;
    /// The transitive `RequireLoaded` closure of one cell, and the bytes it forces resident.
    [[nodiscard]] Status close_dependencies(const CellRuntime& holder, Array<CellId>& closure,
                                            u64& bytes) noexcept;

    Allocator* allocator_;
    const Partitioner* partitioner_;
    ecs::World* ecs_;
    PersistenceOverlay* overlay_ = nullptr;
    WorldProfile profile_ = WorldProfile::Client;

    Array<CellRuntime> cells_;
    HashMap<CellId, usize> index_;
    SourceRegistry sources_;
    LayerTable layers_;
    HlodRegistry hlod_;
    CellEventQueue events_;
    DynamicIndex dynamic_;
    RepresentationTable representations_;

    Array<CellRequirement> requirements_;
    Array<DeferredRequest> deferred_;
    /// Indices into `cells_`, ordered for this tick. Reused rather than reallocated every tick.
    Array<u32> work_;
    u64 total_evictions_ = 0;
    u64 total_activations_ = 0;
};

}  // namespace cy::world
