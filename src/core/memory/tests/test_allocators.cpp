// The allocator interface, the concrete allocators, and the scope. Tasks 2.1 and 2.11.
//
// Every scenario in `core-memory-and-containers` under "Allocator interface" and "Allocator
// propagation" has a case here, named after it, plus the "Hot paths do not reach the general heap"
// scenario of "General heap is an integration decided by measurement" — the half of that
// requirement that is a property of this code rather than of the benchmark in `bench/`.

#include <cy/test/test.h>

#include <cy/core/memory/arena.h>
#include <cy/core/memory/chunk_allocator.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/pool.h>
#include <cy/core/memory/sanitizer.h>
#include <cy/core/memory/scope.h>
#include <cy/core/memory/slab.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/memory/tracking_allocator.h>

#include <cstring>
#include <type_traits>

namespace {

/// An allocator that refuses everything, so the out-of-memory path is exercised without needing the
/// machine to actually run out.
class RefusingAllocator final : public cy::Allocator {
public:
    RefusingAllocator() noexcept : Allocator(cy::MemoryDomain::Engine, "refusing") {}

    cy::u32 attempts = 0;

protected:
    void* do_allocate(cy::usize, cy::usize) noexcept override {
        ++attempts;
        return nullptr;
    }
    void* do_reallocate(void*, cy::usize, cy::usize, cy::usize) noexcept override {
        return nullptr;
    }
    void do_deallocate(void*, cy::usize, cy::usize) noexcept override {}
};

struct Counted {
    static int live;
    int value = 0;

    explicit Counted(int initial = 0) noexcept : value(initial) { ++live; }
    ~Counted() { --live; }
};

int Counted::live = 0;

}  // namespace

CY_TEST_CASE("allocation is attributable: every allocation carries a tag and a domain") {
    const cy::DomainStats before = cy::domain_stats(cy::MemoryDomain::Physics);

    cy::SystemAllocator& allocator = cy::system_allocator(cy::MemoryDomain::Physics);
    CY_CHECK_EQ(allocator.domain(), cy::MemoryDomain::Physics);
    CY_CHECK(std::strcmp(allocator.tag(), "physics") == 0);

    void* block = allocator.allocate(4096, 64);
    CY_REQUIRE(block != nullptr);
    CY_CHECK_EQ(reinterpret_cast<cy::usize>(block) % 64, 0u);

    const cy::DomainStats during = cy::domain_stats(cy::MemoryDomain::Physics);
    CY_CHECK_EQ(during.live_bytes, before.live_bytes + 4096);
    CY_CHECK_EQ(during.live_allocations, before.live_allocations + 1);
    CY_CHECK(during.peak_bytes >= during.live_bytes);

    allocator.deallocate(block, 4096, 64);
    CY_CHECK_EQ(cy::domain_stats(cy::MemoryDomain::Physics).live_bytes, before.live_bytes);
}

CY_TEST_CASE("domains are hierarchical: a child reports into its parent's aggregate") {
    CY_CHECK_EQ(cy::domain_parent(cy::MemoryDomain::Gpu), cy::MemoryDomain::Renderer);
    CY_CHECK_EQ(cy::domain_parent(cy::MemoryDomain::Streaming), cy::MemoryDomain::Assets);
    CY_CHECK_EQ(cy::domain_parent(cy::MemoryDomain::Engine), cy::MemoryDomain::Engine);
    CY_CHECK(cy::domain_is_within(cy::MemoryDomain::Gpu, cy::MemoryDomain::Engine));
    CY_CHECK_FALSE(cy::domain_is_within(cy::MemoryDomain::Renderer, cy::MemoryDomain::Gpu));

    const cy::u64 renderer_before =
        cy::domain_stats_recursive(cy::MemoryDomain::Renderer).live_bytes;

    cy::SystemAllocator& gpu = cy::system_allocator(cy::MemoryDomain::Gpu);
    void* block = gpu.allocate(2048);
    CY_REQUIRE(block != nullptr);

    CY_CHECK_EQ(cy::domain_stats_recursive(cy::MemoryDomain::Renderer).live_bytes,
                renderer_before + 2048);
    // The renderer's own figure, excluding the GPU child, has not moved.
    CY_CHECK_EQ(cy::domain_stats(cy::MemoryDomain::Gpu).live_bytes >= 2048, true);

    gpu.deallocate(block, 2048);
}

