// The per-task scratch arena. Task 3.2.3.

#include "harness.h"

#include <cy/core/jobs/scratch.h>

#include <cstring>

namespace {

using namespace cy;
using namespace cy::jobs;

}  // namespace

CY_TEST_CASE("a scratch arena bumps and reclaims in bulk") {
    ScratchArena arena;
    CY_REQUIRE(arena.initialize(1024).has_value());
    CY_CHECK_EQ(arena.capacity(), 1024u);
    CY_CHECK_EQ(arena.used(), 0u);

    const usize mark = arena.mark();
    u32* first = arena.allocate_array<u32>(16);
    u64* second = arena.allocate_array<u64>(8);
    CY_REQUIRE(first != nullptr);
    CY_REQUIRE(second != nullptr);
    CY_CHECK(arena.used() >= 64 + 64);

    // Alignment is honoured, which is the whole reason the arena aligns rather than packing.
    CY_CHECK_EQ(reinterpret_cast<usize>(second) % alignof(u64), 0u);

    arena.release_to(mark);
    CY_CHECK_EQ(arena.used(), 0u);
    CY_CHECK(arena.high_water() >= 128);
}

CY_TEST_CASE("an exhausted arena refuses rather than asserting") {
    ScratchArena arena;
    CY_REQUIRE(arena.initialize(128).has_value());

    CY_CHECK(arena.allocate_array<u8>(100) != nullptr);
    // A refusal is a result: a task that wants more scratch than a worker has should fall back,
    // and the count is what makes the fallback visible instead of silent.
    CY_CHECK(arena.allocate_array<u8>(100) == nullptr);
    CY_CHECK_EQ(arena.exhaustions(), 1u);
}

CY_TEST_CASE("a zero-length request is not an allocation") {
    ScratchArena arena;
    CY_REQUIRE(arena.initialize(64).has_value());
    CY_CHECK(arena.allocate_array<u32>(0) == nullptr);
    CY_CHECK_EQ(arena.used(), 0u);
    CY_CHECK_EQ(arena.exhaustions(), 0u);
}

CY_TEST_CASE("released scratch is poisoned in a development build") {
    ScratchArena arena;
    CY_REQUIRE(arena.initialize(256).has_value());

    const usize mark = arena.mark();
    u8* bytes = arena.allocate_array<u8>(32);
    CY_REQUIRE(bytes != nullptr);
    std::memset(bytes, 0x11, 32);
    arena.release_to(mark);

    // The poison is Debug and Development only: it is a diagnostic, and Profile and Shipping do not
    // pay for it. This is exactly the profile-dependent behaviour that must be asked about rather
    // than assumed — the mistake that made M0's suite red in two configurations.
    if (cy::jobs::test::assertions_are_live()) {
        CY_CHECK_EQ(bytes[0], kScratchPoison);
        CY_CHECK_EQ(bytes[31], kScratchPoison);
    } else {
        CY_CHECK_EQ(bytes[0], 0x11);
    }
}

CY_TEST_CASE("a scratch scope releases what its body allocated") {
    ScratchArena arena;
    CY_REQUIRE(arena.initialize(1024).has_value());
    CY_REQUIRE(arena.allocate_array<u32>(4) != nullptr);
    const usize before = arena.used();

    {
        const ScratchScope scope(arena);
        CY_REQUIRE(scope.arena().allocate_array<u32>(32) != nullptr);
        CY_CHECK(arena.used() > before);
    }
    CY_CHECK_EQ(arena.used(), before);
}

CY_TEST_CASE("an uninitialised arena allocates nothing") {
    ScratchArena arena;
    CY_CHECK(arena.allocate_array<u32>(1) == nullptr);
    CY_CHECK_EQ(arena.capacity(), 0u);
}
