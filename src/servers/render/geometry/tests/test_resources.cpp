// The GPU memory ledger, mip streaming and the geometry ring. M6 task 8.4.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/geometry/resources.h>
#include <cy/test/test.h>

using namespace cy::render::geometry;
using cy::u32;
using cy::u64;
using cy::u8;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

}  // namespace

CY_TEST_CASE("ledger: a resource is released only after the frames in flight complete") {
    // "GPU resources SHALL be reference counted through their asset handles and released only after
    // the GPU has finished all frames that could reference them." The wrong version of this rule —
    // free when the count reaches zero — is correct on a CPU and works in testing on a GPU.
    ResourceLedger ledger(allocator());
    ledger.set_frames_in_flight(2);
    const auto id = ledger.acquire(MemoryCategory::Meshes, 4096, true);
    CY_REQUIRE(id.has_value());
    CY_CHECK(ledger.reference_count(id.value()) == 1);
    CY_CHECK(ledger.report().bytes[static_cast<u32>(MemoryCategory::Meshes)] == 4096);

    CY_REQUIRE(ledger.release(id.value(), 100).has_value());
    // The count is zero, and the bytes are STILL charged: the device may be reading them.
    CY_CHECK(ledger.live(id.value()));
    CY_CHECK(ledger.report().pending_release == 4096);
    CY_CHECK(ledger.retire(101) == 0);
    CY_CHECK(ledger.live(id.value()));

    // Frame 102 is the first the device can have finished, given two frames in flight.
    CY_CHECK(ledger.retire(102) == 4096);
    CY_CHECK(!ledger.live(id.value()));
    CY_CHECK(ledger.report().bytes[static_cast<u32>(MemoryCategory::Meshes)] == 0);
    CY_CHECK(ledger.report().released == 4096);
    CY_CHECK(ledger.report().pending_release == 0);
}

CY_TEST_CASE("ledger: a second reference keeps a resource, and a revival is refused") {
    ResourceLedger ledger(allocator());
    ledger.set_frames_in_flight(1);
    const auto id = ledger.acquire(MemoryCategory::Textures, 1024, true);
    CY_REQUIRE(id.has_value());
    CY_REQUIRE(ledger.add_reference(id.value()).has_value());
    CY_CHECK(ledger.reference_count(id.value()) == 2);

    CY_REQUIRE(ledger.release(id.value(), 5).has_value());
    CY_CHECK(ledger.retire(100) == 0);  // still referenced

    CY_REQUIRE(ledger.release(id.value(), 5).has_value());
    // A resource whose last reference has gone is waiting for the device, and reviving it would
    // hand back memory that is about to be reused.
    CY_CHECK(!ledger.add_reference(id.value()).has_value());
    CY_CHECK(ledger.retire(100) == 1024);

    // Releasing what is gone, or what never was, is refused rather than corrupting a count.
    CY_CHECK(!ledger.release(id.value(), 6).has_value());
    CY_CHECK(!ledger.release(4242, 6).has_value());
}

CY_TEST_CASE("ledger: a retired slot is reused, so a streaming world does not grow the table") {
    ResourceLedger ledger(allocator());
    ledger.set_frames_in_flight(1);
    const auto first = ledger.acquire(MemoryCategory::Buffers, 64, false);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(ledger.release(first.value(), 0).has_value());
    CY_CHECK(ledger.retire(10) == 64);

    const auto second = ledger.acquire(MemoryCategory::Buffers, 128, false);
    CY_REQUIRE(second.has_value());
    CY_CHECK(second.value() == first.value());
    CY_CHECK(ledger.bytes_of(second.value()) == 128);
}

