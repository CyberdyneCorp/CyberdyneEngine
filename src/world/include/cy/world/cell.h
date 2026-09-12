#pragma once
// The cell: its state, its channels, its cost, and the ECS-NATIVE form its entity data is cooked
// in. Tasks 3.2, 3.4 and 3.5.
//
// --- THE INVARIANT M6 EXISTS TO ESTABLISH -------------------------------------------------------
//
// `world-partition-and-streaming` — "Cells are cooked in ECS-native form": entity data is cooked as
// ARCHETYPE BLOCKS, an identifier array and a column per component type, ready to be copied into
// ECS chunks. "Cooking SHALL NOT produce an entity-by-entity object graph requiring per-entity
// construction or reflection at load." Activation is then: allocate chunks, decompress, bulk copy,
// fix up references.
//
// design.md §5 pins it to this milestone: "Cooking to an intermediate and converting at load makes
// streaming cost proportional to content" — a migration later, cheap now. `CookedBlock` below is
// laid out to be passed to `ecs::World::instantiate()` with no transformation at all: its
// `components` and `columns` spans ARE that call's arguments. If a future change makes this
// structure need a conversion step before instantiation, that change has undone the invariant.
//
// --- FOUR AXES, NOT ONE -------------------------------------------------------------------------
//
// The specification's own framing: "cell residency, cell activation, asset residency and simulation
// detail are four separate axes. Collapsing them is what makes crossing a boundary mean *load
// everything now*." `CellState` below is the first two; the third is `residency`'s and the fourth
// belongs to the AI, animation and physics LOD systems. A cell may be `Resident` for as long as it
// likes without being `Activated`, and the M6 exit criterion "a test holds bytes resident with
// simulation off" is that sentence made checkable.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/asset_id.h>
#include <cy/ecs/component.h>
#include <cy/world/coordinates.h>
#include <cy/world/layers.h>
#include <cy/world/partition.h>

namespace cy::world {

/// A cell's state. Richer than loaded/unloaded, because approach is a gradient and every step of it
/// is cheap only because the expensive part already happened.
enum class CellState : u8 {
    /// Nothing but its index entry.
    Unloaded = 0,
    /// Bounds, cost and dependencies known.
    Metadata,
    /// Data and assets in flight.
    Prefetching,
    /// Data and resources in memory; entities NOT yet in the ECS world.
    Resident,
    /// Entities published and participating in simulation.
    Activated,
    /// Being withdrawn.
    Deactivating,
    /// Resident but not needed; memory reclaimable.
    Evictable,
};

[[nodiscard]] const char* cell_state_name(CellState state) noexcept;

/// Streaming is per channel, not all-or-nothing. A source declares which channels it requires and a
/// cell streams only the channels some active source requires — which is what lets a spectator
/// camera stream geometry and textures while physics, navigation and AI stay unloaded.
enum class Channel : u8 {
    Entities = 0,
    Geometry,
    Textures,
    Physics,
    Navigation,
    Ai,
    Audio,
    Illumination,
    /// Environment field tiles (`environment-fields`, "Field tiles SHALL be cooked as a cell
    /// channel and stream with world cells ... and SHALL be evicted with them"). Added at M10 by
    /// src/environment/, which consumes `CellEventQueue` and keys on this bit; nothing in this
    /// module produces or consumes it, which is why it costs this file one enumerator and
    /// `profile_channels()` one line.
    Fields,
    /// Water payloads (`water`, "Water streaming": "Water payloads SHALL be a cell channel, so a
    /// server profile can omit surface rendering data while retaining the queries and physics
    /// representation it needs"). Added at M10 by src/water/, which consumes `CellEventQueue` and
    /// binds a body's segments to the cells that carry this bit; nothing in this module produces or
    /// consumes it. The *surface rendering* half of a water payload is dropped inside that module
    /// by the world profile rather than by a second channel, because a server that dropped the
    /// whole channel would lose the queries and the physics representation with it.
    Water,
    /// Terrain tiles (`terrain`, "Tiled hierarchical storage": "Tiles SHALL stream as part of world
    /// cells (see `world-partition-and-streaming`), with terrain data COOKED AS A CELL CHANNEL").
    /// Added at M10 by src/terrain/, which owns the arithmetic saying which tiles a cell covers —
    /// `terrain::cell_tile_footprint()` — and keys on this bit; nothing in this module produces or
    /// consumes it, which is why it costs this file one enumerator and `profile_channels()` one
    /// line. The COLLISION resolution of those tiles is a separate axis inside that module rather
    /// than a second channel, because a distant tile may be visible with coarse physics and that is
    /// a detail level, not a payload.
    Terrain,
    /// Foliage clusters (`foliage`, "Foliage clusters": "Clusters SHALL stream with world cells AS
    /// A CELL CHANNEL, and SHALL be independently evictable"). Added at M10 by src/foliage/, which
    /// consumes `CellEventQueue` and binds a cell's clusters on this bit; nothing in this module
    /// produces or consumes it. Ground cover travels inside the same payload rather than in a
    /// second channel, because a patch is decoded against its cluster's bounds and grass that
    /// outlived its cluster would have nothing to decode itself against.
    Foliage,
    kCount,
};

[[nodiscard]] const char* channel_name(Channel channel) noexcept;

/// A set of channels. A plain bitmask with names, so that a mask is copied, compared and stored
/// without a container.
struct ChannelMask {
    u16 bits = 0;