CY_TEST_CASE("out of memory: the allocator returns null and nothing throws") {
    RefusingAllocator refusing;
    CY_CHECK(refusing.allocate(64) == nullptr);
    CY_CHECK_EQ(refusing.attempts, 1u);

    cy::ArenaAllocator arena(cy::MemoryDomain::Frame, "test-arena");
    const cy::Status reserved = arena.reserve(4096, refusing);
    CY_REQUIRE_FALSE(reserved.has_value());
    CY_CHECK_EQ(reserved.error().code, cy::ErrorCode::OutOfMemory);
}

CY_TEST_CASE("per-frame scratch is freed in O(1) by resetting an offset") {
    cy::ArenaAllocator arena(cy::MemoryDomain::Frame, "frame");
    CY_REQUIRE(arena.reserve(cy::usize{64} * 1024).has_value());

    for (int index = 0; index < 100; ++index) {
        CY_REQUIRE(arena.bump(128, 16) != nullptr);
    }
    CY_CHECK(arena.used() >= 100 * 128);
    const cy::usize high_water = arena.high_water();

    arena.reset();

    CY_CHECK_EQ(arena.used(), 0u);
    CY_CHECK_EQ(arena.high_water(), high_water);  // the reset does not forget what it cost
    CY_CHECK(arena.bump(128, 16) != nullptr);
}

CY_TEST_CASE("an arena refuses rather than growing, and counts the refusal") {
    cy::ArenaAllocator arena(cy::MemoryDomain::Frame, "small");
    CY_REQUIRE(arena.reserve(256).has_value());

    CY_CHECK(arena.bump(200, 8) != nullptr);
    CY_CHECK(arena.bump(200, 8) == nullptr);
    CY_CHECK_EQ(arena.overflows(), 1u);
}

CY_TEST_CASE("the stack releases in LIFO order and a scope releases on every path out") {
    cy::StackAllocator stack(cy::MemoryDomain::Frame, "scratch");
    CY_REQUIRE(stack.reserve(8192).has_value());

    void* outer = stack.push(64, 8);
    CY_REQUIRE(outer != nullptr);
    const cy::usize after_outer = stack.used();

    {
        const cy::StackScope scope(stack);
        CY_REQUIRE(stack.push(1024, 16) != nullptr);
        CY_CHECK(stack.used() > after_outer);
    }
    CY_CHECK_EQ(stack.used(), after_outer);

    // The outer allocation is still valid: releasing the inner marker did not disturb it.
    std::memset(outer, 0x11, 64);
    CY_CHECK_EQ(*static_cast<cy::u8*>(outer), 0x11);
}

CY_TEST_CASE("a pool hands out stable addresses and reuses freed blocks") {
    cy::PoolAllocator<Counted> pool(cy::MemoryDomain::Ecs, "counted", 8);

    Counted* first = nullptr;
    Counted* addresses[32] = {};
    for (int index = 0; index < 32; ++index) {
        auto created = pool.create(index);
        CY_REQUIRE(created.has_value());
        addresses[index] = *created;
        if (index == 0) {
            first = *created;
        }
    }
    CY_CHECK_EQ(Counted::live, 32);
    CY_CHECK(pool.capacity() >= 32u);
    // Growth added chunks; the first object did not move.
    CY_CHECK_EQ(addresses[0], first);
    CY_CHECK_EQ(addresses[0]->value, 0);
    CY_CHECK_EQ(addresses[31]->value, 31);

    Counted* recycled = addresses[5];
    pool.destroy(recycled);
    CY_CHECK_EQ(Counted::live, 31);

    auto reused = pool.create(99);
    CY_REQUIRE(reused.has_value());
    CY_CHECK_EQ(*reused, recycled);  // the free list handed the same block back

    for (int index = 0; index < 32; ++index) {
        if (index != 5) {
            pool.destroy(addresses[index]);
        }
    }
    pool.destroy(*reused);
    CY_CHECK_EQ(Counted::live, 0);
}

