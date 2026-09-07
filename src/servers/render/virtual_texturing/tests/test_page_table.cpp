// Page tables: one lookup over two representations, and one batch per frame. M6 task 5.1.
//
// `virtual-texturing` — "Page tables". "The page table implementation — flat or hierarchical —
// SHALL be an internal decision hidden behind the lookup", and "Page table updates SHALL be applied
// on the GPU without a CPU round trip per page". The first is checked by running the same
// properties over both representations; the second by counting batches against updates.

#include <cy/servers/render/virtual_texturing/page_table.h>
#include <cy/test/test.h>

using namespace cy::render::vt;
using cy::u32;
using cy::usize;

namespace {

/// Small enough to be flat: 8 mips over a 1024-texel edge at 128-texel tiles is 85 pages.
[[nodiscard]] VirtualTextureDesc small() noexcept {
    VirtualTextureDesc desc;
    desc.id = 1;
    desc.width = 1024;
    desc.height = 1024;
    desc.tile_size = 128;
    desc.border = 4;
    desc.mip_count = 4;
    desc.mip_tail_levels = 1;
    desc.bytes_per_tile = 16384;
    return desc;
}

/// Large enough to be sparse: mip 0 alone is 4096 x 4096 tiles' worth of address space.
[[nodiscard]] VirtualTextureDesc large() noexcept {
    VirtualTextureDesc desc = small();
    desc.id = 2;
    desc.width = 524288;
    desc.height = 524288;
    desc.mip_count = 12;
    desc.mip_tail_levels = 2;
    return desc;
}

[[nodiscard]] VirtualAddress at(u32 texture, cy::u8 mip, u32 x, u32 y) noexcept {
    VirtualAddress address;
    address.texture = texture;
    address.mip = mip;
    address.tile_x = static_cast<cy::u16>(x);
    address.tile_y = static_cast<cy::u16>(y);
    return address;
}

[[nodiscard]] PageTableEntry resident(u32 tile, cy::u8 mip) noexcept {
    PageTableEntry entry;
    entry.physical_tile = tile;
    entry.resident_mip = mip;
    entry.flags = PageFlags::kResident;
    return entry;
}

/// The properties that must hold whatever the internal representation is.
void check_lookup_properties(PageTable& table, const VirtualTextureDesc& desc) {
    // Never written reads as invalid, not as a failure — a sampler cannot handle an `Expected`.
    const PageTableEntry blank = table.lookup(at(desc.id, 0, 0, 0));
    CY_CHECK_FALSE(blank.resident());
    CY_CHECK_EQ(blank.physical_tile, kNoPhysicalTile);
    CY_CHECK_EQ(blank.flags & PageFlags::kInvalid, PageFlags::kInvalid);

    // Nothing is visible until the batch is applied.
    CY_REQUIRE(table.stage(at(desc.id, 1, 2, 3), resident(42, 1)));
    CY_CHECK_FALSE(table.lookup(at(desc.id, 1, 2, 3)).resident());
    CY_CHECK_EQ(table.apply_staged(), 1U);
    CY_CHECK(table.lookup(at(desc.id, 1, 2, 3)).resident());
    CY_CHECK_EQ(table.lookup(at(desc.id, 1, 2, 3)).physical_tile, 42U);

    // Another texture's address is out of range for this table.
    CY_CHECK_FALSE(table.stage(at(desc.id + 100, 1, 2, 3), resident(1, 1)));
    // So is a tile beyond the level's extent, and a mip beyond the pyramid.
    CY_CHECK_FALSE(table.stage(at(desc.id, 0, desc.tiles_x(0), 0), resident(1, 0)));
    CY_CHECK_FALSE(table.stage(at(desc.id, desc.mip_count, 0, 0), resident(1, 0)));
}

}  // namespace

CY_TEST_CASE("a small address space is flat and a large one is sparse") {
    PageTable flat;
    CY_REQUIRE(flat.configure(small()));
    CY_CHECK(flat.is_flat());

    PageTable sparse;
    CY_REQUIRE(sparse.configure(large()));
    CY_CHECK_FALSE(sparse.is_flat());
}

