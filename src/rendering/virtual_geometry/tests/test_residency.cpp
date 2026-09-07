// The GPU geometry cache: one shared budget, scored eviction, generation counters, and teardown
// under load. M7 task 7.2.
//
// THE TEARDOWN CASE IS HERE BECAUSE M5.5's GATE FOUND A JOLT JOB BRIDGE DESTROYING ITS FREE LIST
// UNDERNEATH A WORKER, one run in forty. `GeometryCache` is documented as not thread-safe and not
// internally threaded — `service()` is one thread's and only the reads run elsewhere — so the
// teardown case tests exactly that contract rather than a stronger one it does not make: a reader
// thread hammering `lookup()` and `resident()` while the owner services and evicts, joined before
// the cache is destroyed, repeated enough times that a race would show. A case that destroyed the
// cache underneath the reader would be testing a promise this module deliberately does not make,
// and its failure would say nothing.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/virtual_geometry/residency.h>

#include "meshes.h"

#include <atomic>
#include <thread>

namespace {

using namespace cy;             // NOLINT(google-build-using-namespace) — the suite's own subject
using namespace cy::rendering;  // NOLINT(google-build-using-namespace)

vg::BuildOptions test_options() noexcept {
    vg::BuildOptions options;
    options.policy.min_triangles = 8;
    options.policy.target_triangles = 32;
    options.policy.max_triangles = 32;
    options.policy.max_vertices = 64;
    options.policy.group_size = 4;
    options.page_bytes = 1024;
    options.resident_budget_bytes = 1024;
    return options;
}

/// A cooked asset a case can register. Held by the caller because `DecodedAsset` borrows the bytes.
struct Cooked {
    explicit Cooked(Allocator& allocator) noexcept : bytes(allocator), asset(allocator) {}

    Array<u8> bytes;
    vg::DecodedAsset asset;
};

[[nodiscard]] bool cook(Allocator& allocator, Cooked& out, u32 subdivisions = 3) noexcept {
    const vg::test::MeshData mesh = vg::test::icosphere(allocator, subdivisions);
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), test_options(), allocator);
    if (!build) {
        return false;
    }
    if (Status encoded = vg::encode_asset(*build, vg::VertexEncoding{}, out.bytes); !encoded) {
        return false;
    }
    Expected<vg::DecodedAsset, Error> decoded = vg::decode_asset(out.bytes.span(), allocator);
    if (!decoded) {
        return false;
    }
    out.asset = std::move(*decoded);
    return true;
}

}  // namespace

CY_TEST_CASE("the always-resident root is resident before anything is serviced") {
    // "An object SHALL never fail to render because streaming has not completed" is unconditional,
    // and a root admitted on the first service would be true from the second frame — which is the
    // frame after the one where it matters.
    Allocator& allocator = system_allocator(MemoryDomain::Gpu);
    Cooked cooked(allocator);
    CY_REQUIRE(cook(allocator, cooked));
    CY_REQUIRE(cooked.asset.pages.size() > 2U);

    vg::CacheOptions options;
    options.budget_bytes = 1024ULL * 1024;
    vg::GeometryCache cache(options, allocator);
    CY_REQUIRE(cache.register_asset(0, cooked.asset).has_value());

    CY_CHECK(cache.resident(0, 0));
    CY_CHECK(cache.lookup(0, 0).pinned());
    CY_CHECK_GT(cache.statistics().resident_bytes, 0U);
    for (u32 page = 0; page < cooked.asset.pages.size(); ++page) {
        CY_CHECK_EQ(cache.resident(0, page), cooked.asset.pages[page].resident);
    }
}

