// Random — reproducible AND inspectable. Task 3.1.6.
//
// `core-math` — "Random number generation" names two scenarios and the milestone brief adds a
// third requirement on top of them:
//
//   * "Reproducible simulation" — the same seed and the same schedule give identical results.
//   * "Independent streams" — several systems each holding their own generator do not perturb one
//     another, and **adding a system does not change what the others draw**. That second half is
//     the one that is easy to miss: it is a statement about streams, not about seeds.
//   * Inspectable, because `simulation-and-determinism` will require it: the whole state is 24
//     bytes that can be recorded, compared and put back, and the draw counter says *when* two runs
//     diverged rather than only that they did.
//
// There is deliberately no test of statistical quality here. PCG's distribution properties are
// established by TestU01 and re-deriving them from a few thousand samples would only assert that
// the sample was small. What is tested is that the *engine's* wrapper does not break them: no bias
// in the bounded integer, no lost state in the snapshot, no hidden global.

#include <cy/core/math/math.h>

#include <cy/test/test.h>

#include <cmath>
#include <vector>

CY_TEST_CASE("Random: the same seed produces the same sequence") {
    // `core-math` — "Reproducible simulation". A default-constructed generator is deterministic
    // too: a generator seeded from the clock cannot be reproduced, and reproducing it is the whole
    // requirement.
    cy::Random a(12345ull);
    cy::Random b(12345ull);
    cy::Random defaulted_a;
    cy::Random defaulted_b;

    for (int i = 0; i < 1000; ++i) {
        CY_REQUIRE_EQ(a.next_u32(), b.next_u32());
        CY_REQUIRE_EQ(defaulted_a.next_u32(), defaulted_b.next_u32());
    }
    CY_CHECK(same_sequence(a, b));

    // A different seed does not.
    cy::Random other(12346ull);
    cy::Random reference(12345ull);
    bool differs = false;
    for (int i = 0; i < 16 && !differs; ++i) {
        differs = other.next_u32() != reference.next_u32();
    }
    CY_CHECK(differs);
}

CY_TEST_CASE("Random: streams are independent, so adding a system perturbs nothing") {
    // The scenario's second half. Two systems, each with its own stream off the same seed: the
    // first system's sequence must be identical whether or not the second exists, and the two must
    // not walk the same sequence at an offset.
    constexpr cy::u64 kSeed = 0xA5A5A5A5ull;
    cy::Random alone(kSeed, 1ull);
    std::vector<cy::u32> without;
    for (int i = 0; i < 256; ++i) {
        without.push_back(alone.next_u32());
    }

    cy::Random system_one(kSeed, 1ull);
    cy::Random system_two(kSeed, 2ull);
    std::vector<cy::u32> with;
    std::vector<cy::u32> second;
    for (int i = 0; i < 256; ++i) {
        with.push_back(system_one.next_u32());
        second.push_back(system_two.next_u32());
    }
    CY_CHECK(with == without);

    // The two streams are different sequences, not one sequence at an offset. Checking for a shared
    // subsequence would be the strong form; checking that no value coincides at any small offset is
    // the practical one and is what a shared-sequence bug would fail.
    for (cy::usize offset = 0; offset < 8; ++offset) {
        cy::usize matches = 0;
        for (cy::usize i = 0; i + offset < with.size(); ++i) {
            matches += with[i + offset] == second[i] ? 1u : 0u;
        }
        CY_REQUIRE(matches < 4u);
    }

    CY_CHECK_EQ(system_one.stream(), 1ull);
    CY_CHECK_EQ(system_two.stream(), 2ull);
}

CY_TEST_CASE("Random: the whole state can be recorded, compared and restored") {
    cy::Random generator(999ull, 3ull);
    for (int i = 0; i < 37; ++i) {
        (void)generator.next_u32();
    }
    // A normal draw first, so the Box-Muller cache is populated and the snapshot has to carry it.
    // A snapshot that omitted the cached value would restore to a generator producing a *different*
    // normal sequence — the exact class of bug an incomplete state record causes.
    (void)generator.next_normal();

    const cy::RandomState saved = generator.snapshot();
    CY_CHECK(saved.has_cached_normal);

    std::vector<cy::f32> expected;
    for (int i = 0; i < 64; ++i) {
        expected.push_back(generator.next_normal());
    }

    generator.restore(saved);
    CY_CHECK(generator.snapshot() == saved);
    for (int i = 0; i < 64; ++i) {
        CY_REQUIRE_EQ(generator.next_normal(), expected[static_cast<cy::usize>(i)]);
    }

    // Two generators produce the same sequence exactly when their states are equal — which is what
    // makes `RandomState` a complete record rather than a partial one.
    cy::Random restored;
    cy::Random also_restored;
    restored.restore(saved);
    also_restored.restore(saved);
    CY_CHECK(same_sequence(restored, also_restored));
    CY_CHECK_EQ(restored.next_normal(), also_restored.next_normal());
    CY_CHECK_EQ(restored.next_normal(), expected[1]);
}

