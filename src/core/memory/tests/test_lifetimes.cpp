// Frame memory, retirement epochs, virtual reservation, ownership, and process-lifetime
// declarations. Tasks 2.7, 2.8, 2.9 and 2.12.

#include <cy/test/test.h>

#include <cy/core/memory/epoch.h>
#include <cy/core/memory/frame_memory.h>
#include <cy/core/memory/lifetime.h>
#include <cy/core/memory/ownership.h>
#include <cy/core/memory/sanitizer.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/memory/tracking_allocator.h>
#include <cy/core/memory/virtual_memory.h>

#include <cstring>
#include <type_traits>
#include <utility>

namespace {

struct Reclaimed {
    int count = 0;
    int order[8] = {};
    int written = 0;
};

void record_reclaim(void* resource, void* user) noexcept {
    auto* state = static_cast<Reclaimed*>(user);
    ++state->count;
    if (state->written < 8) {
        state->order[state->written++] = static_cast<int>(reinterpret_cast<cy::usize>(resource));
    }
}

struct Asset : cy::RefCounted {
    static int live;

    explicit Asset(int identifier = 0) noexcept : id(identifier) { ++live; }
    ~Asset() { --live; }

    int id = 0;
};

int Asset::live = 0;

}  // namespace

// --- 2.7 Scratch and frame memory
// ------------------------------------------------------------------

CY_TEST_CASE("a temporary sized by entity count comes from the frame arena and is not freed") {
    cy::reset_frame_arena();
    const cy::u64 epoch = cy::frame_arena_epoch();

    auto* buffer = static_cast<cy::u32*>(cy::frame_arena().bump(1024 * sizeof(cy::u32), 16));
    CY_REQUIRE(buffer != nullptr);
    for (cy::u32 index = 0; index < 1024; ++index) {
        buffer[index] = index;
    }
    CY_CHECK_EQ(buffer[1023], 1023u);

    const cy::FrameMemoryStats before = cy::frame_memory_stats();
    CY_CHECK(before.frame_used >= 1024u * sizeof(cy::u32));

    cy::reset_frame_arena();
    CY_CHECK_EQ(cy::frame_memory_stats().frame_used, 0u);
    CY_CHECK_EQ(cy::frame_arena_epoch(), epoch + 1);
    CY_CHECK(cy::frame_memory_stats().frame_high_water >= before.frame_used);
}

CY_TEST_CASE("use after reset is caught: the region carries a poison pattern") {
    cy::reset_frame_arena();
    auto* bytes = static_cast<cy::u8*>(cy::frame_arena().bump(64, 8));
    CY_REQUIRE(bytes != nullptr);
    std::memset(bytes, 0x42, 64);

    cy::reset_frame_arena();

    // THERE ARE TWO POISONS AND THIS CASE READS WHICHEVER ONE IS LIVE.
    //
    // Under AddressSanitizer the region is handed back to the tool on reset (sanitizer.h), so
    // reading `bytes[0]` at all is the use-after-reset this requirement is about — and the tool
    // reports it rather than the test comparing a byte. Asking the shadow is how the same claim is
    // made without provoking the abort.
    //
    // Otherwise the byte pattern is the mechanism, and it is a development-build behaviour:
    // `core-memory-and-containers` asks for it in development builds, and writing a megabyte of
    // pattern at every frame boundary is not something a shipping build should pay for. The test
    // checks the profile rather than asserting behaviour that was compiled out — which is the
    // mistake that made M0's suite red in two configurations.
    if constexpr (cy::kAddressSanitizerPresent) {
        CY_CHECK(cy::memory_is_poisoned(bytes));
        CY_CHECK(cy::memory_is_poisoned(bytes + 63));
    } else {
#if defined(CY_DEVELOPMENT)
        CY_CHECK_EQ(bytes[0], cy::kArenaPoisonByte);
        CY_CHECK_EQ(bytes[63], cy::kArenaPoisonByte);
#else
        CY_CHECK_EQ(bytes[0], 0x42);  // untouched: no poison outside a development build
#endif
    }
}

CY_TEST_CASE("a job's scratch is released on every path out of the job") {
    const cy::usize before = cy::scratch_stack().used();
    {
        cy::ScratchScope scratch;
        auto* values = scratch.allocate<cy::f32>(256);
        CY_REQUIRE(values != nullptr);
        values[255] = 1.0f;
        CY_CHECK(cy::scratch_stack().used() > before);

        {
            cy::ScratchScope nested;
            CY_REQUIRE(nested.allocate<cy::u64>(64) != nullptr);
        }
    }
    CY_CHECK_EQ(cy::scratch_stack().used(), before);
}

// --- 2.8 Retirement and frame epochs
// ----------------------------------------------------------------