CY_TEST_CASE("an evicted page's generation makes a stale reference miss") {
    // `virtual-geometry` — "Stale reference is detected": "WHEN a page is evicted and its slot
    // reused THEN the generation counter SHALL cause the old reference to miss rather than read the
    // new page." Without it the captured byte offset would address whatever landed in the slot,
    // which renders as a shard of the wrong mesh.
    Allocator& allocator = system_allocator(MemoryDomain::Gpu);
    Cooked cooked(allocator);
    CY_REQUIRE(cook(allocator, cooked));

    vg::CacheOptions options;
    // Room for the pinned root and a couple of pages, so that admitting more must evict.
    options.budget_bytes = 6ULL * 1024;
    options.minimum_residency_frames = 0;
    vg::GeometryCache cache(options, allocator);
    CY_REQUIRE(cache.register_asset(0, cooked.asset).has_value());

    // Ask for a page, capture a reference to it, then push it out with higher-priority traffic.
    const vg::PageRequest wanted[1] = {{0, 1, 10.0F, 1}};
    CY_REQUIRE(cache.service(Span<const vg::PageRequest>(wanted, 1), 1.0).has_value());
    CY_REQUIRE(cache.resident(0, 1));
    const vg::PageTableEntry entry = cache.lookup(0, 1);
    const vg::PageReference reference{0, 1, entry.location, entry.generation};
    CY_REQUIRE(cache.redeem(reference).has_value());

    Array<vg::PageRequest> pressure(allocator);
    for (u32 page = 2; page < cooked.asset.pages.size(); ++page) {
        CY_REQUIRE(pressure.push_back(vg::PageRequest{0, page, 100.0F, 1}).has_value());
    }
    CY_REQUIRE(pressure.size() > 4U);
    for (u32 frame = 0; frame < 4; ++frame) {
        CY_REQUIRE(cache.service(pressure.span(), 2.0 + frame).has_value());
    }
    CY_REQUIRE_FALSE(cache.resident(0, 1));

    // THE REFERENCE MISSES. It does not read whatever is at that offset now.
    CY_CHECK_FALSE(cache.redeem(reference).has_value());
    CY_CHECK_GT(cache.statistics().stale_references, 0U);
    CY_CHECK_GT(cache.statistics().evictions, 0U);
    CY_TEST_MESSAGE("budget " << cache.statistics().budget_bytes << " bytes, resident "
                              << cache.statistics().resident_bytes << " over "
                              << cache.statistics().resident_pages << " pages; "
                              << cache.statistics().admissions << " admissions, "
                              << cache.statistics().evictions << " evictions, hit rate "
                              << cache.statistics().hit_rate());
}

CY_TEST_CASE("the pinned root is never evicted, however hard the budget is pushed") {
    Allocator& allocator = system_allocator(MemoryDomain::Gpu);
    Cooked cooked(allocator);
    CY_REQUIRE(cook(allocator, cooked));

    vg::CacheOptions options;
    options.budget_bytes = 4ULL * 1024;
    options.minimum_residency_frames = 0;
    vg::GeometryCache cache(options, allocator);
    CY_REQUIRE(cache.register_asset(0, cooked.asset).has_value());

    Array<vg::PageRequest> everything(allocator);
    for (u32 page = 1; page < cooked.asset.pages.size(); ++page) {
        CY_REQUIRE(everything.push_back(vg::PageRequest{0, page, 50.0F, 1}).has_value());
    }
    for (u32 frame = 0; frame < 16; ++frame) {
        CY_REQUIRE(cache.service(everything.span(), 1.0 + frame).has_value());
        CY_REQUIRE(cache.resident(0, 0));
    }
    CY_CHECK_GT(cache.statistics().evictions, 0U);
    CY_CHECK_LE(cache.statistics().resident_bytes, options.budget_bytes);
    // Churn is measured rather than inferred: a cache can hold a high hit rate while thrashing.
    CY_CHECK_GT(cache.statistics().refetches, 0U);
    CY_TEST_MESSAGE("under a " << options.budget_bytes
                               << "-byte budget: " << cache.statistics().evictions << " evictions, "
                               << cache.statistics().refetches << " refetches, occupancy "
                               << cache.statistics().occupancy());
}

CY_TEST_CASE("a page that arrived this frame cannot leave this frame") {
    // `residency`'s minimum residency age, which is the other half of "no oscillation".
    Allocator& allocator = system_allocator(MemoryDomain::Gpu);
    Cooked cooked(allocator);
    CY_REQUIRE(cook(allocator, cooked));

    vg::CacheOptions options;
    options.budget_bytes = 4ULL * 1024;
    options.minimum_residency_frames = 4;
    vg::GeometryCache cache(options, allocator);
    CY_REQUIRE(cache.register_asset(0, cooked.asset).has_value());

    Array<vg::PageRequest> everything(allocator);
    for (u32 page = 1; page < cooked.asset.pages.size(); ++page) {
        CY_REQUIRE(everything.push_back(vg::PageRequest{0, page, 50.0F, 1}).has_value());
    }
    CY_REQUIRE(cache.service(everything.span(), 1.0).has_value());
    const u64 first_evictions = cache.statistics().evictions;
    CY_CHECK_EQ(first_evictions, 0U);
    // The frame after: everything admitted is too young to evict, so the admissions are DEFERRED
    // rather than churning the cache.
    CY_REQUIRE(cache.service(everything.span(), 2.0).has_value());
    CY_CHECK_EQ(cache.statistics().evictions, 0U);
    CY_CHECK_GT(cache.statistics().deferred_requests, 0U);
}

