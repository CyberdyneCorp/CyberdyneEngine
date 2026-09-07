#pragma once
// Staged, atomically-published cell activation, and the structured events it emits. Task 3.5.
//
// `world-partition-and-streaming` — "Activation is staged and published atomically": preparation
// happens in PRIVATE STAGING — allocating chunks, decoding component blocks, building physics
// batches, resolving intra-cell references — "without any of it being observable", and then
// publishes ATOMICALLY. "Systems SHALL observe a cell as either inactive or fully activated, never
// partially." Preparation may span frames; while it does, the cell's HLOD proxy stays visible, so
// the transition is not visible as absence. Deactivation is the same in reverse: withdraw
// atomically, then release incrementally.
//
// --- WHY PUBLICATION CANNOT BE "INSTANTIATE AS YOU GO" ------------------------------------------
//
// `ecs::World::instantiate()` takes one archetype block, and a cell holds several. Publishing block
// by block over several frames would leave a cell half in the world for as long as it took, and
// every query in between would see half a city. So `publish()` instantiates every ready block in
// one call, and if any instantiation fails it DESTROYS the entities it already created and reports
// the failure — the cell stays inactive rather than becoming partially active. That rollback is the
// whole reason each staged block keeps its own entity list rather than a count.
//
// --- EVENTS ARE QUEUED, NEVER CALLED ------------------------------------------------------------
//
// "Streaming SHALL NOT invoke arbitrary subsystem callbacks during activation, since re-entrant
// callbacks during streaming are a reliable source of ordering bugs." So a transition appends a
// record to a queue and consumers drain it at their own defined points, in a declared order. A
// consumer that reacts to activation by requesting another cell gets its request QUEUED — there is
// no callback for it to be re-entrant from.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/entity.h>
#include <cy/world/cell.h>
#include <cy/world/layers.h>

namespace cy::ecs {
class World;
}  // namespace cy::ecs

namespace cy::world {

class PersistenceOverlay;
struct ComponentOverride;

/// How far preparation has got. Publication happens only from `Ready`.
enum class StagingPhase : u8 {
    /// Nothing staged.
    Idle = 0,
    /// Component columns are being copied out of the cooked cell into private staging.
    Decoding,
    /// The persistence overlay is being applied to the staged columns. `world-partition-and-
    /// streaming`: applying an overlay happens DURING activation, "rather than by instantiating
    /// authored content and then correcting it".
    ApplyingOverlay,
    /// Subsystem payloads — physics batches, navigation tiles, GPU scene data — are being built.
    BuildingPayloads,
    /// Everything is prepared. The next `publish()` is one atomic step.
    Ready,
    /// Published into the ECS world.
    Published,
};

[[nodiscard]] const char* staging_phase_name(StagingPhase phase) noexcept;

/// A cell lifecycle event. Structured, queued, and consumed at defined points.
enum class CellEventKind : u8 {
    Resident = 0,
    Activated,
    Deactivated,
    Evicted,
    /// The channels a cell holds changed without it being reloaded — a source's mask gained
    /// physics, so the physics payload streamed for a cell that was already resident.
    ChannelsChanged,
    /// Preparation or publication failed. Carries the cell so a diagnostic can name it.
    Failed,
};

[[nodiscard]] const char* cell_event_kind_name(CellEventKind kind) noexcept;

struct CellEvent {
    CellEventKind kind = CellEventKind::Resident;
    CellId cell;
    ChannelMask channels;
    /// Monotonic, world-wide. Two consumers reading the same queue at different points agree about
    /// order because they agree about this number.
    u64 sequence = 0;
};

/// The queue subsystems consume cell transitions from, and the declared order they do it in.
///
/// A consumer registers with an ORDER key; `drain()` for a consumer returns every event it has not
/// seen. Ordering between consumers is the caller's declaration, not an accident of registration —
/// navigation, audio, illumination and gameplay all consume activation, and "in a declared order"
/// is a requirement rather than a hope.
class CellEventQueue {
public:
    explicit CellEventQueue(Allocator& allocator) noexcept;

    using ConsumerId = u32;

    /// Register a consumer. `order` is what the queue sorts consumers by when it reports them;
    /// lower runs first. Two consumers may share an order, and then their relative order is their
    /// registration order.
    [[nodiscard]] Expected<ConsumerId, Error> add_consumer(const char* name, u32 order) noexcept;

    [[nodiscard]] Status emit(const CellEvent& event) noexcept;

    /// Everything this consumer has not yet seen, appended to `out` in emission order.
    [[nodiscard]] Status drain(ConsumerId consumer, Array<CellEvent>& out) noexcept;

    /// Consumers in their declared order. What a dispatcher iterates.
    struct Consumer {
        ConsumerId id = 0;
        const char* name = "";
        u32 order = 0;
        u64 seen = 0;
    };
    [[nodiscard]] Span<const Consumer> consumers() const noexcept { return consumers_.span(); }

    /// Drop events every consumer has seen. Called once per tick; without it the queue is a log.
    void compact() noexcept;

    [[nodiscard]] usize pending() const noexcept { return events_.size(); }
    [[nodiscard]] u64 next_sequence() const noexcept { return next_sequence_; }

private:
    Array<CellEvent> events_;
    Array<Consumer> consumers_;
    u64 next_sequence_ = 1;
    /// The sequence number of `events_[0]`. Compaction moves it forward.
    u64 first_sequence_ = 1;
    ConsumerId next_consumer_ = 1;
};

/// One cell's private staging area and its published rows.
///
/// It owns COPIES of the cooked columns, because the persistence overlay patches them and a cooked
/// cell is immutable — "authored cells + persistence overlay = current world", and the authored
/// cell is not the thing that changes.
class CellActivation {
public:
    CellActivation(Allocator& allocator, const CookedCell& cell) noexcept;