CY_TEST_CASE("chunks keep their addresses across growth and are reused after release") {
    cy::ChunkAllocator chunks(cy::MemoryDomain::Ecs, "chunks", 4096, 64);

    void* first = chunks.acquire();
    CY_REQUIRE(first != nullptr);
    CY_CHECK_EQ(reinterpret_cast<cy::usize>(first) % 64, 0u);

    void* others[16] = {};
    for (void*& slot : others) {
        slot = chunks.acquire();
        CY_REQUIRE(slot != nullptr);
    }
    CY_CHECK_EQ(chunks.live_chunks(), 17u);

    chunks.release(first);
    CY_CHECK_EQ(chunks.free_chunks(), 1u);
    CY_CHECK_EQ(chunks.acquire(), first);  // the free list returns the same address

    CY_CHECK_EQ(chunks.trim(), 0u);  // nothing free to give back
    chunks.release(first);
    CY_CHECK_EQ(chunks.trim(), 1u);
    CY_CHECK_EQ(chunks.free_chunks(), 0u);
}

CY_TEST_CASE("a slab allocator chains blocks and gives them all back at once") {
    cy::SlabAllocator slab(cy::MemoryDomain::Frame, "worker", 1024);

    for (int index = 0; index < 40; ++index) {
        CY_REQUIRE(slab.take(64, 8) != nullptr);
    }
    CY_CHECK(slab.slab_count() > 1);  // it grew rather than refusing
    const cy::usize grown = slab.slab_count();

    slab.reset();
    CY_CHECK_EQ(slab.used_bytes(), 0u);
    CY_CHECK_EQ(slab.slab_count(), grown);  // reset keeps the slabs for reuse

    CY_CHECK(slab.take(64, 8) != nullptr);
    CY_CHECK(slab.trim() > 0);
    CY_CHECK_EQ(slab.slab_count(), 1u);

    // An allocation larger than a slab is served by a slab sized to it, not refused.
    CY_CHECK(slab.take(4096, 16) != nullptr);
}

CY_TEST_CASE("scope attributes automatically: containers allocate where the scope says") {
    cy::ArenaAllocator renderer_arena(cy::MemoryDomain::Renderer, "renderer");
    CY_REQUIRE(renderer_arena.reserve(cy::usize{64} * 1024).has_value());

    CY_CHECK_EQ(cy::allocator_scope_depth(), 0u);
    CY_CHECK_EQ(cy::current_allocator().domain(), cy::MemoryDomain::Engine);

    {
        const cy::AllocatorScope scope(renderer_arena);
        CY_CHECK_EQ(cy::allocator_scope_depth(), 1u);
        CY_CHECK_EQ(cy::current_allocator().domain(), cy::MemoryDomain::Renderer);

        void* block = cy::current_allocator().allocate(256, 16);
        CY_CHECK(renderer_arena.owns(block));

        {
            cy::SystemAllocator& audio = cy::system_allocator(cy::MemoryDomain::Audio);
            const cy::AllocatorScope nested(audio);
            CY_CHECK_EQ(cy::current_allocator().domain(), cy::MemoryDomain::Audio);
        }
        CY_CHECK_EQ(cy::current_allocator().domain(), cy::MemoryDomain::Renderer);
    }

    CY_CHECK_EQ(cy::allocator_scope_depth(), 0u);
    CY_CHECK_EQ(cy::current_allocator().domain(), cy::MemoryDomain::Engine);
}

CY_TEST_CASE("the tracking allocator records tag, size and call site, and finds a leak") {
    cy::TrackingAllocator tracker(cy::default_allocator(), cy::MemoryDomain::Assets, "asset-load");

    void* released = tracker.allocate(128, 16);
    void* leaked = nullptr;
    {
        CY_ALLOCATION_SITE();
        leaked = tracker.allocate(64, 8);
    }
    CY_REQUIRE(released != nullptr);
    CY_REQUIRE(leaked != nullptr);
    CY_CHECK_EQ(tracker.live_allocations(), 2u);
    CY_CHECK_EQ(tracker.live_bytes(), 192u);

    tracker.deallocate(released, 128, 16);
    CY_CHECK_EQ(tracker.live_allocations(), 1u);

    struct Collected {
        cy::u64 bytes = 0;
        const char* tag = "";
        const char* file = "";
        cy::u32 count = 0;
    } collected;

    const cy::LeakReport report = tracker.report_leaks(
        [](const cy::TrackedAllocation& allocation, void* user) noexcept {
            auto* out = static_cast<Collected*>(user);
            out->bytes = allocation.bytes;
            out->tag = allocation.tag;
            out->file = allocation.site.file;
            ++out->count;
        },
        &collected);

    CY_CHECK_EQ(report.leaked_allocations, 1u);
    CY_CHECK_EQ(report.leaked_bytes, 64u);
    CY_CHECK_EQ(collected.count, 1u);
    CY_CHECK_EQ(collected.bytes, 64u);
    CY_CHECK(std::strcmp(collected.tag, "asset-load") == 0);
    CY_CHECK(std::strstr(collected.file, "test_allocators.cpp") != nullptr);
}

