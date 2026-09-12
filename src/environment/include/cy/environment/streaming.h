#pragma once
// Field tiles streamed with world cells, through the residency policy every other paged subsystem
// already shares. M10 task 1.3.
//
// `environment-fields` — "Sparse tiled storage and streaming": "Field tiles SHALL be cooked as a
// CELL CHANNEL and stream with world cells (see `world-partition-and-streaming`), and SHALL be
// evicted with them." And "Field residency levels": residency "SHALL be requestable by streaming
// sources and by consumers, THROUGH THE SHARED RESIDENCY POLICY in `residency`, and SHALL follow
// the same importance and budget model as other paged data."
//
// ================================================================================================
// NOTHING HERE IS A PARALLEL STREAMING MECHANISM, AND THAT IS THE REQUIREMENT
// ================================================================================================
//
// Two existing mechanisms carry this module and it adds neither:
//
//   * `world::CellEventQueue` — this is a REGISTERED CONSUMER of it, at a declared order, like
//     navigation, audio and illumination. A cell becoming resident with `Channel::Fields` in its
//     mask is what binds that cell's tiles; a cell being evicted is what releases them. "Evicted
//     with them" is therefore a consequence of consuming the same events rather than a second
//     lifetime to keep in step.
//
//   * `residency::ResidencyServer` — every tile this module wants is a `residency::Request` scored
//     by the shared policy, and every tile it drops is that policy's `EvictionOrder`. There is no
//     budget here, no eviction rule here and no importance model here.
//
// **This module does not register the residency subsystem.** The budget is the application's to
// declare, `world-partition-and-streaming`'s own cell preparation draws from the same subsystem,
// and a module that registered a policy at construction would be one deciding a budget on behalf
// of whoever owns it. `attach()` refuses if `Subsystem::WorldCells` is not registered, naming what
// to do about it.
//
// ================================================================================================
// THE PAGE IDENTIFIER, AND THE HALF OF IT THIS MODULE DOES NOT OWN
// ================================================================================================
//
// `residency::PageKey` carries a subsystem tag and 56 opaque bits the subsystem alone resolves.
// Field tiles and world cells share `Subsystem::WorldCells`, because a field tile IS part of a
// cell's preparation, so the two must share those 56 bits without colliding. **Bit 55 is the
// split**: 1 for a field tile, 0 for everything `src/world/` may want to page later. It is declared
// here, in one constant, so that the day world cells page themselves the collision is a compile-
// time constant to read rather than a bug to find.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/environment/store.h>
#include <cy/servers/residency/server.h>
#include <cy/world/activation.h>
#include <cy/world/cell.h>

namespace cy::environment {

/// Bit 55 of a `WorldCells` page identifier: set for a field tile. See the header note.
inline constexpr u64 kFieldPageBit = 1ULL << 55U;

/// The largest tile coordinate a page identifier can carry, in either direction. 2^20 tiles at the
/// smallest useful tile size is over ten thousand kilometres, which is past the point where f64
/// world coordinates are the constraint.
inline constexpr i32 kMaxPageTile = 1 << 20;

/// Pack a tile into the 56 bits `residency::PageKey` gives a subsystem.
[[nodiscard]] u64 page_of_tile(u32 field_slot, const TileAddress& address) noexcept;

/// Where a tile's bytes come from when the policy admits it.
///
/// **This is the seam every producer row plugs into.** The substrate does not know how terrain
/// cooks a moisture tile or how weather cooks a wind tile; it knows when one is wanted and where to
/// put it. A field with no loader materialises its declared default, which is the correct behaviour
/// for a field whose values are written at run time rather than cooked — and the honest behaviour
/// for one whose cooked source has not been written yet.
///
/// Returning an error is not a failure of streaming: it means "no cooked data for that tile", and
/// the admission is cancelled through the policy's own `cancel_admission()` so the bytes are given
/// back rather than silently committed.
using TileLoader = Status (*)(void* user, const TileAddress& address, Array<u8>& out);

/// What one tick did. Everything a test or a profiler view needs, and nothing it has to infer.
struct FieldStreamingReport {
    u32 cells_bound = 0;
    u32 cells_released = 0;
    u32 tiles_requested = 0;
    u32 tiles_admitted = 0;
    u32 tiles_loaded = 0;
    u32 tiles_evicted = 0;
    /// Evictions the policy ordered for a guaranteed tile. Counted rather than obeyed: macro data
    /// must exist for regions that are not loaded, so the order is refused and reported.
    u32 evictions_refused = 0;
    /// Admissions whose loader had nothing. Handed back to the policy.
    u32 loads_abandoned = 0;
};

/// Binds field tiles to world cells and drives them through the shared residency policy.
class FieldStreaming {
public:
    FieldStreaming(Allocator& allocator, FieldStore& store,
                   residency::ResidencyServer& residency) noexcept;

