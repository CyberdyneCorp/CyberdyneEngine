// The four attribution axes: by type, by thread, by world cell, by asset. M7 task 3.1.
//
// `core-memory-and-containers` — "Memory diagnostics": "Reporting SHALL be attributable along the
// axes that answer real questions: by domain, by type, by thread, by world cell, and by asset — so
// that 'why is this region consuming this much' is answerable."
//
// M1 built the domain axis and the tier record has said "three of five" ever since. These are the
// other four, and the case that matters most is the last one: a report whose table is too small
// must SAY so, because a report that silently showed the biggest rows is a report whose numbers do
// not add up and nobody can tell.
//
// Unit: no I/O, one thread except where the thread axis needs a second.

#include <cy/core/memory/attribution.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/memory/tracking_allocator.h>
#include <cy/test/test.h>

#include <thread>

using cy::AttributionAxis;
using cy::MemoryAttribution;
using cy::MemoryAttributionRow;
using cy::MemoryAttributionScope;
using cy::MemoryAttributionSummary;
using cy::u32;
using cy::u64;

namespace {

MemoryAttribution by_cell(u64 cell) {
    MemoryAttribution attribution;
    attribution.world_cell = cell;
    return attribution;
}

MemoryAttribution by_asset(u64 high, u64 low) {
    MemoryAttribution attribution;
    attribution.asset_high = high;
    attribution.asset_low = low;
    return attribution;
}

MemoryAttribution by_type(u64 type) {
    MemoryAttribution attribution;
    attribution.type = type;
    return attribution;
}

}  // namespace

CY_TEST_CASE("attribution: a scope merges over the enclosing one rather than replacing it") {
    // A cell activation pushes a cell; the mesh loader inside it pushes an asset; the container
    // inside that pushes a type. None of the three knows the other two, and every one of them ends
    // up on the allocation.
    CY_CHECK(cy::current_attribution().is_empty());
    {
        const MemoryAttributionScope cell(by_cell(0x4200));
        CY_CHECK(cy::current_attribution().world_cell == 0x4200u);
        {
            const MemoryAttributionScope asset(by_asset(7, 9));
            CY_CHECK(cy::current_attribution().world_cell == 0x4200u);
            CY_CHECK(cy::current_attribution().asset_high == 7u);
            {
                const MemoryAttributionScope type(by_type(31));
                const MemoryAttribution& now = cy::current_attribution();
                CY_CHECK(now.world_cell == 0x4200u);
                CY_CHECK(now.asset_low == 9u);
                CY_CHECK(now.type == 31u);
            }
            CY_CHECK(cy::current_attribution().type == 0u);
        }
        CY_CHECK(cy::current_attribution().asset_low == 0u);
    }
    CY_CHECK(cy::current_attribution().is_empty());
}

CY_TEST_CASE("attribution: live bytes group by world cell") {
    cy::TrackingAllocator tracker(cy::default_allocator(), cy::MemoryDomain::World, "cells");

    void* a = nullptr;
    void* b = nullptr;
    void* c = nullptr;
    {
        const MemoryAttributionScope scope(by_cell(11));
        a = tracker.allocate(1024, 16);
        b = tracker.allocate(512, 16);
    }
    {
        const MemoryAttributionScope scope(by_cell(12));
        c = tracker.allocate(256, 16);
    }
    CY_REQUIRE(a != nullptr);
    CY_REQUIRE(b != nullptr);
    CY_REQUIRE(c != nullptr);

    MemoryAttributionRow rows[4] = {};
    const MemoryAttributionSummary summary = tracker.report_attribution(
        AttributionAxis::WorldCell, cy::Span<MemoryAttributionRow>(rows));
    CY_CHECK_EQ(summary.distinct_keys, 2u);
    CY_CHECK_EQ(summary.rows, 2u);
    // Largest first, which is the order the question "why is this region consuming this much" is
    // asked in.
    CY_CHECK_EQ(rows[0].key, 11u);
    CY_CHECK_EQ(rows[0].live_bytes, 1536u);
    CY_CHECK_EQ(rows[0].live_allocations, 2u);
    CY_CHECK_EQ(rows[1].key, 12u);
    CY_CHECK_EQ(rows[1].live_bytes, 256u);
    CY_CHECK_EQ(summary.unattributed_bytes, 0u);

    tracker.deallocate(a, 1024, 16);
    tracker.deallocate(b, 512, 16);
    tracker.deallocate(c, 256, 16);
}