    ~CellActivation();

    CellActivation(const CellActivation&) = delete;
    CellActivation& operator=(const CellActivation&) = delete;
    CellActivation(CellActivation&&) noexcept = default;
    CellActivation& operator=(CellActivation&&) noexcept = default;

    [[nodiscard]] CellId cell() const noexcept { return id_; }
    [[nodiscard]] StagingPhase phase() const noexcept { return phase_; }
    [[nodiscard]] bool ready() const noexcept { return phase_ == StagingPhase::Ready; }
    [[nodiscard]] bool published() const noexcept { return phase_ == StagingPhase::Published; }

    /// Do up to `budget` nanoseconds of preparation. Returns the phase reached. Preparation is
    /// resumable: calling it again continues from where it stopped, which is what "preparation MAY
    /// span multiple frames" means in code.
    [[nodiscard]] Expected<StagingPhase, Error> advance(const CookedCell& source,
                                                        const LayerTable& layers,
                                                        const PersistenceOverlay* overlay,
                                                        Nanoseconds budget) noexcept;

    /// Publish every prepared block whose layer is activated. ATOMIC: on any failure the entities
    /// created by this call are destroyed and the cell stays unpublished.
    [[nodiscard]] Status publish(ecs::World& world, const LayerTable& layers) noexcept;

    /// Withdraw atomically. The staged data is kept, so the cell can be republished without being
    /// re-prepared — which is what makes a layer switch cheap.
    [[nodiscard]] Status withdraw(ecs::World& world) noexcept;

    /// Publish or withdraw the blocks of ONE layer on an already-published cell. This is the
    /// scenario switch: one operation over whole blocks, not twenty thousand property changes.
    [[nodiscard]] Status publish_layer(ecs::World& world, LayerId layer) noexcept;
    [[nodiscard]] Status withdraw_layer(ecs::World& world, LayerId layer) noexcept;

    /// Give the staging memory back. Incremental release, after an atomic withdrawal.
    void release() noexcept;

    /// The entities this cell currently has in the ECS world.
    [[nodiscard]] Span<const ecs::Entity> entities() const noexcept { return published_.span(); }
    [[nodiscard]] u32 published_rows() const noexcept {
        return static_cast<u32>(published_.size());
    }

    /// How long the last `publish()` took. Compared against the cook's estimate, because
    /// `world-partition-and-streaming` requires estimates to be VALIDATED against measured cost and
    /// significant divergence reported.
    [[nodiscard]] Nanoseconds measured_activation_time() const noexcept { return measured_; }

    /// Bytes held in private staging. Entity memory, for the budget and the eviction report.
    [[nodiscard]] u64 staged_bytes() const noexcept { return staged_bytes_; }

    /// Budget spent preparing this cell so far, in the cost model's units. The caller charges the
    /// DIFFERENCE across an `advance()` against its per-tick budget, which is how a budget can be
    /// enforced without sampling a clock — see the note in cell.h.
    [[nodiscard]] Nanoseconds spent_preparing() const noexcept { return spent_; }

private:
    /// One staged block: the cooked block's columns, copied and patched.
    ///
    /// It owns its OWN entity list rather than a range of a shared one, because a layer switch
    /// publishes and withdraws blocks individually and a shared list would have to be re-indexed on
    /// every switch — which is the per-entity work the layer design exists to avoid.
    struct StagedBlock {
        LayerId layer;
        Array<ecs::ComponentTypeId> components;
        Array<Array<u8>> columns;
        Array<PersistentId> ids;
        Array<ecs::Entity> entities;
        u32 count = 0;
        bool live = false;

        explicit StagedBlock(Allocator& allocator) noexcept
            : components(allocator), columns(allocator), ids(allocator), entities(allocator) {}
    };

    [[nodiscard]] Status decode_one(const CookedCell& source, const LayerTable& layers,
                                    usize index) noexcept;
    [[nodiscard]] Status stage_block(const CookedBlock& source) noexcept;
    [[nodiscard]] Status apply_overlay(const PersistenceOverlay& overlay) noexcept;
    [[nodiscard]] Status compact_removed(StagedBlock& block,
                                         const PersistenceOverlay& overlay) noexcept;
    [[nodiscard]] Status apply_override(StagedBlock& block, const ComponentOverride& record,
                                        const PersistenceOverlay& overlay) noexcept;
    [[nodiscard]] Status instantiate_block(ecs::World& world, StagedBlock& block) noexcept;
    [[nodiscard]] Status destroy_block(ecs::World& world, StagedBlock& block) noexcept;
    /// Refresh the flattened view `entities()` returns.
    [[nodiscard]] Status reindex() noexcept;

    Allocator* allocator_;
    CellId id_;
    StagingPhase phase_ = StagingPhase::Idle;
    /// How many of the source's blocks have been decoded. The resumption point.
    usize decoded_ = 0;
    Array<StagedBlock> staged_;
    /// A flattened cache of every live block's entities. Rebuilt by `reindex()`.
    Array<ecs::Entity> published_;
    Array<const void*> column_pointers_;
    Nanoseconds measured_ = 0;
    Nanoseconds spent_ = 0;
    /// The staging bytes this cell holds, for the memory budget and for the eviction report.
    u64 staged_bytes_ = 0;
};

}  // namespace cy::world
