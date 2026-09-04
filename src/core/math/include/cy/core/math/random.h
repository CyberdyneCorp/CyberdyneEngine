#pragma once
// Random — a seeded, inspectable PCG generator. Task 3.1.6.
//
// `core-math` — "Random number generation": a PCG-family generator with explicit seeding and
// streams, plus helpers for uniform integers and floats, the normal distribution, and sampling on
// a sphere, hemisphere and disk. **Global implicit random state does not exist**: every generator
// is an explicit object, so a simulation can be made reproducible.
//
// There is deliberately no `cy::random()` and no thread-local default. A hidden generator makes
// reproducibility depend on call order across every system in the frame, which is the property
// `simulation-and-determinism` exists to remove; the absence is the feature.
//
// REPRODUCIBLE **AND** INSPECTABLE. The milestone brief asks for both, because M9 will need both.
// Reproducible is the easy half: same seed, same stream, same sequence. Inspectable is
// `snapshot()` / `restore()` and `draws()` — the entire state of a generator is 24 bytes a
// determinism harness can record at a frame boundary, diff against another run, and put back. A
// divergence then has an answer to "which generator, and after how many draws", instead of only
// "the simulation differs".
//
// WHY PCG. The engine needs a generator that is small (it will be one per system, and eventually
// one per entity), that has trivially independent streams, and whose state can be written down.
// `std::mt19937` fails all three: 2.5 KB of state, no stream concept, and a seeding procedure that
// is a poor use of anyone's cache. PCG is 16 bytes, has 2^63 independent streams selected by an
// odd increment, and passes TestU01's BigCrush. It is not cryptographic and is not claimed to be.

#include <cy/core/base/types.h>
#include <cy/core/math/scalar.h>
#include <cy/core/math/vec.h>

namespace cy {

/// The complete internal state of a `Random`, for recording and restoring.
///
/// A plain aggregate of integers and one float: it serialises as bytes, compares with `==`, and has
/// no pointer in it, which is what makes a determinism harness able to write it into a trace and a
/// test able to assert on it.
struct RandomState {
    u64 state = 0;
    u64 increment = 0;
    /// How many 32-bit outputs this generator has produced since it was seeded. Not part of the
    /// generator's mathematics — it exists so that a divergence can be located in time.
    u64 draws = 0;
    /// The second value from a Box–Muller pair, held back for the next `next_normal()`. Part of the
    /// state precisely because it is hidden state: a snapshot that omitted it would restore to a
    /// generator that produces a different normal sequence than the one it was taken from.
    f32 cached_normal = 0.0f;
    bool has_cached_normal = false;
};

[[nodiscard]] constexpr bool operator==(const RandomState& a, const RandomState& b) noexcept {
    return a.state == b.state && a.increment == b.increment && a.draws == b.draws &&
           a.cached_normal == b.cached_normal && a.has_cached_normal == b.has_cached_normal;
}
[[nodiscard]] constexpr bool operator!=(const RandomState& a, const RandomState& b) noexcept {
    return !(a == b);
}

/// A PCG32 generator (`pcg_setseq_64_xsh_rr_32`): a 64-bit LCG whose output is permuted by an
/// xorshift and a data-dependent rotation.
///
/// Copyable and comparable by state, so a system can fork a generator by copying it — and two
/// copies then produce the same sequence, which is either exactly what was wanted or a bug that
/// `snapshot()` makes visible.
class Random {
public:
    /// The default seed and stream. A default-constructed `Random` is deterministic, not
    /// arbitrary: a generator whose sequence depends on when the process started is one that cannot
    /// be reproduced, and reproducing it is the whole requirement. Where a genuinely unpredictable
    /// seed is wanted, the caller says so with `from_entropy()`.
    Random() noexcept : Random(kDefaultSeed, kDefaultStream) {}

    /// `stream` selects one of 2^63 independent sequences. Two generators with the same seed and
    /// different streams do not merely start at different points in one sequence — they walk
    /// different sequences, which is what makes adding a system unable to perturb the others.
    explicit Random(u64 seed, u64 stream = kDefaultStream) noexcept { this->seed(seed, stream); }

    /// A generator seeded from the platform's entropy source. For a session id, a procedural seed
    /// offered to a player, a fuzz run — never on a path whose output has to be reproducible.
    [[nodiscard]] static Random from_entropy() noexcept;

    void seed(u64 seed_value, u64 stream = kDefaultStream) noexcept;

    // --- Raw output -----------------------------------------------------------------------------

    /// One 32-bit output. Every other method here is defined in terms of this one, so the draw
    /// count is a count of these.
    [[nodiscard]] u32 next_u32() noexcept;