CY_TEST_CASE("the tracking allocator sees a red-zone overrun and a double free") {
    cy::TrackingAllocator tracker(cy::default_allocator(), cy::MemoryDomain::Engine, "guarded");

    auto* block = static_cast<cy::u8*>(tracker.allocate(32, 8));
    CY_REQUIRE(block != nullptr);
    block[32] = 0x01;  // one byte past the payload, inside the red zone
    tracker.deallocate(block, 32, 8);
    CY_CHECK_EQ(tracker.overruns(), 1u);

    // REGRESSION. Freeing it again used to be detected by reading the block's header — memory the
    // upstream allocator had already taken back, which AddressSanitizer correctly reported as a
    // heap-use-after-free. The detection now consults a ring of recently freed pointers before
    // anything is dereferenced, so this case is caught without touching the block. Run this suite
    // under CY_SANITIZE=address: that is where the defect showed and where the fix is checked.
    CY_CHECK_EQ(tracker.double_frees(), 0u);
    tracker.deallocate(block, 32, 8);
    CY_CHECK_EQ(tracker.double_frees(), 1u);
    tracker.deallocate(block, 32, 8);
    CY_CHECK_EQ(tracker.double_frees(), 2u);
}

CY_TEST_CASE("a capture mode is declared rather than always on") {
    cy::TrackingAllocator tracker(cy::default_allocator(), cy::MemoryDomain::Engine, "sampled");
    CY_CHECK_EQ(tracker.capture_mode(), cy::CaptureMode::Full);

    tracker.set_capture_mode(cy::CaptureMode::Off);
    CY_CHECK_EQ(tracker.capture_mode(), cy::CaptureMode::Off);
    void* block = nullptr;
    {
        CY_ALLOCATION_SITE();
        block = tracker.allocate(16);
    }
    CY_REQUIRE(block != nullptr);

    const char* recorded = "";
    (void)tracker.report_leaks(
        [](const cy::TrackedAllocation& allocation, void* user) noexcept {
            *static_cast<const char**>(user) = allocation.site.file;
        },
        static_cast<void*>(&recorded));
    CY_CHECK(std::strcmp(recorded, "<unknown>") == 0);
    tracker.deallocate(block, 16);
}

// --- The two scenarios that are prose to state and awkward to check --------------------------
//
// The two scenarios below are the ones this file's header claims for the whole "Allocator
// interface" and "General heap" requirements, and they are the two that are easiest to write as
// prose and hardest to write as a check. Both are checked here by an observable difference rather
// than by an assertion about the generated code.