    [[nodiscard]] static constexpr ChannelMask none() noexcept { return ChannelMask{}; }
    [[nodiscard]] static constexpr ChannelMask all() noexcept {
        return ChannelMask{static_cast<u16>((1u << static_cast<u32>(Channel::kCount)) - 1u)};
    }
    [[nodiscard]] static constexpr ChannelMask of(Channel channel) noexcept {
        return ChannelMask{static_cast<u16>(1u << static_cast<u32>(channel))};
    }

    [[nodiscard]] constexpr bool has(Channel channel) const noexcept {
        return (bits & of(channel).bits) != 0;
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return bits == 0; }
    constexpr void set(Channel channel) noexcept { bits |= of(channel).bits; }
    constexpr void clear(Channel channel) noexcept {
        bits = static_cast<u16>(bits & ~of(channel).bits);
    }

    [[nodiscard]] constexpr ChannelMask operator|(ChannelMask other) const noexcept {
        return ChannelMask{static_cast<u16>(bits | other.bits)};
    }
    [[nodiscard]] constexpr ChannelMask operator&(ChannelMask other) const noexcept {
        return ChannelMask{static_cast<u16>(bits & other.bits)};
    }
    /// The channels in this mask that `other` does not carry. This is the whole of "a source's
    /// channel mask gains physics, so the physics payload streams for cells already resident,
    /// without reloading them" — the delta is requested, the rest is left alone.
    [[nodiscard]] constexpr ChannelMask without(ChannelMask other) const noexcept {
        return ChannelMask{static_cast<u16>(bits & ~other.bits)};
    }

    friend constexpr bool operator==(ChannelMask, ChannelMask) noexcept = default;
};

/// A dedicated server needs entities, physics, navigation and AI and none of the rendering
/// channels. `world-partition-and-streaming`, "Client and server world profiles": one world
/// definition, two profiles — and cell, entity and layer identity are identical across them, which
/// is why a profile selects channels and never touches the partition.
enum class WorldProfile : u8 { Client = 0, DedicatedServer, Editor };

[[nodiscard]] ChannelMask profile_channels(WorldProfile profile) noexcept;
[[nodiscard]] const char* world_profile_name(WorldProfile profile) noexcept;

/// The cost model's rates. Crude on purpose: `world-partition-and-streaming` requires estimates to
/// be VALIDATED against measured runtime cost and significant divergence reported, and a crude
/// model that is checked is worth more than a clever one that is not.
///
/// THE ACTIVATION BUDGET IS SPENT AGAINST THE ESTIMATE, NOT AGAINST A CLOCK. A budget enforced by
/// sampling wall time makes activation non-deterministic — the same route on two machines prepares
/// different cells on the same frame — and it is unreproducible in a test. So preparation spends
/// `estimate_activation_time()` of its budget per block, and the measured time is compared against
/// the estimate afterwards, which is the "estimates are checked" scenario doing double duty.
inline constexpr Nanoseconds kActivationNanosecondsPerRow = 120;
inline constexpr Nanoseconds kActivationNanosecondsPerKilobyte = 40;

[[nodiscard]] constexpr Nanoseconds estimate_activation_time(u32 rows, u64 bytes) noexcept {
    return (static_cast<Nanoseconds>(rows) * kActivationNanosecondsPerRow) +
           (static_cast<Nanoseconds>(bytes / 1024) * kActivationNanosecondsPerKilobyte);
}

/// What a cell costs, computed by the cooker so that the planner compares candidates BEFORE
/// requesting either, rather than discovering cost after loading.
struct CellCost {
    u64 io_bytes = 0;
    u64 cpu_memory_bytes = 0;
    u64 gpu_memory_bytes = 0;
    u32 entities = 0;
    u32 physics_bodies = 0;
    u32 navigation_tiles = 0;
    Nanoseconds activation_time = 0;
};

/// One archetype's worth of one layer's entities, in the exact shape `ecs::World::instantiate()`
/// consumes. Nothing here is converted at load.
struct CookedBlock {
    /// Which layer these rows belong to. Rows are grouped by (archetype, layer) so that activating
    /// or deactivating a layer publishes or withdraws whole blocks rather than touching entities
    /// one at a time — the "scenario switch is one operation" scenario.
    LayerId layer;
    /// The component set. Parallel to `columns`.
    Array<ecs::ComponentTypeId> components;
    /// One column per component, `count` rows of that component's size, contiguous.
    Array<Array<u8>> columns;
    /// The persistent identifier of each row, in row order. This is what a cross-cell reference
    /// resolves through and what the persistence overlay keys on.
    Array<PersistentId> ids;
    u32 count = 0;