CY_TEST_CASE("the lookup answers identically whichever representation is in force") {
    // The SAME properties, run twice. If the two ever disagree, this case says so — which is what
    // makes "an internal decision hidden behind the lookup" a checked claim.
    PageTable flat;
    CY_REQUIRE(flat.configure(small()));
    CY_REQUIRE(flat.is_flat());
    check_lookup_properties(flat, small());

    PageTable sparse;
    CY_REQUIRE(sparse.configure(large()));
    CY_REQUIRE_FALSE(sparse.is_flat());
    check_lookup_properties(sparse, large());
}

CY_TEST_CASE("many pages becoming resident is one batch, not one call each") {
    PageTable table;
    CY_REQUIRE(table.configure(large()));

    // The COUNT is not the property — one batch whatever the count is. Sized for the Debug
    // profile's 1 ms unit budget, and the per-iteration result accumulated rather than asserted,
    // because a doctest assertion costs more than the call it is checking.
    constexpr u32 kPages = 400;
    bool every_stage_accepted = true;
    for (u32 index = 0; index < kPages; ++index) {
        every_stage_accepted =
            table.stage(at(2, 4, index % 100, index / 100), resident(index, 4)).has_value() &&
            every_stage_accepted;
    }
    CY_CHECK(every_stage_accepted);
    CY_CHECK_EQ(table.staged_count(), kPages);
    CY_CHECK_EQ(table.batches_applied(), 0U);

    CY_CHECK_EQ(table.apply_staged(), kPages);
    CY_CHECK_EQ(table.batches_applied(), 1U);
    CY_CHECK_EQ(table.last_batch_size(), kPages);
    CY_CHECK_EQ(table.staged_count(), 0U);
    CY_CHECK_EQ(table.resident_entries(), kPages);

    // An empty apply is not a batch: a frame that made nothing resident uploads nothing.
    CY_CHECK_EQ(table.apply_staged(), 0U);
    CY_CHECK_EQ(table.batches_applied(), 1U);
}

CY_TEST_CASE("the generation advances on every rewrite, so a stale read is detectable") {
    PageTable table;
    CY_REQUIRE(table.configure(small()));

    CY_REQUIRE(table.stage(at(1, 0, 0, 0), resident(7, 0)));
    table.apply_staged();
    const cy::u16 first = table.lookup(at(1, 0, 0, 0)).generation;

    CY_REQUIRE(table.stage(at(1, 0, 0, 0), resident(9, 0)));
    table.apply_staged();
    CY_CHECK_EQ(table.lookup(at(1, 0, 0, 0)).generation, static_cast<cy::u16>(first + 1));
    CY_CHECK_EQ(table.lookup(at(1, 0, 0, 0)).physical_tile, 9U);
}

CY_TEST_CASE("invalidation reaches the declared depth and no further") {
    PageTable table;
    CY_REQUIRE(table.configure(small()));

    // Fill mip 2 (2 x 2 tiles) and mip 1 (4 x 4) and mip 0 (8 x 8).
    for (cy::u8 mip = 0; mip < 3; ++mip) {
        for (u32 y = 0; y < small().tiles_y(mip); ++y) {
            for (u32 x = 0; x < small().tiles_x(mip); ++x) {
                CY_REQUIRE(table.stage(at(1, mip, x, y), resident(1, mip)));
            }
        }
    }
    table.apply_staged();
    const usize before = table.resident_entries();

    // Depth zero touches the named page alone.
    CY_REQUIRE(table.invalidate(at(1, 2, 0, 0), 0));
    table.apply_staged();
    CY_CHECK_EQ(table.resident_entries(), before - 1);

    // Depth one adds the four pages of mip 1 under it, and stops there — the sixteen of mip 0 are
    // untouched, because each level below quadruples the pages and the caller says how deep.
    CY_REQUIRE(table.invalidate(at(1, 2, 1, 0), 1));
    table.apply_staged();
    CY_CHECK_EQ(table.resident_entries(), before - 6);
    CY_CHECK(table.lookup(at(1, 0, 4, 0)).resident());
}

CY_TEST_CASE("a table that was never configured refuses rather than pretending") {
    PageTable table;
    CY_CHECK_FALSE(table.configured());
    CY_CHECK_FALSE(table.lookup(at(1, 0, 0, 0)).resident());
    CY_CHECK_FALSE(table.stage(at(1, 0, 0, 0), resident(1, 0)));
    CY_CHECK_FALSE(table.invalidate(at(1, 0, 0, 0), 0));

    VirtualTextureDesc broken = small();
    broken.mip_tail_levels = 0;
    CY_CHECK_FALSE(table.configure(broken));
}