    FieldStreaming(const FieldStreaming&) = delete;
    FieldStreaming& operator=(const FieldStreaming&) = delete;

    /// Take ownership of a field's producer token, and say which levels stream with cells.
    ///
    /// The streamer IS the producer of a cooked field: it is what puts cooked bytes into tiles, and
    /// "one producer per field" would be a rule about `FieldWriter` alone if a streamer could
    /// insert values for a field it does not own. A row that writes its field at run time keeps its
    /// own token and does not adopt it here.
    ///
    /// `finest` is the finest level this field streams; coarser declared levels stream too. A field
    /// that only ever exists at macro resolution passes `FieldResidency::Macro`.
    [[nodiscard]] Status adopt(ProducerToken&& token, FieldResidency finest) noexcept;

    /// Tell the streamer where a cell is. A cell event carries an identifier and a channel mask
    /// and nothing spatial — deliberately, because `world::CellId` is opaque by requirement — so
    /// the footprint of a cell has to come from the same place the cell did: whoever added it to
    /// the world. One call per cooked cell, at load time, and the module needs no reference to
    /// `WorldStreaming` at all.
    [[nodiscard]] Status declare_cell(world::CellId cell, const world::CellCoord& coord) noexcept;

    /// The cooked source for one adopted field. Optional; see `TileLoader`.
    [[nodiscard]] Status set_loader(FieldId field, TileLoader loader, void* user) noexcept;

    /// Register as a consumer of a world's cell events, at a declared order.
    ///
    /// Refuses when `residency::Subsystem::WorldCells` is not registered with the policy: this
    /// module does not decide that budget. See the header note.
    [[nodiscard]] Status attach(world::CellEventQueue& events, u32 order) noexcept;

    /// Make one field's macro level exist over a rectangle of tiles, guaranteed and unevictable.
    ///
    /// "Macro-level data SHALL be resident for the whole world where a field declares it, since
    /// environmental and ecosystem state must exist for regions that are not loaded." The rectangle
    /// is the world's extent, which is the caller's to know; this module will not guess it from the
    /// cells that happen to have streamed, because the whole point of the macro level is that it
    /// covers the ones that have not.
    /// The tiles are made resident directly rather than through an admission — a guaranteed tile is
    /// the coarse fallback every other answer is defined in terms of, and a budget that could
    /// refuse it would make "the whole world has weather" a matter of memory pressure. They are
    /// then REPORTED to the policy as guaranteed resident pages, so their bytes are in the same
    /// budget every other paged subsystem draws from rather than in a pool this module keeps
    /// privately.
    [[nodiscard]] Status guarantee_macro(FieldId field, i32 min_tile_x, i32 min_tile_z,
                                         i32 max_tile_x, i32 max_tile_z, f64 now) noexcept;

    /// One tick: drain cell events, ask the policy for what the bound cells need, act on what it
    /// decided. `now` is the clock the policy scores deadlines against.
    ///
    /// It does NOT call `ResidencyServer::end_frame()`. That is the frame's boundary and belongs to
    /// whoever owns the frame; a paged subsystem that advanced it would advance it for the other
    /// five as well.
    [[nodiscard]] Expected<FieldStreamingReport, Error> tick(f64 now) noexcept;

