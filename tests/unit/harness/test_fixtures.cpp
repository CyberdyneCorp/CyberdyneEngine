// The injectable fixtures: a clock a test drives, and a generator whose sequence is a function of
// its seed. `testing-and-quality` requires both — time and randomness are supplied to a test, never
// sourced from the process.

#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include <cstdint>

CY_TEST_CASE("clock: time moves only when the test moves it") {
    cy::test::DeterministicClock clock;
    CY_CHECK_EQ(clock.now_ns(), 0ULL);

    clock.advance_ns(1500);
    CY_CHECK_EQ(clock.now_ns(), 1500ULL);

    clock.advance_ns(500);
    CY_CHECK_EQ(clock.now_ns(), 2000ULL);
}

CY_TEST_CASE("clock: frames advance by the fixed step of their rate") {
    cy::test::DeterministicClock clock{1000};
    clock.advance_frames(3, 60);
    CY_CHECK_EQ(clock.now_ns(), 1000ULL + (3ULL * (1000000000ULL / 60ULL)));

    cy::test::DeterministicClock other;
    other.advance_frames(120, 120);
    CY_CHECK_EQ(other.now_ns(), 120ULL * (1000000000ULL / 120ULL));
}

CY_TEST_CASE("random: the same seed produces the same sequence") {
    cy::test::SeededRandom first{12345};
    cy::test::SeededRandom second{12345};

    for (int i = 0; i < 32; ++i) {
        CY_CHECK_EQ(first.next_u64(), second.next_u64());
    }
}

CY_TEST_CASE("random: a different seed produces a different sequence") {
    cy::test::SeededRandom first{1};
    cy::test::SeededRandom second{2};

    int identical = 0;
    for (int i = 0; i < 32; ++i) {
        if (first.next_u64() == second.next_u64()) {
            ++identical;
        }
    }
    CY_CHECK_EQ(identical, 0);
}

CY_TEST_CASE("random: the sequence is pinned, not merely reproducible in this process") {
    // A generator that is only reproducible within one build is no use to a determinism test that
    // compares runs on two platforms. These values were produced by the xorshift64* in
    // cy/test/fixtures.h and are part of its contract: changing them is changing the fixture.
    cy::test::SeededRandom random{0x5eed'5eed'5eed'5eedULL};
    CY_CHECK_EQ(random.next_u64(), 0x1dd781b8081c32a0ULL);
    CY_CHECK_EQ(random.next_u64(), 0x490821fb468f800bULL);
    CY_CHECK_EQ(random.next_u64(), 0xefd7d673b12429beULL);
}

CY_TEST_CASE("random: a bounded draw stays inside its bound") {
    cy::test::SeededRandom random{7};
    for (int i = 0; i < 256; ++i) {
        const std::uint32_t value = random.next_below(10);
        CY_CHECK_LT(value, 10U);
    }
}

CY_TEST_CASE("random: a unit draw stays in [0, 1)") {
    cy::test::SeededRandom random{99};
    for (int i = 0; i < 256; ++i) {
        const double value = random.next_unit();
        CY_CHECK_GE(value, 0.0);
        CY_CHECK_LT(value, 1.0);
    }
}