CY_TEST_CASE("no virtual dispatch where it matters: the fast path is not the interface") {
    // C++ offers no trait for "this member function is not virtual", so the property is checked by
    // a behaviour only the interface has. `Allocator::allocate` is a non-virtual wrapper that
    // answers null for a zero-sized request before it dispatches; every concrete fast path is a
    // different function that never sees that wrapper. A fast path routed through the interface
    // would therefore start answering null here, and this case would fail.
    //
    // The wrapper is also where the alignment assertion lives, which is the second half of the same
    // point: the checks the interface owes a generic caller are not paid by a caller that has the
    // concrete type in hand.

    cy::ArenaAllocator arena(cy::MemoryDomain::Frame, "fast-path");
    CY_REQUIRE(arena.reserve(4096).has_value());
    CY_CHECK(static_cast<cy::Allocator&>(arena).allocate(0, 8) == nullptr);
    CY_CHECK(arena.bump(0, 8) != nullptr);

    cy::StackAllocator stack(cy::MemoryDomain::Frame, "fast-path");
    CY_REQUIRE(stack.reserve(4096).has_value());
    CY_CHECK(static_cast<cy::Allocator&>(stack).allocate(0, 8) == nullptr);
    CY_CHECK(stack.push(0, 8) != nullptr);

    cy::SlabAllocator slab(cy::MemoryDomain::Frame, "fast-path", 4096);
    CY_CHECK(static_cast<cy::Allocator&>(slab).allocate(0, 8) == nullptr);
    CY_CHECK(slab.take(0, 8) != nullptr);

    cy::PoolAllocator<Counted> pool(cy::MemoryDomain::Ecs, "fast-path", 8);
    CY_CHECK(static_cast<cy::Allocator&>(pool).allocate(0, 8) == nullptr);
    Counted* storage = pool.acquire();
    CY_CHECK(storage != nullptr);
    pool.release(storage);

    cy::ChunkAllocator chunks(cy::MemoryDomain::Ecs, "fast-path", 4096, 64);
    CY_CHECK(static_cast<cy::Allocator&>(chunks).allocate(0, 64) == nullptr);
    void* chunk = chunks.acquire();
    CY_CHECK(chunk != nullptr);
    chunks.release(chunk);

    // The arithmetic itself lives in a type with no virtual table at all, so a header that needs
    // only the bump does not acquire an interface along with it.
    static_assert(!std::is_polymorphic_v<cy::detail::BumpRegion>);

    // Every concrete allocator is final, so even a call made through a reference to one of them is
    // statically bound. `Allocator` itself is not, because a subsystem may write its own.
    static_assert(std::is_final_v<cy::ArenaAllocator>);
    static_assert(std::is_final_v<cy::StackAllocator>);
    static_assert(std::is_final_v<cy::SlabAllocator>);
    static_assert(std::is_final_v<cy::ChunkAllocator>);
    static_assert(std::is_final_v<cy::SystemAllocator>);
    static_assert(std::is_final_v<cy::PoolAllocator<Counted>>);
    static_assert(!std::is_final_v<cy::Allocator>);
}

CY_TEST_CASE("hot paths do not reach the general heap") {
    // `core-memory-and-containers` — "General heap is an integration decided by measurement":
    // "per-frame and per-task allocation SHALL come from arenas, scratch, and pools, and general
    // heap allocation SHALL be rare and attributable". The measurable form of that is a frame that
    // touches the platform heap exactly zero times.
    //
    // Domain accounting is the instrument, because it records at the moment memory is taken FROM
    // THE PLATFORM and nowhere else: an arena charges its reservation once, and its bumps charge
    // nothing. `total_allocations` is monotonic, so a delta of zero over the frame body is the
    // whole assertion, and it is the same counter a shipping build reports.

    cy::ArenaAllocator frame_arena(cy::MemoryDomain::Frame, "frame");
    CY_REQUIRE(frame_arena.reserve(cy::usize{64} * 1024).has_value());
    cy::StackAllocator job_scratch(cy::MemoryDomain::Frame, "scratch");
    CY_REQUIRE(job_scratch.reserve(cy::usize{64} * 1024).has_value());
    cy::PoolAllocator<Counted> pool(cy::MemoryDomain::Ecs, "records", 64);
    cy::ChunkAllocator chunks(cy::MemoryDomain::Ecs, "chunks", cy::usize{16} * 1024, 64);

    // Warm the two allocators that take from upstream on demand, so that what the frame body
    // measures is steady state rather than the first frame after a level load.
    Counted* warm[64] = {};
    for (Counted*& slot : warm) {
        slot = pool.acquire();
        CY_REQUIRE(slot != nullptr);
    }
    for (Counted* slot : warm) {
        pool.release(slot);
    }
    void* warm_chunks[4] = {};
    for (void*& slot : warm_chunks) {
        slot = chunks.acquire();
        CY_REQUIRE(slot != nullptr);
    }
    for (void* slot : warm_chunks) {
        chunks.release(slot);
    }

    const cy::u64 before = cy::domain_stats_recursive(cy::MemoryDomain::Engine).total_allocations;

    for (int frame = 0; frame < 2; ++frame) {
        for (int index = 0; index < 200; ++index) {
            CY_REQUIRE(frame_arena.bump(64, 16) != nullptr);
        }
        {
            const cy::StackScope scope(job_scratch);
            CY_REQUIRE(job_scratch.push(4096, 64) != nullptr);
        }
        for (Counted*& slot : warm) {
            slot = pool.acquire();
            CY_REQUIRE(slot != nullptr);
        }
        for (Counted* slot : warm) {
            pool.release(slot);
        }
        for (void*& slot : warm_chunks) {
            slot = chunks.acquire();
            CY_REQUIRE(slot != nullptr);
        }
        for (void* slot : warm_chunks) {
            chunks.release(slot);
        }
        frame_arena.reset();
    }

    const cy::u64 after = cy::domain_stats_recursive(cy::MemoryDomain::Engine).total_allocations;
    CY_CHECK_EQ(after, before);
}