CY_TEST_CASE("destroyed while in use: a resource is reclaimed only after its epoch has passed") {
    cy::EpochManager epochs(64);
    CY_REQUIRE(epochs.initialize().has_value());

    Reclaimed state;
    const cy::Epoch retired_in = epochs.current();
    CY_REQUIRE(epochs.retire(reinterpret_cast<void*>(1), &record_reclaim, &state, "gpu-buffer")
                   .has_value());

    // The frame that may still reference it has not finished, so nothing is reclaimed.
    CY_CHECK_EQ(epochs.reclaim(), 0u);
    CY_CHECK_EQ(state.count, 0);
    CY_CHECK_EQ(epochs.stats().depth, 1u);

    epochs.advance();
    CY_CHECK_EQ(epochs.reclaim(), 0u);  // advancing is not the same as completing

    epochs.complete(retired_in);
    CY_CHECK_EQ(epochs.reclaim(), 1u);
    CY_CHECK_EQ(state.count, 1);
    CY_CHECK_EQ(epochs.stats().depth, 0u);
}

CY_TEST_CASE("one mechanism, many consumers: the asset system and the renderer share it") {
    cy::EpochManager epochs(64);
    CY_REQUIRE(epochs.initialize().has_value());

    Reclaimed renderer;
    Reclaimed assets;
    CY_REQUIRE(
        epochs.retire(reinterpret_cast<void*>(1), &record_reclaim, &renderer, "command-buffer")
            .has_value());
    CY_REQUIRE(epochs.retire(reinterpret_cast<void*>(2), &record_reclaim, &assets, "asset-page")
                   .has_value());

    const cy::Epoch first = epochs.current();
    epochs.advance();
    CY_REQUIRE(epochs.retire(reinterpret_cast<void*>(3), &record_reclaim, &renderer, "snapshot")
                   .has_value());

    // Completing the first epoch reclaims both of its consumers' resources and neither of the
    // second's — one queue, one rule, two subsystems.
    epochs.complete(first);
    CY_CHECK_EQ(epochs.reclaim(), 2u);
    CY_CHECK_EQ(renderer.count, 1);
    CY_CHECK_EQ(assets.count, 1);
    CY_CHECK_EQ(epochs.stats().depth, 1u);
}

CY_TEST_CASE("stuck retirement is visible, and the queue is bounded") {
    cy::EpochManager epochs(4);
    CY_REQUIRE(epochs.initialize().has_value());

    Reclaimed state;
    for (int index = 0; index < 4; ++index) {
        CY_REQUIRE(epochs.retire(reinterpret_cast<void*>(1), &record_reclaim, &state, "stuck")
                       .has_value());
    }

    // Bounded: the fifth is refused, and the refusal is counted rather than the queue growing.
    const cy::Status refused =
        epochs.retire(reinterpret_cast<void*>(1), &record_reclaim, &state, "stuck");
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::OutOfRange);
    CY_CHECK_EQ(epochs.stats().refused, 1u);

    // A consumer that never completes: the epochs advance and the completed one does not.
    for (int frame = 0; frame < 10; ++frame) {
        epochs.advance();
    }
    CY_CHECK_EQ(epochs.reclaim(), 0u);
    CY_CHECK(epochs.stalled_epochs() >= 10u);

    const cy::RetirementStats stats = epochs.stats();
    CY_CHECK_EQ(stats.depth, 4u);
    CY_CHECK_EQ(stats.capacity, 4u);
    CY_CHECK_EQ(stats.oldest_pending, 1u);  // the epoch that has not been completed

    CY_CHECK_EQ(epochs.reclaim_all(), 4u);  // shutdown drains regardless
}

// --- 2.9 Virtual memory and ownership
// ---------------------------------------------------------------

CY_TEST_CASE("reservation is not consumption: reserved space is reported apart from committed") {
    const cy::VirtualMemoryInfo info = cy::virtual_memory_info();
    CY_REQUIRE(info.supported);
    CY_CHECK(info.page_size >= 4096u);

    const cy::DomainStats before = cy::domain_stats(cy::MemoryDomain::Streaming);

    cy::VirtualArena arena(cy::MemoryDomain::Streaming, "texture-cache");
    CY_REQUIRE(arena.reserve(cy::usize{64} * 1024 * 1024).has_value());
    CY_CHECK(arena.reserves_address_space());

    const cy::DomainStats reserved = cy::domain_stats(cy::MemoryDomain::Streaming);
    CY_CHECK_EQ(reserved.reserved_bytes, before.reserved_bytes + (cy::u64{64} * 1024 * 1024));
    CY_CHECK_EQ(reserved.live_bytes, before.live_bytes);  // reserved is NOT live
    CY_CHECK_EQ(arena.committed_bytes(), 0u);
}