CY_TEST_CASE("a prefetch never outbids the frame that needs the page now") {
    Allocator& allocator = system_allocator(MemoryDomain::Gpu);
    Cooked cooked(allocator);
    CY_REQUIRE(cook(allocator, cooked));

    vg::CacheOptions options;
    options.budget_bytes = 3ULL * 1024;
    options.minimum_residency_frames = 0;
    options.admissions_per_frame = 1;
    vg::GeometryCache cache(options, allocator);
    CY_REQUIRE(cache.register_asset(0, cooked.asset).has_value());

    // A reactive request and a prefetch for two different pages, at the same nominal priority. The
    // reactive one wins, because a prediction is a request at a discount.
    const u32 predicted[1] = {2};
    CY_REQUIRE(cache.prefetch(0, Span<const u32>(predicted, 1), 10.0F).has_value());
    const vg::PageRequest reactive[1] = {{0, 1, 6.0F, 1}};
    CY_REQUIRE(cache.service(Span<const vg::PageRequest>(reactive, 1), 2.0).has_value());
    CY_CHECK(cache.resident(0, 1));
}

CY_TEST_CASE("two subsystems share one policy, and geometry declares its lever") {
    // M7 task 4.3 asks for "more than one subsystem registered against the residency policy in a
    // shipped path". This is the geometry half of that, asserted where it is made rather than where
    // it is consumed: the cache registers, reports what it holds, and declares the one lever
    // `residency` names for it.
    Allocator& allocator = system_allocator(MemoryDomain::Gpu);
    Cooked cooked(allocator);
    CY_REQUIRE(cook(allocator, cooked));

    residency::ResidencyServer server(allocator);
    vg::CacheOptions options;
    options.budget_bytes = 64ULL * 1024;
    vg::GeometryCache cache(options, allocator);
    CY_REQUIRE(cache.attach(server).has_value());
    CY_CHECK(server.registered(residency::Subsystem::Geometry));

    const residency::SubsystemPolicy policy = server.policy(residency::Subsystem::Geometry);
    CY_CHECK_EQ(policy.budget_bytes, options.budget_bytes);
    CY_CHECK(policy.levers[static_cast<u32>(residency::Lever::GeometryErrorThreshold)].declared);
    CY_CHECK_LT(policy.levers[static_cast<u32>(residency::Lever::GeometryErrorThreshold)].normal,
                policy.levers[static_cast<u32>(residency::Lever::GeometryErrorThreshold)].critical);

    CY_REQUIRE(cache.register_asset(0, cooked.asset).has_value());
    const vg::PageRequest wanted[1] = {{0, 1, 4.0F, 1}};
    CY_REQUIRE(cache.service(Span<const vg::PageRequest>(wanted, 1), 1.0).has_value());
    CY_CHECK_GT(server.resident_bytes(residency::Subsystem::Geometry), 0U);
}

CY_TEST_CASE("a cache is destroyed with requests in flight, in a loop, under a reader") {
    // The contract, tested as the contract: `service()` belongs to one thread and the reads do not,
    // so a reader runs alongside a servicing owner and is JOINED before the cache goes away. Two
    // hundred cycles, because M5.5's gate found its job-bridge defect one run in forty.
    Allocator& allocator = system_allocator(MemoryDomain::Gpu);
    Cooked cooked(allocator);
    CY_REQUIRE(cook(allocator, cooked, 2));

    Array<vg::PageRequest> requests(allocator);
    for (u32 page = 0; page < cooked.asset.pages.size(); ++page) {
        CY_REQUIRE(requests.push_back(vg::PageRequest{0, page, static_cast<f32>(page) + 1.0F, 1})
                       .has_value());
    }

    u64 observed = 0;
    for (u32 cycle = 0; cycle < 200; ++cycle) {
        vg::CacheOptions options;
        options.budget_bytes = 2ULL * 1024;
        options.minimum_residency_frames = 0;
        vg::GeometryCache cache(options, allocator);
        CY_REQUIRE(cache.register_asset(0, cooked.asset).has_value());

        std::atomic<bool> stop{false};
        std::atomic<u64> reads{0};
        std::thread reader([&cache, &stop, &reads, pages = cooked.asset.pages.size()]() noexcept {
            u64 local = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                for (u32 page = 0; page < pages; ++page) {
                    local += cache.resident(0, page) ? 1U : 0U;
                    local += cache.lookup(0, page).generation;
                }
            }
            reads.store(local, std::memory_order_relaxed);
        });

        for (u32 frame = 0; frame < 8; ++frame) {
            CY_REQUIRE(cache.service(requests.span(), 1.0 + frame).has_value());
        }
        stop.store(true, std::memory_order_relaxed);
        reader.join();
        observed += reads.load(std::memory_order_relaxed);
        // The cache is destroyed here, after the join and with the last frame's admissions still
        // recorded — which is the ordering the header promises and the one a teardown gets wrong.
    }
    CY_CHECK_GT(observed, 0U);
}
