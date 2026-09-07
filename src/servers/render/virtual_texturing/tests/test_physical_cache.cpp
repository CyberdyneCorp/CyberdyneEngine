// The shared physical tile cache. M6 task 5.2.
//
// `virtual-texturing` — "Physical tile cache": caches are shared and grouped by format class, "Many
// virtual textures SHALL share one cache", capacity is a budget, "and eviction SHALL follow the
// shared residency policy". That last clause is the one a cache violates by accident, and the case
// "the cache refuses rather than choosing a victim" is what stops it.

#include <cy/servers/render/virtual_texturing/physical_cache.h>
#include <cy/test/test.h>

using namespace cy::render::vt;
using cy::u32;
using cy::u64;

namespace {

[[nodiscard]] TileCacheDesc cache_desc(u32 slots) noexcept {
    TileCacheDesc desc;
    desc.format = FormatClass::BlockColour;
    desc.tile_size = 128;
    desc.border = 4;
    desc.bytes_per_tile = 256;
    desc.tile_capacity = slots;
    return desc;
}

[[nodiscard]] u64 address_of(u32 texture, cy::u8 mip, u32 x) noexcept {
    VirtualAddress address;
    address.texture = texture;
    address.mip = mip;
    address.tile_x = static_cast<cy::u16>(x);
    return address.encode();
}

}  // namespace

CY_TEST_CASE("a cache without a tile size cannot be budgeted, and is refused") {
    PhysicalTileCache cache;
    TileCacheDesc desc = cache_desc(8);
    desc.bytes_per_tile = 0;
    CY_CHECK_FALSE(cache.configure(desc));

    desc = cache_desc(0);
    CY_CHECK_FALSE(cache.configure(desc));
    CY_CHECK_FALSE(cache.configured());
    // An unconfigured cache refuses rather than pretending it has room.
    CY_CHECK_FALSE(cache.acquire(address_of(1, 0, 0), false));
}

CY_TEST_CASE("a thousand virtual textures share one cache") {
    PhysicalTileCache cache;
    CY_REQUIRE(cache.configure(cache_desc(64)));

    // Sharing is by construction: the key is the encoded address, which carries the texture id in
    // its top bits, so there is no per-texture structure for a thousand textures to fragment into.
    for (u32 texture = 0; texture < 64; ++texture) {
        const auto tile = cache.acquire(address_of(texture, 0, 0), false);
        CY_REQUIRE(tile);
    }
    CY_CHECK_EQ(cache.occupancy(), 64U);
    CY_CHECK_EQ(cache.free_slots(), 0U);
    CY_CHECK_EQ(cache.bytes_in_use(), 64U * 256U);
    CY_CHECK_EQ(cache.budget_bytes(), 64U * 256U);
}

CY_TEST_CASE("a full cache refuses rather than choosing a victim") {
    PhysicalTileCache cache;
    CY_REQUIRE(cache.configure(cache_desc(2)));
    CY_REQUIRE(cache.acquire(address_of(1, 0, 0), false));
    CY_REQUIRE(cache.acquire(address_of(1, 0, 1), false));

    // THE CLAUSE THIS CASE EXISTS FOR. A cache that evicted its own least-recently-used tile here
    // would be a second residency policy, which is exactly what `residency` forbids: "rather than
    // each subsystem independently evicting — which produces one subsystem freeing memory another
    // immediately consumes". There is no method on this class that would choose one.
    CY_CHECK_FALSE(cache.acquire(address_of(1, 0, 2), false));
    CY_CHECK_EQ(cache.refusals(), 1U);
    CY_CHECK_EQ(cache.occupancy(), 2U);

    // The caller releases what the shared policy named, and then it fits.
    CY_CHECK(cache.release_address(address_of(1, 0, 0)));
    CY_CHECK(cache.acquire(address_of(1, 0, 2), false));
}