    explicit CookedBlock(Allocator& allocator) noexcept
        : components(allocator), columns(allocator), ids(allocator) {}
};

/// A payload the cell carries for one channel, independently requestable. Bulk data is addressed by
/// content through the asset system rather than held inline, so patching and deduplication work at
/// the chunk level; `bytes` is the small, cell-specific part that has no separate identity.
struct CellPayload {
    Channel channel = Channel::Entities;
    AssetId chunk;
    u64 size_bytes = 0;
};

/// A reference from one persistent entity to another, valid whether or not the target is loaded. A
/// raw pointer or a runtime entity identifier is never persisted.
enum class ReferencePolicy : u8 {
    /// May be unresolved; the holder handles absence.
    Soft = 0,
    /// Resolving it issues a streaming request.
    LoadOnDemand,
    /// The target must be resident whenever the holder is. These are what build the hard
    /// dependency closure the cooker reports on.
    RequireLoaded,
    /// The target lives with its owner rather than with a cell.
    FollowOwner,
};

struct PersistentReference {
    PersistentId holder;
    PersistentId target;
    /// The cell the target is cooked into, so that a `RequireLoaded` closure can be computed
    /// without loading anything.
    CellId target_cell;
    ReferencePolicy policy = ReferencePolicy::Soft;
};

/// A cooked cell: a multi-subsystem package, not a list of entities.
struct CookedCell {
    CellId id;
    CellCoord coord;
    /// The channels this cell was cooked with. A server cook omits the rendering ones, and the
    /// identity above is unchanged by that.
    ChannelMask channels = ChannelMask::all();
    Array<CookedBlock> blocks;
    Array<CellPayload> payloads;
    Array<AssetId> assets;
    Array<PersistentReference> references;
    /// Cells this one's activation forces resident, through `RequireLoaded` references. Computed by
    /// `close_hard_dependencies()`, not by hand.
    Array<CellId> hard_dependencies;
    /// The aggregate that stands in for this cell at distance. Nil when the cell has none.
    AssetId hlod_proxy;
    CellCost cost;

    explicit CookedCell(Allocator& allocator) noexcept
        : blocks(allocator),
          payloads(allocator),
          assets(allocator),
          references(allocator),
          hard_dependencies(allocator) {}

    CookedCell(const CookedCell&) = delete;
    CookedCell& operator=(const CookedCell&) = delete;
    CookedCell(CookedCell&&) noexcept = default;
    CookedCell& operator=(CookedCell&&) noexcept = default;
    ~CookedCell() = default;

    /// Rows across every block. The entity count the cost model reports.
    [[nodiscard]] u32 row_count() const noexcept;
};

/// Append one entity's row to a cell, in ECS-native form: it lands in the block for its
/// (archetype, layer), or starts one. `components`, `values` and `sizes` are parallel.
///
/// A free function rather than a `CellBuilder` method because the persistence overlay records
/// runtime-created entities in exactly this form — one code path stages authored and created rows,
/// which is what keeps "authored cells + persistence overlay = current world" from needing two
/// activation paths.
[[nodiscard]] Status append_cooked_row(Allocator& allocator, CookedCell& cell, PersistentId id,
                                       LayerId layer, Span<const ecs::ComponentTypeId> components,
                                       Span<const void* const> values,
                                       Span<const u32> sizes) noexcept;

/// Build one cell's entity data in ECS-native form. It exists so that a cook is a sequence of
/// `add_entity()` calls and the grouping by (archetype, layer) is done once, here, rather than by
/// every producer.
class CellBuilder {
public:
    CellBuilder(Allocator& allocator, CellId id, CellCoord coord) noexcept;

    /// Append one entity's row. `components` and `values` are parallel; `sizes` gives each value's
    /// byte length, which is the component's size in the runtime layout.
    [[nodiscard]] Status add_entity(PersistentId id, LayerId layer,
                                    Span<const ecs::ComponentTypeId> components,
                                    Span<const void* const> values, Span<const u32> sizes) noexcept;

    [[nodiscard]] Status add_payload(const CellPayload& payload) noexcept;
    [[nodiscard]] Status add_asset(AssetId asset) noexcept;
    [[nodiscard]] Status add_reference(const PersistentReference& reference) noexcept;
    void set_hlod_proxy(AssetId proxy) noexcept { cell_.hlod_proxy = proxy; }
    void set_channels(ChannelMask channels) noexcept { cell_.channels = channels; }

    /// Finish: compute the cost model and hand the cell over. The builder is empty afterwards.
    [[nodiscard]] CookedCell finish() noexcept;

private:
    Allocator* allocator_;
    CookedCell cell_;
};

/// Duplicate persistent identifiers are a cook error, and this is what reports them. Returns the
/// first duplicate found, so a diagnostic can name it.
struct ValidationResult {
    bool ok = true;
    PersistentId duplicate;
    /// A reference whose target is in no cell of the world. Named with its holder, per "Broken
    /// reference fails the build".
    PersistentId broken_holder;
    PersistentId broken_target;
    /// Set when an entity is too large for any partition level.
    PersistentId oversized;
    const char* message = "";
};

/// Validate one cell in isolation: duplicate identities within it, and payload/channel agreement.
[[nodiscard]] ValidationResult validate_cell(const CookedCell& cell) noexcept;

}  // namespace cy::world