    /// Two draws combined high-word first, so the value is a documented function of the sequence
    /// rather than of the platform's word order.
    [[nodiscard]] u64 next_u64() noexcept;

    // --- Uniform --------------------------------------------------------------------------------

    /// Uniform in [0, 1). Built from the top 24 bits — a float has 24 bits of mantissa, and using
    /// all 32 would round some values to exactly 1.0 and break the half-open interval that every
    /// caller assumes.
    [[nodiscard]] f32 next_float() noexcept;

    /// Uniform in [low, high).
    [[nodiscard]] f32 next_float_in(f32 low, f32 high) noexcept;

    /// Uniform in [0, bound), unbiased. Rejects the values in the leading partial block rather than
    /// taking a modulo, which would over-represent the low end by up to one part in 2^32/bound —
    /// invisible in a die roll and quite visible in a loot table.
    [[nodiscard]] u32 next_u32_below(u32 bound) noexcept;

    /// Uniform in [low, high], inclusive at both ends, which is what a die roll and a random index
    /// range both mean when written by hand.
    [[nodiscard]] i32 next_int_in(i32 low, i32 high) noexcept;

    /// True with probability `probability`.
    [[nodiscard]] bool next_bool(f32 probability = 0.5f) noexcept;

    // --- Distributions --------------------------------------------------------------------------

    /// Standard normal (mean 0, standard deviation 1), by the polar Box–Muller method.
    ///
    /// Box–Muller produces two independent values per pair of draws; the second is cached, which is
    /// why `RandomState` carries it. A caller that alternates between `next_normal()` and
    /// `next_float()` therefore sees a different `next_float()` sequence than one that does not —
    /// true of every cached-pair implementation, and the reason the cache is part of the snapshot.
    [[nodiscard]] f32 next_normal() noexcept;

    [[nodiscard]] f32 next_normal_in(f32 mean, f32 standard_deviation) noexcept;

    // --- Geometric sampling
    // -----------------------------------------------------------------------

    /// A uniformly distributed point on the unit sphere. Uniform in *area*, by sampling z uniformly
    /// and the azimuth uniformly — the naive "normalise three normals" is also correct but costs
    /// three normals and a square root, and the naive "random angles" is not uniform at all.
    [[nodiscard]] Vec3 on_unit_sphere() noexcept;

    /// A uniformly distributed point inside the unit sphere (uniform in volume).
    [[nodiscard]] Vec3 in_unit_sphere() noexcept;

    /// A uniformly distributed point on the hemisphere around a unit-length `normal`.
    [[nodiscard]] Vec3 on_hemisphere(Vec3 normal) noexcept;

    /// Cosine-weighted on the hemisphere around a unit-length `normal`: the distribution a diffuse
    /// bounce wants, so that the importance weight cancels and the estimator has no division.
    [[nodiscard]] Vec3 on_cosine_hemisphere(Vec3 normal) noexcept;

    /// A uniformly distributed point in the unit disk (uniform in area).
    [[nodiscard]] Vec2 in_unit_disk() noexcept;

    /// A uniformly distributed point on the unit circle.
    [[nodiscard]] Vec2 on_unit_circle() noexcept;

    // --- Sequences
    // ---------------------------------------------------------------------------------

    /// Fisher–Yates, drawing from the back. `count` elements starting at `first`.
    template <typename T>
    void shuffle(T* first, usize count) noexcept {
        for (usize i = count; i > 1; --i) {
            const auto j = static_cast<usize>(next_u32_below(static_cast<u32>(i)));
            T tmp = first[i - 1];
            first[i - 1] = first[j];
            first[j] = tmp;
        }
    }

    // --- Inspection
    // ----------------------------------------------------------------------------------

    [[nodiscard]] RandomState snapshot() const noexcept { return state_; }

    /// Put a generator back exactly where a snapshot found it. The next output after `restore()` is
    /// the output that followed the snapshot.
    void restore(const RandomState& state) noexcept { state_ = state; }

    /// How many 32-bit outputs this generator has produced since it was seeded.
    [[nodiscard]] u64 draws() const noexcept { return state_.draws; }

    /// The stream this generator walks. Two generators agree on their sequence only if this and the
    /// LCG state both agree.
    [[nodiscard]] u64 stream() const noexcept { return state_.increment >> 1; }

    static constexpr u64 kDefaultSeed = 0x853c49e6748fea9bull;
    static constexpr u64 kDefaultStream = 0xda3e39cb94b95bdbull;

private:
    RandomState state_{};
};

/// Two generators produce the same sequence from here on exactly when their states are equal.
[[nodiscard]] inline bool same_sequence(const Random& a, const Random& b) noexcept {
    return a.snapshot() == b.snapshot();
}

}  // namespace cy