CY_TEST_CASE("Random: the draw counter says when two runs diverged") {
    // The inspectable half. A determinism harness records this at a frame boundary and a divergence
    // then has an answer to "which generator, and after how many draws" rather than only "the
    // simulation differs".
    cy::Random generator(7ull);
    CY_CHECK_EQ(generator.draws(), 0u);

    (void)generator.next_u32();
    CY_CHECK_EQ(generator.draws(), 1u);
    (void)generator.next_u64();  // two 32-bit outputs
    CY_CHECK_EQ(generator.draws(), 3u);
    (void)generator.next_float();
    CY_CHECK_EQ(generator.draws(), 4u);

    // Re-seeding resets the count, so it is a count since the seed rather than since construction.
    generator.seed(7ull);
    CY_CHECK_EQ(generator.draws(), 0u);

    // And the seeding steps themselves are not counted: they are the generator's own business.
    cy::Random fresh(7ull);
    CY_CHECK_EQ(fresh.draws(), 0u);
    CY_CHECK_EQ(fresh.next_u32(), generator.next_u32());
}

CY_TEST_CASE("Random: bounded integers are unbiased and stay in range") {
    cy::Random generator(31337ull);

    // A bound that does not divide 2^32 is where a naive modulo shows its bias. 3 is the smallest
    // such bound and the one where the bias would be largest per draw.
    constexpr cy::u32 kBound = 3;
    constexpr cy::usize kDraws = 60000;
    cy::usize counts[kBound] = {};
    for (cy::usize i = 0; i < kDraws; ++i) {
        const cy::u32 value = generator.next_u32_below(kBound);
        CY_REQUIRE(value < kBound);
        counts[value] += 1;
    }
    // Each bucket should be within a few percent of a third. A modulo bias at this bound is about
    // one part in 1.4 billion and would not be visible here — what this catches is a gross error
    // such as an off-by-one range or a rejection loop that skews the distribution.
    for (const cy::usize count : counts) {
        CY_REQUIRE(count > kDraws / kBound * 9 / 10);
        CY_REQUIRE(count < kDraws / kBound * 11 / 10);
    }

    // Inclusive at both ends, and both ends must actually occur.
    bool saw_low = false;
    bool saw_high = false;
    for (int i = 0; i < 2000; ++i) {
        const cy::i32 value = generator.next_int_in(-2, 2);
        CY_REQUIRE(value >= -2);
        CY_REQUIRE(value <= 2);
        saw_low = saw_low || value == -2;
        saw_high = saw_high || value == 2;
    }
    CY_CHECK(saw_low);
    CY_CHECK(saw_high);

    // A degenerate range is its own answer rather than an error.
    CY_CHECK_EQ(generator.next_int_in(5, 5), 5);
    CY_CHECK_EQ(generator.next_u32_below(1), 0u);
}

CY_TEST_CASE("Random: floats are in [0, 1) and never reach one") {
    cy::Random generator(0xBEEFull);
    cy::f32 highest = 0.0f;
    for (int i = 0; i < 100000; ++i) {
        const cy::f32 value = generator.next_float();
        CY_REQUIRE(value >= 0.0f);
        CY_REQUIRE(value < 1.0f);  // half-open: using all 32 bits would round some values to 1.0
        highest = cy::math::max(highest, value);
    }
    CY_CHECK(highest > 0.999f);

    for (int i = 0; i < 1000; ++i) {
        const cy::f32 value = generator.next_float_in(-3.0f, 7.0f);
        CY_REQUIRE(value >= -3.0f);
        CY_REQUIRE(value < 7.0f);
    }

    // A probability of 0 never fires and 1 always does, which is what makes `next_bool` usable as a
    // gate that can be turned off.
    for (int i = 0; i < 100; ++i) {
        CY_REQUIRE_FALSE(generator.next_bool(0.0f));
        CY_REQUIRE(generator.next_bool(1.0f));
    }
}