CY_TEST_CASE("a cache grows without moving: pages are committed inside the reservation") {
    cy::VirtualArena arena(cy::MemoryDomain::Streaming, "grow");
    CY_REQUIRE(arena.reserve(cy::usize{8} * 1024 * 1024).has_value());

    auto* first = static_cast<cy::u8*>(arena.bump(64, 64));
    CY_REQUIRE(first != nullptr);
    std::memset(first, 0x5A, 64);
    const cy::usize committed_after_first = arena.committed_bytes();
    CY_CHECK(committed_after_first > 0u);

    // Grow far past the first page. Existing pointers stay valid, because the address range was
    // reserved up front and only the backing was added.
    for (int index = 0; index < 64; ++index) {
        CY_REQUIRE(arena.bump(cy::usize{64} * 1024, 64) != nullptr);
    }
    CY_CHECK(arena.committed_bytes() > committed_after_first);
    CY_CHECK_EQ(first[0], 0x5A);
    CY_CHECK_EQ(first[63], 0x5A);

    const cy::u64 live_before = cy::domain_stats(cy::MemoryDomain::Streaming).live_bytes;
    arena.decommit_all();
    CY_CHECK_EQ(arena.committed_bytes(), 0u);
    CY_CHECK(cy::domain_stats(cy::MemoryDomain::Streaming).live_bytes < live_before);
}

CY_TEST_CASE("UniquePtr is sole ownership and releases to the allocator it came from") {
    Asset::live = 0;
    {
        auto owned = cy::make_unique<Asset>(cy::default_allocator(), 7);
        CY_REQUIRE(owned.has_value());
        CY_CHECK_EQ(Asset::live, 1);
        CY_CHECK_EQ((*owned)->id, 7);

        static_assert(!std::is_copy_constructible_v<cy::UniquePtr<Asset>>,
                      "sole ownership must not be copyable");

        cy::UniquePtr<Asset> moved(std::move(*owned));
        CY_CHECK_FALSE(static_cast<bool>(*owned));
        CY_CHECK_EQ(moved->id, 7);
    }
    CY_CHECK_EQ(Asset::live, 0);
}

CY_TEST_CASE("an asset is unloaded when the last Ref is released") {
    Asset::live = 0;
    {
        auto loaded = cy::make_ref<Asset>(cy::default_allocator(), 3);
        CY_REQUIRE(loaded.has_value());
        CY_CHECK_EQ(Asset::live, 1);
        CY_CHECK_EQ(loaded->use_count(), 1u);

        {
            cy::Ref<Asset> second = *loaded;
            CY_CHECK_EQ(loaded->use_count(), 2u);
            CY_CHECK_EQ(second->id, 3);
            // Self-assignment through two references to one object must not destroy it.
            second = *loaded;
            CY_CHECK_EQ(loaded->use_count(), 2u);
        }
        CY_CHECK_EQ(loaded->use_count(), 1u);
        CY_CHECK_EQ(Asset::live, 1);
    }
    CY_CHECK_EQ(Asset::live, 0);  // the last release unloaded it
}

// --- 2.12 Process-lifetime declarations
// --------------------------------------------------------------

CY_TEST_CASE("an intentional lifetime allocation is declared, and the leak report excludes it") {
    cy::TrackingAllocator tracker(cy::default_allocator(), cy::MemoryDomain::Engine, "pool");

    void* intentional = tracker.allocate(4096, 64);
    void* accidental = tracker.allocate(128, 8);
    CY_REQUIRE(intentional != nullptr);
    CY_REQUIRE(accidental != nullptr);

    const cy::u32 declared_before = cy::process_lifetime_count();
    cy::declare_process_lifetime(intentional, 4096, "trace-ring");
    CY_CHECK(cy::is_process_lifetime(intentional));
    CY_CHECK_FALSE(cy::is_process_lifetime(accidental));
    CY_CHECK_EQ(cy::process_lifetime_count(), declared_before + 1);

    // Declaring the same pointer twice is not two allocations.
    cy::declare_process_lifetime(intentional, 4096, "trace-ring");
    CY_CHECK_EQ(cy::process_lifetime_count(), declared_before + 1);

    cy::u32 reported = 0;
    const cy::LeakReport report = tracker.report_leaks(
        [](const cy::TrackedAllocation&, void* user) noexcept { ++*static_cast<cy::u32*>(user); },
        &reported);

    CY_CHECK_EQ(report.live_allocations, 2u);
    CY_CHECK_EQ(report.process_lifetime_allocations, 1u);
    CY_CHECK_EQ(report.process_lifetime_bytes, 4096u);
    CY_CHECK_EQ(report.leaked_allocations, 1u);  // only the undeclared one is a defect
    CY_CHECK_EQ(report.leaked_bytes, 128u);
    CY_CHECK_EQ(reported, 1u);

    // Freeing a declared block withdraws the declaration, so the registry does not fill up with
    // pointers that no longer exist.
    tracker.deallocate(intentional, 4096, 64);
    CY_CHECK_EQ(cy::process_lifetime_count(), declared_before);
    tracker.deallocate(accidental, 128, 8);
}
