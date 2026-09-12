#pragma once
// Clusters as a cell channel: loaded with a world cell, evicted with it, independently evictable.
// M10 task 2.4.
//
// `foliage` — "Foliage clusters": "Clusters SHALL STREAM WITH WORLD CELLS as a cell channel, and
// SHALL BE INDEPENDENTLY EVICTABLE", with the scenario "WHEN a region streams in THEN its foliage
// clusters SHALL be loaded and published, and EVICTED WHEN THE REGION UNLOADS."
//
// ================================================================================================
// "EVICTED WITH THEM" FALLS OUT OF CONSUMING THE SAME EVENTS
// ================================================================================================
//
// This class is a registered consumer of `world::CellEventQueue`, exactly as
// `environment::FieldStreaming` and `water::WaterStreaming` are. It does not observe cells, poll a
// partition or hold a streaming policy: it drains the queue the world already emits, binds clusters
// to the cells that carry `world::Channel::Foliage`, and drops them on `Evicted`. A second
// notification model here would be a second thing that can disagree with the world about what is
// resident.
//
// INDEPENDENTLY EVICTABLE is the other half, and it is why `drop_cluster()` exists beside the event
// path: memory pressure may take one cluster out of a resident cell without the cell going
// anywhere, and a design in which a cluster could only leave with its cell would make the foliage
// budget's only lever "unload the region".
//
// ================================================================================================
// THE LOADER SEAM
// ================================================================================================
//
// A cluster arrives one of two ways and this class does not care which: a COOKED cluster comes from
// `ClusterLoader`, and a GENERATED one comes from `placement.h` through the same callback. That is
// `environment::FieldStreaming::set_loader()`'s arrangement, deliberately — a world that cooks its
// forests and a world that generates them at load differ in one function pointer, and every other
// line of streaming is the same.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/foliage/grass.h>
#include <cy/foliage/instance.h>
#include <cy/world/activation.h>
#include <cy/world/cell.h>
#include <cy/world/coordinates.h>

namespace cy::foliage {

/// A cluster produced for a cell, with its ground cover. The loader's output unit.
///
/// The patches travel WITH the cluster rather than in a second payload, because a cluster and its
/// ground cover are evicted together: a meadow whose grass outlived the cluster it was placed
/// against would be grass with no bounds to decode itself against.
struct FoliagePopulationHandle {
    FoliageCluster cluster;
    Array<GrassPatch> patches;

    FoliagePopulationHandle(FoliageCluster&& built, Allocator& allocator) noexcept
        : cluster(static_cast<FoliageCluster&&>(built)), patches(allocator) {}

    FoliagePopulationHandle(const FoliagePopulationHandle&) = delete;
    FoliagePopulationHandle& operator=(const FoliagePopulationHandle&) = delete;
    FoliagePopulationHandle(FoliagePopulationHandle&&) noexcept = default;
    FoliagePopulationHandle& operator=(FoliagePopulationHandle&&) noexcept = default;
    ~FoliagePopulationHandle() = default;
};

/// Produces the clusters of one cell. Cooked or generated; see the header note.
///
/// Returns `NotFound` where a cell simply has no foliage, which is not an error — most cells in a
/// world have none — and the streamer records the cell as bound with zero clusters so it does not
/// ask again every frame.
using ClusterLoader = Status (*)(void* user, world::CellId cell,
                                 const world::PartitionConfig& partition,
                                 Array<FoliagePopulationHandle>& out) noexcept;

/// What one tick of streaming did.
struct StreamingReport {
    u32 events_drained = 0;
    u32 cells_bound = 0;
    u32 cells_released = 0;
    u32 clusters_loaded = 0;
    u32 clusters_evicted = 0;
    /// Cells whose channel mask does not carry `Channel::Foliage` — a server profile, a spectator
    /// source. Counted so a world that streams no foliage says so rather than looking broken.
    u32 cells_without_channel = 0;
    /// Cells the loader had nothing for.
    u32 cells_empty = 0;
    u32 loader_failures = 0;
};

/// Binds clusters to world cells.
class FoliageStreaming {
public:
    FoliageStreaming(Allocator& allocator, ClusterStore& clusters, GrassField& grass,
                     const world::PartitionConfig& partition) noexcept;

    FoliageStreaming(const FoliageStreaming&) = delete;
    FoliageStreaming& operator=(const FoliageStreaming&) = delete;

    /// Register with the world's queue. `order` is the declared consumer order — foliage runs after
    /// terrain and after the field substrate, because a cluster's placement reads both.
    [[nodiscard]] Status attach(world::CellEventQueue& events, u32 order) noexcept;
    [[nodiscard]] bool attached() const noexcept { return attached_; }

    void set_loader(ClusterLoader loader, void* user) noexcept {
        loader_ = loader;
        loader_user_ = user;
    }

    /// Drain the queue and act on it. The per-tick call.
    [[nodiscard]] Expected<StreamingReport, Error> tick() noexcept;

    /// Drop one cluster without touching its cell. The "independently evictable" half.
    [[nodiscard]] bool drop_cluster(ClusterId cluster) noexcept;

    /// The clusters bound to one cell.
    [[nodiscard]] Span<const ClusterId> clusters_of(world::CellId cell) const noexcept;
    [[nodiscard]] usize bound_cells() const noexcept { return cells_.size(); }

private:
    struct Binding {
        world::CellId cell;
        Array<ClusterId> clusters;

        explicit Binding(Allocator& allocator) noexcept : clusters(allocator) {}
    };

    [[nodiscard]] Status bind_cell(world::CellId cell, StreamingReport& report) noexcept;
    [[nodiscard]] u32 release_cell(world::CellId cell) noexcept;
    [[nodiscard]] Binding* find_binding(world::CellId cell) noexcept;

    Allocator* allocator_;
    ClusterStore* clusters_;
    GrassField* grass_;
    const world::PartitionConfig* partition_;
    world::CellEventQueue* events_ = nullptr;
    world::CellEventQueue::ConsumerId consumer_ = 0;
    bool attached_ = false;
    ClusterLoader loader_ = nullptr;
    void* loader_user_ = nullptr;
    Array<Binding> cells_;
    HashMap<u64, usize> index_;
    Array<world::CellEvent> drained_;
};

}  // namespace cy::foliage
