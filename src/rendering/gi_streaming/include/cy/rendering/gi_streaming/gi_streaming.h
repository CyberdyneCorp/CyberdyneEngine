// SPDX-License-Identifier: MIT
#pragma once
// The GI's scene content as a cell channel: loaded with a world cell, evicted with it, a little at
// a time. Issue #35, the world half.
//
// `rendering-global-illumination` ("Scene representation": the GI scene is ingested and evicted
// per world cell) and `world-partition-and-streaming` ("Structured cell events consumed at defined
// points"): this class is a registered consumer of `world::CellEventQueue`, exactly as
// `foliage::FoliageStreaming` and `water::WaterStreaming` are. It drains the events the world
// already emits, asks a loader for what a cell that carries `world::Channel::Illumination` holds
// for the GI — its surface cards (emitters among them), its meshes' distance fields and its GI
// volumes — and hands it to `gi::IlluminationSystem` through the calls a cell already had:
// `ingest_cell` and `evict_cell`, `field().place` and `remove`, `acceleration().add_instance` and
// `remove_instance` for the hardware tier, and `set_volumes` under the streamed cells' own
// `GiVolumeOwner::Streaming`.
//
// ================================================================================================
// A CELL ARRIVES OVER SEVERAL FRAMES, AND NO FRAME PAYS FOR A WHOLE ONE
// ================================================================================================
//
// What a cell costs the GI is proportional to its cards and its fields: a page a card, a brick
// invalidation a field, a device upload of each. `tick` spends a budget (`GiStreamingBudget`) and
// carries the rest to the next tick: a cell's cards go in as CHUNKS — each its own GI scene cell,
// cut only between instances so an emitter is never split across two — and its fields a few at a
// time. A cell is evicted whole, at once, and what it released is charged to the tick's budget
// first, so a tick that takes a cell out brings less in. The device copies follow by what changed
// (`gi_gpu::GpuGiScene` uploads cards by revision and bricks by what moved), so the device half is
// incremental for the same reason.
//
// A cell that leaves before it has finished arriving is withdrawn from the queue and what part of
// it arrived is evicted; one that arrives again starts over. The GI the streamed cells give is the
// GI those cells give ingested at once, which `integration.render_gi_streaming` holds to a system
// that never streamed.
//
// ================================================================================================
// WHICH EVENTS
// ================================================================================================
//
// `Activated` brings a cell's GI content in and `Deactivated` or `Evicted` takes it out: the GI
// lights what the world shows, and a resident cell that is not active is bytes in memory and
// nothing a player sees. `ChannelsChanged` brings it in or takes it out as the cell gains or loses
// `Channel::Illumination` while active.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/gi_volume.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/raytracing/acceleration.h>
#include <cy/world/activation.h>
#include <cy/world/coordinates.h>

namespace cy::rendering::gi_streaming {

/// One mesh's distance field, placed by a cell. The field is borrowed: it outlives the cell's
/// residency (a cooked asset the loader keeps).
struct GiFieldPlacement {
    const gi::AssetDistanceField* field = nullptr;
    Mat4 transform = Mat4::identity();
};

/// What one cell holds for the GI. The loader fills it; the streamer keeps a copy until the cell
/// has arrived.
struct GiCellContent {
    explicit GiCellContent(Allocator& allocator) noexcept
        : cards(allocator), fields(allocator), volumes(allocator), instances(allocator) {}

    /// World-space cards. An emissive card is classified as any ingested emitter is.
    Array<gi::Surfel> cards;
    Array<GiFieldPlacement> fields;
    Array<gi::GiVolume> volumes;
    /// The cell's meshes for the hardware tier: instances of geometry the application declared to
    /// the system's acceleration service (`declare_geometry`) once, as a level's meshes are. Added
    /// with the fields, removed with the cell. Empty where the device has no ray queries.
    Array<rt::InstanceDescriptor> instances;
};

/// Produces a cell's GI content. Leaving `out` empty is a cell with nothing for the GI, which is
/// not an error; a failure is reported and the cell is not asked again until it arrives again.
using GiCellLoader = Status (*)(void* user, world::CellId cell,
                                const world::PartitionConfig& partition,
                                GiCellContent& out) noexcept;

/// What one tick may spend. What left is paid for first — it is evicted at once, as the world
/// asks — and what arrives takes the rest.
struct GiStreamingBudget {
    /// Cards ingested or evicted a tick. A cell with more arrives over several ticks; one
    /// instance's cards (an emitter's) are never split, so a single instance larger than this is a
    /// tick of its own.
    u32 cards_per_tick = 2048;
    /// Distance-field placements made or removed a tick.
    u32 fields_per_tick = 8;
};

/// What one tick did.
struct GiStreamingReport {
    u32 events_drained = 0;
    /// Cells whose content the loader produced this tick.
    u32 cells_loaded = 0;
    /// Cells whose last chunk and last field went in this tick.
    u32 cells_completed = 0;
    u32 cells_evicted = 0;
    /// Cells still arriving after this tick.
    u32 cells_pending = 0;
    u32 chunks_ingested = 0;
    u32 cards_ingested = 0;
    /// Cards the cells that left took out of the GI: spent from the tick's card budget first.
    u32 cards_evicted = 0;
    u32 fields_placed = 0;
    u32 fields_removed = 0;
    u32 instances_added = 0;
    u32 instances_removed = 0;
    /// The streamed volumes were placed again (a cell with volumes arrived or left).
    bool volumes_changed = false;
    /// Cells whose events did not carry `Channel::Illumination`.
    u32 cells_without_channel = 0;
    u32 loader_failures = 0;
};

class GiCellStreaming {
public:
    /// `system` and `partition` are borrowed and outlive this.
    GiCellStreaming(Allocator& allocator, gi::IlluminationSystem& system,
                    const world::PartitionConfig& partition) noexcept;
    ~GiCellStreaming();