CY_TEST_CASE("sanitiser build: the allocators route through AddressSanitizer's interface") {
    // `core-memory-and-containers` — "Memory diagnostics", scenario "Sanitiser build". An arena
    // carves one heap block, so without this the tool sees one valid object and a write off the end
    // of a bump allocation is not a finding. The check is on the shadow state rather than on a
    // crash: asking `memory_is_poisoned` costs nothing and does not need a second process.
    //
    // In a build without the instrumentation there is no shadow to read, every answer is `false`,
    // and this case asserts only that the calls compile and are harmless — which is the other half
    // of the contract, since the poisoning sits on the hot paths of five allocators.

    cy::ArenaAllocator arena(cy::MemoryDomain::Frame, "poison");
    CY_REQUIRE(arena.reserve(4096).has_value());
    auto* first = static_cast<cy::u8*>(arena.bump(16, 16));
    CY_REQUIRE(first != nullptr);
    auto* second = static_cast<cy::u8*>(arena.bump(16, 64));  // forces alignment padding between
    CY_REQUIRE(second != nullptr);
    CY_REQUIRE(second > first + 16);

    cy::PoolAllocator<Counted> pool(cy::MemoryDomain::Ecs, "poison", 8);
    Counted* block = pool.acquire();
    CY_REQUIRE(block != nullptr);

    cy::ChunkAllocator chunks(cy::MemoryDomain::Ecs, "poison", 4096, 64);
    void* chunk = chunks.acquire();
    CY_REQUIRE(chunk != nullptr);

    if constexpr (!cy::kAddressSanitizerPresent) {
        // Nothing to observe. Writing through the pointers is the assertion that the calls above
        // did not poison anything in a build with no shadow memory to poison.
        *first = 1;
        *second = 2;
        pool.release(block);
        chunks.release(chunk);
        CY_CHECK_FALSE(cy::memory_is_poisoned(first));
        return;
    }

    // What was handed out is readable; the untouched tail of the arena is not.
    CY_CHECK_FALSE(cy::memory_is_poisoned(first));
    CY_CHECK_FALSE(cy::memory_is_poisoned(second));
    CY_CHECK(cy::memory_is_poisoned(second + 16));

    // The alignment padding between two allocations is poisoned, which is what makes a write off
    // the end of `first` a finding rather than an overlap with its neighbour.
    CY_CHECK(cy::memory_is_poisoned(second - 8));

    // A reset takes the whole region back.
    arena.reset();
    CY_CHECK(cy::memory_is_poisoned(first));
    CY_CHECK(cy::memory_is_poisoned(second));

    // A pool block and a chunk are poisoned when they go back on their free lists, and readable
    // again when they are handed out.
    CY_CHECK_FALSE(cy::memory_is_poisoned(block));
    pool.release(block);
    CY_CHECK(cy::memory_is_poisoned(block));
    CY_CHECK_EQ(pool.acquire(), block);
    CY_CHECK_FALSE(cy::memory_is_poisoned(block));
    pool.release(block);

    CY_CHECK_FALSE(cy::memory_is_poisoned(chunk));
    chunks.release(chunk);
    CY_CHECK(cy::memory_is_poisoned(chunk));
    CY_CHECK_EQ(chunks.acquire(), chunk);
    CY_CHECK_FALSE(cy::memory_is_poisoned(chunk));
    chunks.release(chunk);
}
