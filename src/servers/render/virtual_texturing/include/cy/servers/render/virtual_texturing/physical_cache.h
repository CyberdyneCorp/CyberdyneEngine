#pragma once
// The physical tile cache: shared, grouped by format class, and never the owner of the policy that
// evicts from it. Task 5.2.
//
// `virtual-texturing` — "Physical tile cache": tiles live in "shared caches grouped by format class
// — block-compressed colour, two-channel normal, single-channel mask, high dynamic range — rather
// than one physical cache per asset". "Many virtual textures SHALL share one cache, so that a
// project with thousands of virtual textures does not fragment its memory across thousands of
// allocations." Capacity is a budget from the memory budget tree, "and eviction SHALL follow the
// **shared residency policy**".
//
// --- WHY `acquire()` REFUSES INSTEAD OF EVICTING
// -----------------------------------------------------
//
// That last clause is the design constraint, and it is easy to violate by accident. A cache that
// evicted its own least-recently-used tile when it ran out would be a second residency policy, and
// `residency` says so in as many words: "rather than each subsystem independently evicting — which
// produces one subsystem freeing memory another immediately consumes".
//
// So `acquire()` returns `Unavailable` when the cache is full. The caller — `VirtualTextureSystem`,
// which holds the residency server — asks the shared policy which tile should go, releases that
// one, and acquires again. The cache owns storage; residency owns the decision. There is
// deliberately no method here that chooses a victim.
//
// --- SHARING IS BY CONSTRUCTION
// ------------------------------------------------------------------------
//
// A cache is keyed on the ENCODED VIRTUAL ADDRESS, which carries the virtual texture id in its top
// bits. Two textures of the same format class therefore share one cache because there is no per-
// texture structure to put them in — a thousand virtual textures are a thousand key prefixes, not a
// thousand allocations.
//
// --- WHAT THE STAGING BYTES ARE, AND ARE NOT
// -----------------------------------------------------------
//
// This module is LAYER 2 and may not name a device or an image (see `src/servers/CMakeLists.txt`).
// The bytes a cache owns are therefore the CPU-side staging mirror the uploader copies from — the
// thing a runtime producer writes into and a disk tile is decompressed into. The GPU-resident array
// lives above this layer and is addressed by the same tile index, which is what makes the index
// meaningful on both sides.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/servers/render/virtual_texturing/address.h>
#include <cy/servers/render/virtual_texturing/page_table.h>

namespace cy::render::vt {

/// What one cache is. Tile size and border are per cache rather than per texture, because a shared
/// cache can only hold tiles of one shape — which is the constraint that makes tile size "a cooker
/// and platform policy" rather than a per-asset choice.
struct TileCacheDesc {
    FormatClass format = FormatClass::BlockColour;
    u16 tile_size = 128;
    u8 border = 4;
    /// Bytes one tile occupies, border included, in its GPU-ready block-compressed form.
    u32 bytes_per_tile = 0;
    /// Slots. Capacity in bytes is `tile_capacity * bytes_per_tile`, and that is the number the
    /// memory budget tree apportions.
    u32 tile_capacity = 0;
};

/// One slot's occupancy. `pinned` is the mip tail and anything the residency layer holds; a pinned
/// slot is not a candidate the policy will ever be offered, because it cannot be released.
struct TileSlot {
    u64 address = 0;
    bool occupied = false;
    bool pinned = false;
};

class PhysicalTileCache {
public:
    explicit PhysicalTileCache(Allocator& allocator = current_allocator()) noexcept
        : slots_(allocator), free_list_(allocator), by_address_(allocator), staging_(allocator) {}

    PhysicalTileCache(const PhysicalTileCache&) = delete;
    PhysicalTileCache& operator=(const PhysicalTileCache&) = delete;
    PhysicalTileCache(PhysicalTileCache&&) noexcept = default;
    PhysicalTileCache& operator=(PhysicalTileCache&&) noexcept = default;
    ~PhysicalTileCache() = default;

    Status configure(const TileCacheDesc& desc) noexcept;
    [[nodiscard]] bool configured() const noexcept { return configured_; }
    [[nodiscard]] const TileCacheDesc& description() const noexcept { return desc_; }

    /// Take a slot for `address`. `Unavailable` when full — see the header note: choosing a victim
    /// is the residency layer's, and there is no method here that would do it.
    [[nodiscard]] Expected<u32, Error> acquire(u64 encoded_address, bool pinned) noexcept;
    /// Give a slot back. Refuses a pinned slot: a pin is a fact, not a preference.
    bool release(u32 tile) noexcept;
    bool release_address(u64 encoded_address) noexcept;
    /// Change a slot's pinned state — what making a mip tail resident, and later releasing a
    /// residency hold, actually does to the cache.
    bool set_pinned(u32 tile, bool pinned) noexcept;

    [[nodiscard]] u32 find(u64 encoded_address) const noexcept;
    [[nodiscard]] const TileSlot* slot(u32 tile) const noexcept;

    /// The staging bytes for one tile. Empty when the cache was configured with no tile size.
    [[nodiscard]] u8* tile_data(u32 tile) noexcept;
    [[nodiscard]] const u8* tile_data(u32 tile) const noexcept;

    [[nodiscard]] u32 capacity() const noexcept { return desc_.tile_capacity; }
    [[nodiscard]] u32 occupancy() const noexcept { return occupancy_; }
    [[nodiscard]] u32 pinned_count() const noexcept { return pinned_; }
    [[nodiscard]] u32 free_slots() const noexcept { return desc_.tile_capacity - occupancy_; }
    [[nodiscard]] u64 budget_bytes() const noexcept {
        return static_cast<u64>(desc_.tile_capacity) * desc_.bytes_per_tile;
    }
    [[nodiscard]] u64 bytes_in_use() const noexcept {
        return static_cast<u64>(occupancy_) * desc_.bytes_per_tile;
    }
    [[nodiscard]] u64 acquisitions() const noexcept { return acquisitions_; }
    [[nodiscard]] u64 refusals() const noexcept { return refusals_; }

    /// Drop everything, pins included. What tearing a world down does to a cache; the pinned check
    /// in `release` deliberately does not apply, because the thing that was pinning is gone.
    void clear() noexcept;

private:
    TileCacheDesc desc_;
    Array<TileSlot> slots_;
    Array<u32> free_list_;
    HashMap<u64, u32> by_address_;
    Array<u8> staging_;
    u32 occupancy_ = 0;
    u32 pinned_ = 0;
    u64 acquisitions_ = 0;
    u64 refusals_ = 0;
    bool configured_ = false;
};

}  // namespace cy::render::vt