CY_TEST_CASE("ledger: memory is reported by category, and a budget reports rather than refuses") {
    ResourceLedger ledger(allocator());
    CY_REQUIRE(ledger.set_budget(MemoryCategory::Textures, 1000).has_value());
    CY_REQUIRE(ledger.acquire(MemoryCategory::Textures, 600, true).has_value());
    CY_REQUIRE(ledger.acquire(MemoryCategory::Meshes, 300, true).has_value());
    MemoryReport report = ledger.report();
    CY_CHECK(report.bytes[static_cast<u32>(MemoryCategory::Textures)] == 600);
    CY_CHECK(report.bytes[static_cast<u32>(MemoryCategory::Meshes)] == 300);
    CY_CHECK(report.total_bytes() == 900);
    CY_CHECK(report.budget_exceeded == 0);

    // Over budget: the allocation still SUCCEEDS. Refusing one mid-frame produces a missing object,
    // and the requirement's answer to pressure is eviction.
    CY_REQUIRE(ledger.acquire(MemoryCategory::Textures, 600, true).has_value());
    report = ledger.report();
    CY_CHECK(report.bytes[static_cast<u32>(MemoryCategory::Textures)] == 1200);
    CY_CHECK(report.budget_exceeded == 1);
    CY_CHECK(report.budget[static_cast<u32>(MemoryCategory::Textures)] == 1000);

    CY_CHECK(!ledger.set_budget(MemoryCategory::Count, 1).has_value());
}

CY_TEST_CASE("ledger: eviction candidates are streamable, in the right category, largest first") {
    // An ORDERING and not a decision: this module does not know an instance's importance and
    // `residency` does. A ledger that evicted by size alone would drop the terrain the camera is
    // standing on because it is big.
    ResourceLedger ledger(allocator());
    const auto small = ledger.acquire(MemoryCategory::Textures, 100, true);
    const auto large = ledger.acquire(MemoryCategory::Textures, 900, true);
    const auto pinned = ledger.acquire(MemoryCategory::Textures, 5000, false);
    const auto other = ledger.acquire(MemoryCategory::Meshes, 8000, true);
    CY_REQUIRE(small.has_value());
    CY_REQUIRE(large.has_value());
    CY_REQUIRE(pinned.has_value());
    CY_REQUIRE(other.has_value());

    ResourceId candidates[8] = {};
    const u32 count =
        ledger.evict_candidates(MemoryCategory::Textures, cy::Span<ResourceId>(candidates, 8));
    CY_REQUIRE(count == 2);
    CY_CHECK(candidates[0] == large.value());
    CY_CHECK(candidates[1] == small.value());

    // A render target cannot be evicted — nothing can produce it again on demand — and another
    // category's resources are not this category's problem.
    for (u32 index = 0; index < count; ++index) {
        CY_CHECK(candidates[index] != pinned.value());
        CY_CHECK(candidates[index] != other.value());
    }
}

CY_TEST_CASE("mip streaming: the tail is a guarantee, and a tail of zero is refused") {
    // "The lowest few mips SHALL always be resident so no texture is ever entirely missing."
    MipChain chain;
    CY_CHECK(!chain.configure(TextureResidency::MipStreamed, 10, 0, 1 << 20).has_value());
    CY_CHECK(!chain.configure(TextureResidency::MipStreamed, 4, 8, 1 << 20).has_value());
    CY_CHECK(!chain.configure(TextureResidency::MipStreamed, 0, 1, 1 << 20).has_value());

    // The virtual models belong to `virtual-texturing`, and this module says so rather than
    // pretending to implement them.
    CY_CHECK(!chain.configure(TextureResidency::VirtualStreamed, 10, 3, 1 << 20).has_value());
    CY_CHECK(!chain.configure(TextureResidency::VirtualRuntime, 10, 3, 1 << 20).has_value());

    CY_REQUIRE(chain.configure(TextureResidency::MipStreamed, 10, 3, 1 << 20).has_value());
    CY_CHECK(chain.tail() == 3);
    // The tail is resident from the moment the texture exists: mips 7, 8 and 9.
    CY_CHECK(chain.highest_resident() == 7);
    CY_CHECK(chain.resident_bytes() > 0);
}

CY_TEST_CASE("mip streaming: a non-resident mip falls back to the highest resident one") {
    // "WHEN the camera approaches and higher mips are sampled THEN they SHALL be scheduled and
    // swapped in when ready, with the lower mip shown meanwhile." Never blocking is why this cannot
    // fail: it answers a level rather than an error.
    MipChain chain;
    CY_REQUIRE(chain.configure(TextureResidency::MipStreamed, 10, 3, 1 << 20).has_value());
    CY_CHECK(chain.resident_mip(0) == 7);
    CY_CHECK(chain.resident_mip(9) == 9);

    chain.request(2);
    CY_CHECK(chain.pending());
    CY_CHECK(chain.requested_bytes() > chain.resident_bytes());
    // Still nothing has arrived, so the frame still samples mip 7.
    CY_CHECK(chain.resident_mip(0) == 7);

    chain.commit_resident(2);
    CY_CHECK(!chain.pending());
    CY_CHECK(chain.resident_mip(0) == 2);
    CY_CHECK(chain.resident_mip(5) == 5);

    // Eviction cannot cut into the tail, which is the guarantee restated as behaviour.
    chain.commit_resident(9);
    CY_CHECK(chain.highest_resident() == 7);
    CY_CHECK(chain.resident_mip(0) == 7);
}

