// SPDX-License-Identifier: MIT
// The device's card lookup grid, built and updated. Issue #35.

#include <cy/rendering/gi/card_lighting.h>
#include <cy/rendering/gi/surface_cache.h>
#include <cy/test/test.h>

#include <algorithm>
#include <vector>

namespace gi = cy::rendering::gi;
using cy::f32;
using cy::u32;

// --- The card neighbour grid ---------------------------------------------------------------------
//
// `CardNeighbourGrid::build` counted the occupied cells by sorting every (cell, card) entry, and
// every layout change of a surface cache — a cell streamed in, an object moved — paid that sort
// over the whole cache. It now counts them with a hash set and sorts only within each bucket. Held
// to the build as it was, written out here, buffer for buffer.

namespace {

struct ReferenceEntry {
    cy::i32 cell[3] = {0, 0, 0};
    u32 handle = 0;
    u32 bucket = 0;
};

[[nodiscard]] f32 reference_gap(f32 value, cy::i32 index, f32 cell) noexcept {
    return std::max({(static_cast<f32>(index) * cell) - value,
                     value - (static_cast<f32>(index + 1) * cell), 0.0F});
}

void reference_build(const std::vector<gi::SurfacePage>& pages, f32 radius, u32 max_buckets,
                     std::vector<u32>& ranges, std::vector<u32>& handles) {
    const f32 cell = std::max(radius, 1.0e-3F);
    std::vector<ReferenceEntry> entries;
    for (u32 handle = 0; handle < pages.size(); ++handle) {
        if (!pages[handle].live) {
            continue;
        }
        const cy::Vec3 p = pages[handle].position;
        const f32 reach = (cell * cell * 1.001F) + 1.0e-6F;
        const cy::i32 lo[3] = {gi::CardGrid::cell_of(p.x - cell, cell),
                               gi::CardGrid::cell_of(p.y - cell, cell),
                               gi::CardGrid::cell_of(p.z - cell, cell)};
        const cy::i32 hi[3] = {gi::CardGrid::cell_of(p.x + cell, cell),
                               gi::CardGrid::cell_of(p.y + cell, cell),
                               gi::CardGrid::cell_of(p.z + cell, cell)};
        for (cy::i32 z = lo[2]; z <= hi[2]; ++z) {
            for (cy::i32 y = lo[1]; y <= hi[1]; ++y) {
                for (cy::i32 x = lo[0]; x <= hi[0]; ++x) {
                    const f32 dx = reference_gap(p.x, x, cell);
                    const f32 dy = reference_gap(p.y, y, cell);
                    const f32 dz = reference_gap(p.z, z, cell);
                    if ((dx * dx) + (dy * dy) + (dz * dz) <= reach) {
                        entries.push_back(ReferenceEntry{{x, y, z}, handle, 0});
                    }
                }
            }
        }
    }
    std::ranges::sort(entries, [](const ReferenceEntry& a, const ReferenceEntry& b) {
        if (a.cell[2] != b.cell[2]) {
            return a.cell[2] < b.cell[2];
        }
        if (a.cell[1] != b.cell[1]) {
            return a.cell[1] < b.cell[1];
        }
        if (a.cell[0] != b.cell[0]) {
            return a.cell[0] < b.cell[0];
        }
        return a.handle < b.handle;
    });
    u32 cells = 0;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const bool fresh = index == 0 || entries[index].cell[0] != entries[index - 1].cell[0] ||
                           entries[index].cell[1] != entries[index - 1].cell[1] ||
                           entries[index].cell[2] != entries[index - 1].cell[2];
        cells += fresh ? 1U : 0U;
    }
    u32 buckets = 64;
    while (buckets < cells * 2U && buckets < max_buckets) {
        buckets <<= 1U;
    }
    ranges.assign(static_cast<std::size_t>(buckets) * 2U, 0U);
    for (ReferenceEntry& entry : entries) {
        entry.bucket =
            gi::CardGrid::bucket_of(entry.cell[0], entry.cell[1], entry.cell[2], buckets);
        ranges[(static_cast<std::size_t>(entry.bucket) * 2U) + 1U] += 1U;
    }
    u32 running = 0;
    for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
        ranges[bucket * 2U] = running;
        running += ranges[(bucket * 2U) + 1U];
    }
    handles.assign(running, 0U);
    std::vector<u32> cursor(buckets);
    for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
        cursor[bucket] = ranges[bucket * 2U];
    }
    for (const ReferenceEntry& entry : entries) {
        handles[cursor[entry.bucket]++] = entry.handle;
    }
}

}  // namespace