CY_TEST_CASE("attribution: an asset key keeps both halves of a 128-bit identity") {
    // Folding an AssetId to 64 bits would attribute two assets to one row at a rate nobody would
    // ever notice was happening, so the two that differ only in the high half must not merge.
    cy::TrackingAllocator tracker(cy::default_allocator(), cy::MemoryDomain::Assets, "assets");

    void* first = nullptr;
    void* second = nullptr;
    {
        const MemoryAttributionScope scope(by_asset(1, 0xdead'beefULL));
        first = tracker.allocate(2048, 16);
    }
    {
        const MemoryAttributionScope scope(by_asset(2, 0xdead'beefULL));
        second = tracker.allocate(128, 16);
    }
    CY_REQUIRE(first != nullptr);
    CY_REQUIRE(second != nullptr);

    MemoryAttributionRow rows[4] = {};
    const MemoryAttributionSummary summary =
        tracker.report_attribution(AttributionAxis::Asset, cy::Span<MemoryAttributionRow>(rows));
    CY_CHECK_EQ(summary.distinct_keys, 2u);
    CY_CHECK_EQ(rows[0].key, 0xdead'beefULL);
    CY_CHECK_EQ(rows[0].key_high, 1u);
    CY_CHECK_EQ(rows[0].live_bytes, 2048u);
    CY_CHECK_EQ(rows[1].key_high, 2u);

    tracker.deallocate(first, 2048, 16);
    tracker.deallocate(second, 128, 16);
}

CY_TEST_CASE("attribution: the thread axis is captured, not declared") {
    cy::TrackingAllocator tracker(cy::default_allocator(), cy::MemoryDomain::Engine, "threads");

    void* here = tracker.allocate(64, 16);
    CY_REQUIRE(here != nullptr);
    const u32 this_thread = cy::current_thread_ordinal();

    void* elsewhere = nullptr;
    u32 other_thread = 0;
    // `TrackingAllocator` is not thread-safe, so the second thread runs alone: the point is that
    // the ordinal is taken from the allocating thread, not that two threads may allocate at once.
    std::thread worker([&] {
        other_thread = cy::current_thread_ordinal();
        elsewhere = tracker.allocate(4096, 16);
    });
    worker.join();
    CY_REQUIRE(elsewhere != nullptr);
    CY_CHECK(other_thread != this_thread);

    MemoryAttributionRow rows[4] = {};
    const MemoryAttributionSummary summary =
        tracker.report_attribution(AttributionAxis::Thread, cy::Span<MemoryAttributionRow>(rows));
    CY_CHECK_EQ(summary.distinct_keys, 2u);
    CY_CHECK_EQ(rows[0].key, other_thread);
    CY_CHECK_EQ(rows[0].live_bytes, 4096u);
    CY_CHECK_EQ(rows[1].key, this_thread);
    // Nothing declared a thread and every allocation still has one.
    CY_CHECK_EQ(summary.unattributed_bytes, 0u);

    tracker.deallocate(here, 64, 16);
    tracker.deallocate(elsewhere, 4096, 16);
}

CY_TEST_CASE("attribution: an axis nobody declared is reported as unattributed, not as key zero") {
    cy::TrackingAllocator tracker(cy::default_allocator(), cy::MemoryDomain::Engine, "bare");

    void* bare = tracker.allocate(777, 16);
    CY_REQUIRE(bare != nullptr);

    MemoryAttributionRow rows[4] = {};
    const MemoryAttributionSummary summary =
        tracker.report_attribution(AttributionAxis::Type, cy::Span<MemoryAttributionRow>(rows));
    CY_CHECK_EQ(summary.rows, 0u);
    CY_CHECK_EQ(summary.distinct_keys, 0u);
    CY_CHECK_EQ(summary.unattributed_bytes, 777u);
    CY_CHECK_EQ(summary.reported_bytes, 0u);

    tracker.deallocate(bare, 777, 16);
}

CY_TEST_CASE("attribution: a table too small for the keys says so rather than quietly truncating") {
    // The failure this case exists to prevent: a report that showed the biggest two of five and
    // said nothing, so the rows do not add up to the total and the reader concludes the total is
    // wrong.
    cy::TrackingAllocator tracker(cy::default_allocator(), cy::MemoryDomain::World, "many-cells");

    void* held[5] = {};
    for (u32 index = 0; index < 5; ++index) {
        const MemoryAttributionScope scope(by_cell(100 + index));
        held[index] = tracker.allocate(64, 16);
        CY_REQUIRE(held[index] != nullptr);
    }

    MemoryAttributionRow rows[2] = {};
    const MemoryAttributionSummary summary = tracker.report_attribution(
        AttributionAxis::WorldCell, cy::Span<MemoryAttributionRow>(rows));
    CY_CHECK_EQ(summary.rows, 2u);
    CY_CHECK_EQ(summary.distinct_keys, 5u);
    CY_CHECK_EQ(summary.reported_bytes, 128u);
    CY_CHECK_EQ(summary.unreported_bytes, 192u);
    CY_CHECK_EQ(summary.reported_bytes + summary.unreported_bytes + summary.unattributed_bytes,
                tracker.live_bytes());

    for (void* block : held) {
        tracker.deallocate(block, 64, 16);
    }
}

CY_TEST_CASE("attribution: a leak report names the cell and the asset that leaked") {
    cy::TrackingAllocator tracker(cy::default_allocator(), cy::MemoryDomain::Assets, "leaky");
    {
        const MemoryAttributionScope cell(by_cell(0x99));
        const MemoryAttributionScope asset(by_asset(0, 0x5150));
        (void)tracker.allocate(321, 16);
    }

    struct Found {
        u64 cell = 0;
        u64 asset = 0;
        u32 thread = 0;
        u32 count = 0;
    } found;

    const cy::LeakReport report = tracker.report_leaks(
        [](const cy::TrackedAllocation& allocation, void* user) noexcept {
            auto* out = static_cast<Found*>(user);
            out->cell = allocation.attribution.world_cell;
            out->asset = allocation.attribution.asset_low;
            out->thread = allocation.thread;
            ++out->count;
        },
        &found);

    CY_CHECK_EQ(report.leaked_allocations, 1u);
    CY_CHECK_EQ(found.count, 1u);
    CY_CHECK_EQ(found.cell, 0x99u);
    CY_CHECK_EQ(found.asset, 0x5150u);
    CY_CHECK_EQ(found.thread, cy::current_thread_ordinal());
    // The tracker still owns the block; its destructor returns it upstream.
}
