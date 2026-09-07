#pragma once
// Runtime producers: pages written rather than read, and the persistence class that says whether
// they are ever saved. Task 5.3.
//
// `virtual-texturing` — "Runtime producers": pages "SHALL be producible at runtime through a
// **producer interface**, invoked by the residency system when a page is required". The engine
// provides producers for "cooked disk tiles, terrain material composition, decal accumulation,
// world-state data, and procedural generation", and projects register their own. A producer
// "SHALL render or compute into the physical cache through the render graph, SHALL declare its
// cost, and SHALL be budgeted like any other GPU work". Production is cached: "an expensive
// composition is evaluated once rather than per frame".
//
// --- COOKED DISK TILES ARE A PRODUCER, NOT A SPECIAL CASE
// ------------------------------------------
//
// The specification lists "cooked disk tiles" first among the producers, and that ordering is the
// design. If reading from disk were the built-in path and production were the extension, the two
// would have different request paths, different budgets and different diagnostics, and every
// question about "why is this page slow" would have two answers. One interface, and the disk reader
// implements it.
//
// --- DECLARING A COST IS NOT OPTIONAL
// -----------------------------------------------------------------
//
// `declared_cost_ms()` is pure virtual, with no default. It reaches `residency::RequestInputs::
// production_cost_ms`, which is one of the seven scoring terms and the one that turns a deadline
// into slack. A producer that did not declare a cost would be scheduled as though it were free, and
// an expensive composition scheduled as free is a missed deadline that looks like a scoring bug.
//
// --- LAYER 2 AND THE RENDER GRAPH
// ------------------------------------------------------------------------
//
// "through the render graph" is a statement about where production RUNS, and the render graph is
// layer 4. This module is layer 2 and may not name it (see `src/servers/CMakeLists.txt`), so the
// interface here is what a graph pass calls: an address, the tile that was reserved for it, and the
// staging bytes to fill. The pass that wraps this and puts it in the graph belongs above.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/hash_map.h>
#include <cy/servers/render/virtual_texturing/address.h>

namespace cy::render::vt {

/// `virtual-texturing` — "Runtime page invalidation and persistence": runtime page content "SHALL
/// declare its persistence class".
enum class PersistenceClass : u8 {
    /// Regenerated every time it is needed and never kept across a load.
    Transient = 0,
    /// Regenerable from inputs that are themselves saved, so it is NOT written to the save. The
    /// specification's example is wetness produced from a field.
    Derived,
    /// Written to the save as a **delta over the cooked base**, in the world persistence overlay,
    /// "so that a terraformed region saves the changed pages rather than a new texture".
    SaveGame,
    /// Replicated to peers rather than saved.
    Replicated,
    Count,
};

[[nodiscard]] const char* persistence_class_name(PersistenceClass persistence) noexcept;

/// Whether a page of this class is written to a save. One function, so that the persistence overlay
/// and the diagnostics cannot disagree about which pages a save contains.
[[nodiscard]] constexpr bool is_saved(PersistenceClass persistence) noexcept {
    return persistence == PersistenceClass::SaveGame;
}

/// What a producer fills, and where.
struct ProductionRequest {
    VirtualAddress address;
    /// The slot `PhysicalTileCache::acquire` reserved. The producer does not choose it.
    u32 physical_tile = kNoPhysicalTile;
    /// Staging bytes for that slot, and how many. Never null when `bytes` is non-zero.
    u8* destination = nullptr;
    u32 bytes = 0;
};

class PageProducer {
public:
    PageProducer() = default;
    virtual ~PageProducer() = default;

    PageProducer(const PageProducer&) = delete;
    PageProducer& operator=(const PageProducer&) = delete;
    PageProducer(PageProducer&&) = delete;
    PageProducer& operator=(PageProducer&&) = delete;

    /// For a diagnostic. Never null.
    [[nodiscard]] virtual const char* producer_name() const noexcept = 0;

    /// What it costs to produce one page, in milliseconds. Pure virtual on purpose; see the header.
    [[nodiscard]] virtual f32 declared_cost_ms() const noexcept = 0;

    [[nodiscard]] virtual PersistenceClass persistence() const noexcept {
        return PersistenceClass::Derived;
    }

    /// Fill one page. Called on a production worker, never on the frame's thread.
    ///
    /// MAY BE CALLED CONCURRENTLY FOR DIFFERENT PAGES, and never twice for the same page at once —
    /// the system reserves the tile before dispatching and does not dispatch a second request for a
    /// tile that is still pending. A producer that keeps mutable state shared between pages must
    /// synchronise it itself.
    virtual Status produce(const ProductionRequest& request) noexcept = 0;
};

/// Which producer serves which virtual texture.
///
/// Keyed per texture rather than per page: a virtual texture has one source, and a design that let
/// individual pages come from different producers would make "why is this page slow" a per-page
/// question with no stable answer.
class ProducerRegistry {
public:
    explicit ProducerRegistry(Allocator& allocator = current_allocator()) noexcept
        : producers_(allocator) {}

    ProducerRegistry(const ProducerRegistry&) = delete;
    ProducerRegistry& operator=(const ProducerRegistry&) = delete;
    ProducerRegistry(ProducerRegistry&&) noexcept = default;
    ProducerRegistry& operator=(ProducerRegistry&&) noexcept = default;
    ~ProducerRegistry() = default;

    /// The registry does NOT own the producer. A producer outlives the pages it makes and is owned
    /// by whatever configured it — a terrain system, a plugin, a test fixture.
    Status register_producer(u32 texture, PageProducer& producer) noexcept;
    bool unregister_producer(u32 texture) noexcept;

    [[nodiscard]] PageProducer* find(u32 texture) const noexcept;
    [[nodiscard]] usize size() const noexcept { return producers_.size(); }
    void clear() noexcept { producers_.clear(); }

private:
    HashMap<u64, PageProducer*> producers_;
};

}  // namespace cy::render::vt