CY_TEST_CASE("the card neighbour grid is the grid the full sort built, buffer for buffer") {
    std::vector<gi::SurfacePage> cards(160);
    u32 state = 4242U;
    for (gi::SurfacePage& card : cards) {
        state = (state * 1664525U) + 1013904223U;
        card.live = (state >> 7U) % 6U != 0U;
        // On a half-metre lattice and between it, negative coordinates included: cards that share
        // cells, and cells that share buckets.
        card.position =
            cy::Vec3{static_cast<f32>(static_cast<cy::i32>((state >> 9U) % 24U) - 12) * 0.25F,
                     static_cast<f32>((state >> 14U) % 6U) * 0.5F,
                     static_cast<f32>(static_cast<cy::i32>((state >> 18U) % 20U) - 10) * 0.3F};
    }
    for (const u32 max_buckets : {64U, 1U << 16U}) {
        gi::CardNeighbourGrid grid;
        CY_REQUIRE(grid.build({cards.data(), cards.size()}, 0.75F, max_buckets).has_value());
        std::vector<u32> ranges;
        std::vector<u32> handles;
        reference_build(cards, 0.75F, max_buckets, ranges, handles);
        CY_REQUIRE_EQ(grid.ranges().size(), ranges.size());
        CY_REQUIRE_EQ(grid.handles().size(), handles.size());
        bool same = true;
        for (std::size_t index = 0; index < ranges.size(); ++index) {
            same = same && grid.ranges()[index] == ranges[index];
        }
        for (std::size_t index = 0; index < handles.size(); ++index) {
            same = same && grid.handles()[index] == handles[index];
        }
        CY_CHECK(same);
    }
}

CY_TEST_CASE("the card neighbour grid updated for the cards that moved is the grid built anew") {
    std::vector<gi::SurfacePage> cards(160);
    u32 state = 99U;
    const auto place = [&state](gi::SurfacePage& card) {
        state = (state * 1664525U) + 1013904223U;
        card.position =
            cy::Vec3{static_cast<f32>(static_cast<cy::i32>((state >> 9U) % 24U) - 12) * 0.25F,
                     static_cast<f32>((state >> 14U) % 6U) * 0.5F,
                     static_cast<f32>(static_cast<cy::i32>((state >> 18U) % 20U) - 10) * 0.3F};
    };
    for (gi::SurfacePage& card : cards) {
        card.live = true;
        place(card);
    }
    gi::CardNeighbourGrid updated;
    CY_REQUIRE(updated.build({cards.data(), cards.size()}, 0.75F, 1U << 16U).has_value());
    for (u32 round = 0; round < 4; ++round) {
        // A few cards move, one dies, one is born (a cell streams out and in, an object moves);
        // the list names one of them twice.
        std::vector<u32> changed;
        for (u32 pick = 0; pick < 9; ++pick) {
            const u32 handle = ((round * 37U) + (pick * 17U)) % static_cast<u32>(cards.size());
            if (pick == 0) {
                cards[handle].live = !cards[handle].live;
            } else {
                place(cards[handle]);
            }
            changed.push_back(handle);
        }
        changed.push_back(changed.front());
        // A grown cache: a page allocated past the end.
        gi::SurfacePage born;
        born.live = true;
        place(born);
        cards.push_back(born);
        changed.push_back(static_cast<u32>(cards.size() - 1U));

        const cy::Expected<bool, cy::Error> result = updated.update(
            {cards.data(), cards.size()}, {changed.data(), changed.size()}, 0.75F, 1U << 16U);
        CY_REQUIRE(result.has_value());
        gi::CardNeighbourGrid built;
        CY_REQUIRE(
            built.build({cards.data(), cards.size()}, 0.75F, 1U << 16U, updated.bucket_count())
                .has_value());
        CY_REQUIRE_EQ(updated.ranges().size(), built.ranges().size());
        CY_REQUIRE_EQ(updated.handles().size(), built.handles().size());
        bool same = true;
        for (std::size_t index = 0; index < built.ranges().size(); ++index) {
            same = same && updated.ranges()[index] == built.ranges()[index];
        }
        for (std::size_t index = 0; index < built.handles().size(); ++index) {
            same = same && updated.handles()[index] == built.handles()[index] &&
                   updated.positions()[index].x == built.positions()[index].x &&
                   updated.positions()[index].y == built.positions()[index].y &&
                   updated.positions()[index].z == built.positions()[index].z;
        }
        CY_CHECK(same);
    }
}