    /// Cells currently bound — those whose tiles this module is asking for.
    [[nodiscard]] usize bound_cells() const noexcept { return bindings_.size(); }
    [[nodiscard]] usize adopted_fields() const noexcept { return fields_.size(); }
    /// The page identifier a tile would be requested under. For diagnostics and for a test that
    /// wants to ask the policy about a tile by name.
    [[nodiscard]] Expected<residency::PageKey, Error> page_of(
        const TileAddress& address) const noexcept;

private:
    struct Adopted {
        ProducerToken token;
        FieldId field;
        u8 finest = 0;
        TileLoader loader = nullptr;
        void* user = nullptr;
    };

    /// One bound cell: which cell, and the tiles it wants. Recomputed when the cell binds and kept
    /// so that releasing it does not have to recompute the footprint from a partition configuration
    /// that may have moved on.
    struct Binding {
        world::CellId cell;
        Array<TileAddress> tiles;

        explicit Binding(Allocator& allocator) noexcept : tiles(allocator) {}
    };

    /// The tick, in the five steps `tick()` runs them in. Separate functions because a tick written
    /// as one was 126 of cognitive complexity and unreviewable — and because each of the five is a
    /// seam a reader already has a name for.
    [[nodiscard]] Status consume_cell_events(FieldStreamingReport& report) noexcept;
    [[nodiscard]] Status collect_wanted() noexcept;
    [[nodiscard]] Status submit_requests(FieldStreamingReport& report) noexcept;
    [[nodiscard]] Status load_admission(const residency::Admission& admission, f64 now,
                                        FieldStreamingReport& report) noexcept;
    [[nodiscard]] Status apply_evictions(f64 now, FieldStreamingReport& report) noexcept;
    /// Which wanted tile carries a page identifier, and the backwards index an eviction needs.
    [[nodiscard]] const TileAddress* wanted_tile_of(u64 page) const noexcept;
    [[nodiscard]] Status index_resident_pages() noexcept;
    /// One guaranteed tile: cooked bytes or the declared default, and the report that puts its
    /// bytes in the shared budget.
    [[nodiscard]] Status materialise_guaranteed(Adopted& adopted,
                                                const FieldDeclaration& declaration,
                                                const TileAddress& address, f64 now) noexcept;

    [[nodiscard]] Adopted* find_field(FieldId field) noexcept;
    [[nodiscard]] const Adopted* find_field(FieldId field) const noexcept;
    [[nodiscard]] i32 slot_of(FieldId field) const noexcept;
    [[nodiscard]] Status bind_cell(world::CellId cell, FieldStreamingReport& report) noexcept;
    [[nodiscard]] Status release_cell(world::CellId cell, FieldStreamingReport& report) noexcept;

    Allocator* allocator_;
    FieldStore* store_;
    residency::ResidencyServer* residency_;
    world::CellEventQueue* events_ = nullptr;
    world::CellEventQueue::ConsumerId consumer_ = 0;

    Array<Adopted> fields_;
    Array<Binding> bindings_;
    HashMap<world::CellId, world::CellCoord> cells_;
    Array<world::CellEvent> drained_;
    residency::Schedule schedule_;
    /// Tiles wanted by at least one bound cell, and their page identifiers, rebuilt each tick.
    /// Kept as members so a tick allocates nothing in the steady state.
    Array<TileAddress> wanted_;
    Array<u64> wanted_pages_;
    /// The resident tiles and their page identifiers, rebuilt only when the policy orders an
    /// eviction — which is the only time a page identifier has to be resolved backwards.
    Array<TileAddress> resident_;
    Array<u64> resident_pages_;
    /// One tile's cooked bytes, reused across admissions so a tick allocates nothing in the steady
    /// state.
    Array<u8> load_buffer_;
};

/// The tiles of one field, at one level, covering one cell.
///
/// A free function rather than a member, and a `CellCoord` rather than a `WorldStreaming`, because
/// the footprint of a cell is arithmetic over its coordinate and the partition — it needs no
/// streaming object, which is what lets a cooker compute the same rectangle offline.
[[nodiscard]] Status cell_tile_footprint(const world::PartitionConfig& partition,
                                         const world::CellCoord& coord,
                                         const FieldDeclaration& declaration, FieldId field,
                                         u8 level, Array<TileAddress>& out) noexcept;

}  // namespace cy::environment