    GiCellStreaming(const GiCellStreaming&) = delete;
    GiCellStreaming& operator=(const GiCellStreaming&) = delete;
    GiCellStreaming(GiCellStreaming&&) = delete;
    GiCellStreaming& operator=(GiCellStreaming&&) = delete;

    /// Register with the world's queue at the declared consumer `order`.
    [[nodiscard]] Status attach(world::CellEventQueue& events, u32 order) noexcept;
    [[nodiscard]] bool attached() const noexcept { return events_ != nullptr; }

    void set_loader(GiCellLoader loader, void* user) noexcept {
        loader_ = loader;
        loader_user_ = user;
    }

    /// Drain the queue, evict what left, and bring in what arrived up to `budget`. Once per frame,
    /// before the GI's `update`; `frame` stamps the invalidations it files.
    [[nodiscard]] Expected<GiStreamingReport, Error> tick(u64 frame,
                                                          const GiStreamingBudget& budget) noexcept;

    /// Whether every chunk and field of `cell` is in the GI.
    [[nodiscard]] bool resident(world::CellId cell) const noexcept;
    /// Cells bound (arrived or arriving).
    [[nodiscard]] usize cells() const noexcept { return cells_.size(); }
    /// Cards the streamed cells hold in the GI now.
    [[nodiscard]] u64 cards_in_gi() const noexcept { return cards_in_gi_; }

    /// Evict every streamed cell. What the owner does before the system goes.
    void clear(u64 frame) noexcept;

private:
    struct Cell {
        explicit Cell(Allocator& allocator) noexcept
            : content(allocator), chunks(allocator), placements(allocator), instances(allocator) {}
        Cell(Cell&&) noexcept = default;
        Cell& operator=(Cell&&) noexcept = default;
        Cell(const Cell&) = delete;
        Cell& operator=(const Cell&) = delete;
        ~Cell() = default;

        world::CellId id;
        GiCellContent content;
        /// The GI scene cells its cards went in as, and the next card to go.
        Array<u64> chunks;
        usize next_card = 0;
        /// The field placement ids it placed, and the next field to place.
        Array<u64> placements;
        usize next_field = 0;
        /// The acceleration service's handles of the instances it added, and the next to add.
        Array<u32> instances;
        usize next_instance = 0;
        u64 cards_in = 0;
        bool complete = false;
    };

    [[nodiscard]] Status bind_cell(world::CellId cell, GiStreamingReport& report) noexcept;
    void release_cell(world::CellId cell, u64 frame, GiStreamingReport& report) noexcept;
    [[nodiscard]] Status advance(Cell& cell, u64 frame, u32 cards_per_tick, u32& cards_left,
                                 u32& fields_left, GiStreamingReport& report) noexcept;
    [[nodiscard]] Status place_volumes() noexcept;
    [[nodiscard]] Cell* find(world::CellId cell) noexcept;
    [[nodiscard]] const Cell* find(world::CellId cell) const noexcept;

    Allocator* allocator_;
    gi::IlluminationSystem* system_;
    const world::PartitionConfig* partition_;
    world::CellEventQueue* events_ = nullptr;
    world::CellEventQueue::ConsumerId consumer_ = 0;
    GiCellLoader loader_ = nullptr;
    void* loader_user_ = nullptr;
    /// In arrival order: a cell arrives before any cell that arrived after it.
    Array<Cell> cells_;
    Array<world::CellEvent> drained_;
    /// The next GI scene cell and field placement id this streamer hands out, in a range of its
    /// own (`kStreamedTag`) so it never meets an object's or an application's.
    u64 next_id_ = 1;
    u64 cards_in_gi_ = 0;
};

/// The tag every GI scene cell and field placement id a streamer hands out carries: bit 62, beside
/// the objects' bit 63 (`gi::kObjectCellTag`).
inline constexpr u64 kStreamedTag = 1ULL << 62U;

}  // namespace cy::rendering::gi_streaming