CY_TEST_CASE("Random: the normal distribution has the right mean and spread") {
    cy::Random generator(2718ull);
    constexpr cy::usize kSamples = 100000;
    cy::f64 sum = 0.0;
    cy::f64 sum_squares = 0.0;
    for (cy::usize i = 0; i < kSamples; ++i) {
        const cy::f64 value = static_cast<cy::f64>(generator.next_normal());
        sum += value;
        sum_squares += value * value;
    }
    const cy::f64 mean = sum / static_cast<cy::f64>(kSamples);
    const cy::f64 variance = sum_squares / static_cast<cy::f64>(kSamples) - mean * mean;
    // The standard error of the mean at this sample size is about 0.003, so 0.02 is six sigma and
    // will not flake while still catching a shifted or mis-scaled distribution.
    CY_CHECK(std::fabs(mean) < 0.02);
    CY_CHECK(std::fabs(variance - 1.0) < 0.05);

    // The scaled form shifts and stretches it.
    cy::Random scaled(2718ull);
    cy::f64 scaled_sum = 0.0;
    for (cy::usize i = 0; i < 10000; ++i) {
        scaled_sum += static_cast<cy::f64>(scaled.next_normal_in(10.0f, 2.0f));
    }
    CY_CHECK(std::fabs(scaled_sum / 10000.0 - 10.0) < 0.2);
}

CY_TEST_CASE("Random: geometric sampling produces unit-length and in-range results") {
    cy::Random generator(5150ull);
    const cy::Vec3 normal = normalize(cy::Vec3{0.3f, 1.0f, -0.2f});

    cy::Vec3 sphere_sum{};
    for (int i = 0; i < 20000; ++i) {
        const cy::Vec3 on_sphere = generator.on_unit_sphere();
        CY_REQUIRE(cy::math::nearly_equal(length(on_sphere), 1.0f, 1e-4f));
        sphere_sum += on_sphere;

        CY_REQUIRE(length(generator.in_unit_sphere()) <= 1.0f + 1e-5f);

        // A hemisphere sample is on the sphere and on the normal's side.
        const cy::Vec3 hemisphere = generator.on_hemisphere(normal);
        CY_REQUIRE(cy::math::nearly_equal(length(hemisphere), 1.0f, 1e-4f));
        CY_REQUIRE(dot(hemisphere, normal) >= 0.0f);

        const cy::Vec3 cosine = generator.on_cosine_hemisphere(normal);
        CY_REQUIRE(cy::math::nearly_equal(length(cosine), 1.0f, 1e-3f));
        CY_REQUIRE(dot(cosine, normal) >= -1e-5f);

        const cy::Vec2 disk = generator.in_unit_disk();
        CY_REQUIRE(length(disk) <= 1.0f + 1e-5f);
        CY_REQUIRE(cy::math::nearly_equal(length(generator.on_unit_circle()), 1.0f, 1e-5f));
    }

    // Uniform on the sphere means the samples cancel out. A distribution that crowded the poles —
    // the classic result of sampling two angles uniformly — would not.
    CY_CHECK(length(sphere_sum) / 20000.0f < 0.05f);
}

CY_TEST_CASE("Random: shuffle is a permutation and depends on the seed") {
    std::vector<cy::u32> values(64);
    for (cy::u32 i = 0; i < 64; ++i) {
        values[i] = i;
    }
    const std::vector<cy::u32> original = values;

    cy::Random generator(4242ull);
    generator.shuffle(values.data(), values.size());

    // Every element is still present exactly once.
    std::vector<bool> seen(64, false);
    for (const cy::u32 value : values) {
        CY_REQUIRE(value < 64u);
        CY_REQUIRE_FALSE(seen[value]);
        seen[value] = true;
    }
    CY_CHECK(values != original);

    // Reproducible, like everything else here.
    std::vector<cy::u32> again = original;
    cy::Random same_seed(4242ull);
    same_seed.shuffle(again.data(), again.size());
    CY_CHECK(again == values);
}

CY_TEST_CASE("Random: from_entropy gives different generators without throwing") {
    // Entropy seeding exists for a session id or a fuzz run, never for a reproducible path. What is
    // asserted is only that it is usable: two calls differ, and neither aborts on a machine with no
    // entropy device — which is why it does not use std::random_device under -fno-exceptions.
    cy::Random first = cy::Random::from_entropy();
    cy::Random second = cy::Random::from_entropy();
    CY_CHECK_FALSE(same_sequence(first, second));
    CY_CHECK(first.next_float() >= 0.0f);
}