CY_TEST_CASE("mip streaming: a fully resident texture ignores requests") {
    // "WHEN a small user-interface texture and a terrain material are cooked THEN the first SHALL
    // be fully resident and the second virtual, from their declared models."
    MipChain chain;
    CY_REQUIRE(chain.configure(TextureResidency::FullyResident, 6, 1, 4096).has_value());
    CY_CHECK(chain.highest_resident() == 0);
    chain.request(4);
    CY_CHECK(chain.highest_resident() == 0);
    CY_CHECK(chain.resident_mip(0) == 0);
    CY_CHECK(!chain.pending());
}

CY_TEST_CASE("geometry ring: an allocation that does not fit is refused rather than wrapped") {
    // Wrapping into the next slice is how a frame overwrites the vertices the device is still
    // reading, and the symptom is geometry that flickers on one machine and not another.
    GeometryRing ring;
    CY_CHECK(!ring.configure(0, 2).has_value());
    CY_CHECK(!ring.configure(1024, 0).has_value());
    CY_REQUIRE(ring.configure(1024, 3).has_value());
    CY_CHECK(ring.total_bytes() == 3072);

    ring.begin_frame(0);
    const auto first = ring.allocate(256, 16);
    CY_REQUIRE(first.has_value());
    CY_CHECK(first.value() == 0);
    const auto second = ring.allocate(256, 256);
    CY_REQUIRE(second.has_value());
    CY_CHECK(second.value() == 256);
    CY_CHECK(ring.used_this_frame() == 512);

    CY_CHECK(!ring.allocate(2048, 16).has_value());
    CY_CHECK(ring.overflows() == 1);
    // The refusal did not consume the slice: the next allocation that does fit still works.
    CY_REQUIRE(ring.allocate(128, 16).has_value());
}

CY_TEST_CASE("geometry ring: each frame writes its own slice, and the peak is remembered") {
    GeometryRing ring;
    CY_REQUIRE(ring.configure(1000, 3).has_value());

    ring.begin_frame(0);
    CY_REQUIRE(ring.allocate(100, 1).has_value());
    ring.begin_frame(1);
    const auto second = ring.allocate(200, 1);
    CY_REQUIRE(second.has_value());
    CY_CHECK(second.value() == 1000);
    ring.begin_frame(2);
    const auto third = ring.allocate(300, 1);
    CY_REQUIRE(third.has_value());
    CY_CHECK(third.value() == 2000);
    // Frame 3 reuses frame 0's slice, which the device has finished by then.
    ring.begin_frame(3);
    const auto fourth = ring.allocate(50, 1);
    CY_REQUIRE(fourth.has_value());
    CY_CHECK(fourth.value() == 0);

    // The high-water mark across every frame, which is what sizes the ring next time.
    CY_CHECK(ring.peak() == 300);
    CY_CHECK(ring.overflows() == 0);
}

CY_TEST_CASE("statistics: merging two views weights overdraw by draws rather than adding it") {
    GeometryStatistics left;
    left.draws = 10;
    left.triangles = 1000;
    left.overdraw_fixed_point = 512;  // exactly two
    left.lod_histogram[0] = 10;

    GeometryStatistics right;
    right.draws = 10;
    right.triangles = 500;
    right.overdraw_fixed_point = 256;  // exactly one
    right.mip_histogram[4] = 3;

    left.merge(right);
    CY_CHECK(left.draws == 20);
    CY_CHECK(left.triangles == 1500);
    CY_CHECK(left.lod_histogram[0] == 10);
    CY_CHECK(left.mip_histogram[4] == 3);
    // Adding two overdraw figures would report four on two views that each overdrew twice.
    CY_CHECK(left.overdraw_fixed_point == 384);

    left.clear();
    CY_CHECK(left.draws == 0);
    CY_CHECK(left.triangles == 0);
}