CY_TEST_CASE("acquiring a resident address returns the same tile and may add the pin") {
    PhysicalTileCache cache;
    CY_REQUIRE(cache.configure(cache_desc(4)));

    const auto first = cache.acquire(address_of(1, 0, 0), false);
    CY_REQUIRE(first);
    CY_CHECK_EQ(cache.pinned_count(), 0U);

    // The mip tail is often already resident by the time it is made permanent.
    const auto again = cache.acquire(address_of(1, 0, 0), true);
    CY_REQUIRE(again);
    CY_CHECK_EQ(*again, *first);
    CY_CHECK_EQ(cache.occupancy(), 1U);
    CY_CHECK_EQ(cache.pinned_count(), 1U);
}

CY_TEST_CASE("a pin is a fact: releasing a pinned tile is refused until it is unpinned") {
    PhysicalTileCache cache;
    CY_REQUIRE(cache.configure(cache_desc(4)));
    const auto tile = cache.acquire(address_of(1, 7, 0), true);
    CY_REQUIRE(tile);

    CY_CHECK_FALSE(cache.release(*tile));
    CY_CHECK_FALSE(cache.release_address(address_of(1, 7, 0)));
    CY_CHECK_EQ(cache.occupancy(), 1U);

    CY_CHECK(cache.set_pinned(*tile, false));
    CY_CHECK_EQ(cache.pinned_count(), 0U);
    CY_CHECK(cache.release(*tile));
    CY_CHECK_EQ(cache.occupancy(), 0U);
}

CY_TEST_CASE("a tile's staging bytes belong to it alone") {
    PhysicalTileCache cache;
    CY_REQUIRE(cache.configure(cache_desc(4)));
    const auto first = cache.acquire(address_of(1, 0, 0), false);
    const auto second = cache.acquire(address_of(1, 0, 1), false);
    CY_REQUIRE(first);
    CY_REQUIRE(second);

    cy::u8* one = cache.tile_data(*first);
    cy::u8* two = cache.tile_data(*second);
    CY_REQUIRE(one != nullptr);
    CY_REQUIRE(two != nullptr);
    CY_CHECK(one != two);

    for (u32 index = 0; index < 256; ++index) {
        one[index] = 0xAB;
        two[index] = 0xCD;
    }
    CY_CHECK_EQ(one[255], 0xAB);
    CY_CHECK_EQ(two[0], 0xCD);

    CY_CHECK(cache.tile_data(cache.capacity()) == nullptr);
}

CY_TEST_CASE("find answers the tile, or says there is none") {
    PhysicalTileCache cache;
    CY_REQUIRE(cache.configure(cache_desc(4)));
    CY_CHECK_EQ(cache.find(address_of(1, 0, 0)), kNoPhysicalTile);

    const auto tile = cache.acquire(address_of(1, 0, 0), false);
    CY_REQUIRE(tile);
    CY_CHECK_EQ(cache.find(address_of(1, 0, 0)), *tile);
    CY_REQUIRE(cache.slot(*tile) != nullptr);
    CY_CHECK(cache.slot(*tile)->occupied);
    CY_CHECK_EQ(cache.slot(*tile)->address, address_of(1, 0, 0));

    CY_CHECK(cache.release(*tile));
    CY_CHECK_EQ(cache.find(address_of(1, 0, 0)), kNoPhysicalTile);
}

CY_TEST_CASE("clear takes the pins with it, because the thing that was pinning is gone") {
    PhysicalTileCache cache;
    CY_REQUIRE(cache.configure(cache_desc(4)));
    CY_REQUIRE(cache.acquire(address_of(1, 7, 0), true));
    CY_REQUIRE(cache.acquire(address_of(2, 7, 0), true));
    CY_CHECK_EQ(cache.pinned_count(), 2U);

    cache.clear();
    CY_CHECK_EQ(cache.occupancy(), 0U);
    CY_CHECK_EQ(cache.pinned_count(), 0U);
    CY_CHECK_FALSE(cache.configured());
}
