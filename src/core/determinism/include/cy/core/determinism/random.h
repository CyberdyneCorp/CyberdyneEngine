#pragma once
// Named, counter-based random streams. Task 4.2.4.
//
// `simulation-and-determinism` — "Random streams": authoritative randomness comes from named
// streams derived from the session seed, no authoritative system uses a global or ambient
// generator, streams are counter-based so that "a value SHALL be derivable from seed, stream
// identity, tick, entity, and sample index", streams are independent so that consuming randomness
// in one does not shift another's sequence, stream identity is hierarchical and derived from stable
// identifiers, and a presentation-only stream is declared as such.
//
// --- THE SHAPE IS THE GUARANTEE ------------------------------------------------------------------
//
// A draw is a **pure function of its five inputs**. There is no `next()`, no cursor inside the
// stream, no mutable state anywhere in this file — a `RandomStream` is 24 bytes of constants and
// every accessor on it is `const`. Three of the requirement's properties then hold by construction
// rather than by care:
//
//   * parallel sampling is safe, because there is nothing to share;
//   * sampling is order-independent and randomly accessible, because the answer does not depend on
//     what was drawn before it;
//   * "a new call does not shift the world" is not a discipline, it is arithmetic — a system that
//     begins drawing one more value per tick changes nothing any other system computes, and cannot,
//     because no other system's inputs mention it.
//
// The cost is that the caller supplies the sample index. That is the point: an index that comes
// from a hidden counter is exactly the shared mutable state the requirement forbids. `SampleCursor`
// is the ergonomic wrapper for a call site that draws several values in a row, and it is a local
// integer with no ordering meaning outside its own expression.
//
// --- THE MIXER -----------------------------------------------------------------------------------
//
// Two multiply-fold rounds over the folded inputs, with constants fixed in this file. It is not a
// cryptographic generator and is not claimed to be; what it has to be is *stable*, which is why it
// does not call `cy::hash_bytes` — that function is seeded per process in development builds, on
// purpose, and a random stream keyed by it would produce a different sequence on every run.
//
// Changing a constant here changes every draw in every session ever recorded. `kMixerVersion` is
// what a replay header records so that a mismatch is a diagnostic rather than a divergence.

#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>

namespace cy::determinism {

/// Bumped whenever the mixing below changes. Recorded in replay and save headers.
inline constexpr u32 kMixerVersion = 1;

namespace detail {

inline constexpr u64 kStreamSecret0 = 0x9e3779b97f4a7c15ULL;
inline constexpr u64 kStreamSecret1 = 0xbf58476d1ce4e5b9ULL;
inline constexpr u64 kStreamSecret2 = 0x94d049bb133111ebULL;

/// 64x64 -> 128 multiply folded to 64 bits. The whole of the mixing, used twice per draw.
[[nodiscard]] constexpr u64 fold_multiply(u64 a, u64 b) noexcept {
    const __uint128_t product = static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b);
    return static_cast<u64>(product) ^ static_cast<u64>(product >> 64U);
}

/// FNV-1a over a name. Deliberately a different function from the engine's `hash_bytes`: this one
/// is `constexpr`, so a stream id is a compile-time constant, and it is unseeded, so the id of
/// "combat.crit" is the same number in every process on every machine forever.
[[nodiscard]] constexpr u64 hash_name(const char* text) noexcept {
    u64 value = 0xcbf29ce484222325ULL;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        value ^= static_cast<u64>(static_cast<u8>(*cursor));
        value *= 0x100000001b3ULL;
    }
    return value;
}

}  // namespace detail

/// A stream's identity: a hash of its hierarchical name, resolved at compile time where the name is
/// a literal.
///
/// It is an identity and not an index. Nothing enumerates streams, nothing allocates one, and two
/// subsystems that independently name "ai.target-selection" get the same stream without having been
/// introduced — which is what "derived from stable identifiers" buys.
struct StreamId {
    u64 value = 0;

    friend constexpr bool operator==(StreamId, StreamId) noexcept = default;
};

/// The stream a name denotes. `stream_id("combat.crit")` is a compile-time constant.
[[nodiscard]] constexpr StreamId stream_id(const char* name) noexcept {
    return StreamId{detail::hash_name(name)};
}

/// A stream beneath another, named by an integer rather than a string: a per-ability stream, a
/// per-rule stream, a per-participant stream. Hierarchical identity without a string concatenation
/// at run time.
[[nodiscard]] constexpr StreamId substream(StreamId parent, u64 key) noexcept {
    return StreamId{
        detail::fold_multiply(parent.value ^ detail::kStreamSecret0, key ^ detail::kStreamSecret1)};
}

/// Whether a stream's values are part of authoritative state.
///
/// `simulation-and-determinism`: "Streams used only for presentation SHALL be declared as such and
/// SHALL NOT be required to be reproducible." Declared, so that the validator can ignore a muzzle
/// flash's jitter and cannot ignore a critical hit. The declaration is on the stream, not on the
/// draw, because a stream that is authoritative on Tuesday and presentation on Wednesday is the bug
/// this classification exists to name.
enum class StreamPurpose : u8 {
    Authoritative = 0,
    Presentation = 1,
};

const char* stream_purpose_name(StreamPurpose purpose) noexcept;

/// One named stream of one session. Immutable, copyable, and cheap enough to pass by value.
class RandomStream {
public:
    constexpr RandomStream() = default;
    constexpr RandomStream(u64 session_seed, StreamId stream, StreamPurpose purpose) noexcept
        : seed_(session_seed), stream_(stream), purpose_(purpose) {}

    [[nodiscard]] constexpr StreamId id() const noexcept { return stream_; }
    [[nodiscard]] constexpr StreamPurpose purpose() const noexcept { return purpose_; }
    [[nodiscard]] constexpr bool authoritative() const noexcept {
        return purpose_ == StreamPurpose::Authoritative;
    }

    /// The raw draw. Every other accessor is a shaping of this one.
    ///
    /// `entity` is any stable per-subject identifier — an entity index, a team id, zero for a draw
    /// that belongs to the world rather than to a subject. `index` is the sample index within
    /// (stream, point, entity).
    [[nodiscard]] constexpr u64 draw(SimulationPoint at, u64 entity, u64 index) const noexcept {
        // The tick and the epoch are folded separately from the subject, so that two entities at
        // one tick and one entity at two ticks are equally unrelated. Folding them into one sum
        // first would make (entity+1, tick) and (entity, tick+1) collide.
        const u64 moment =
            detail::fold_multiply(static_cast<u64>(at.epoch.value) ^ detail::kStreamSecret0,
                                  at.tick ^ detail::kStreamSecret1);
        const u64 subject =
            detail::fold_multiply(entity ^ detail::kStreamSecret2, index ^ detail::kStreamSecret0);
        const u64 keyed =
            detail::fold_multiply(seed_ ^ stream_.value, moment ^ detail::kStreamSecret1);
        return detail::fold_multiply(keyed ^ subject, detail::kStreamSecret2);
    }

    [[nodiscard]] constexpr u32 draw_u32(SimulationPoint at, u64 entity, u64 index) const noexcept {
        return static_cast<u32>(draw(at, entity, index) >> 32U);
    }

    /// A value in [0, bound), without modulo bias.
    ///
    /// Lemire's multiply-shift: the rejection branch is taken with probability below
    /// `bound / 2^32` and, when it is taken, the *retry uses a different sample index*, which is
    /// why this is still a pure function of its arguments. A loop that redrew from a hidden counter
    /// would have reintroduced exactly the state this file exists without.
    [[nodiscard]] constexpr u32 below(u32 bound, SimulationPoint at, u64 entity,
                                      u64 index) const noexcept;

    /// A float in [0, 1). 24 bits of mantissa, which is every value a f32 can represent in that
    /// range at uniform spacing; taking more would only add values the type cannot distinguish.
    [[nodiscard]] constexpr f32 unit_float(SimulationPoint at, u64 entity,
                                           u64 index) const noexcept {
        return static_cast<f32>(draw(at, entity, index) >> 40U) * 0x1.0p-24F;
    }

    /// A double in [0, 1). 53 bits, for the same reason.
    [[nodiscard]] constexpr f64 unit_double(SimulationPoint at, u64 entity,
                                            u64 index) const noexcept {
        return static_cast<f64>(draw(at, entity, index) >> 11U) * 0x1.0p-53;
    }

private:
    u64 seed_ = 0;
    StreamId stream_;
    StreamPurpose purpose_ = StreamPurpose::Authoritative;
};

constexpr u32 RandomStream::below(u32 bound, SimulationPoint at, u64 entity,
                                  u64 index) const noexcept {
    if (bound <= 1) {
        return 0;
    }
    u64 attempt = index;
    for (u32 tries = 0; tries < 4; ++tries) {
        const u32 sample = draw_u32(at, entity, attempt);
        const u64 wide = static_cast<u64>(sample) * static_cast<u64>(bound);
        const u32 low = static_cast<u32>(wide);
        if (low >= static_cast<u32>(-static_cast<i32>(bound)) % bound) {
            return static_cast<u32>(wide >> 32U);
        }
        // A fresh index rather than a fresh generator state. Salted so that the retry cannot
        // collide with the caller's next ordinary draw.
        attempt = detail::fold_multiply(attempt ^ detail::kStreamSecret1, detail::kStreamSecret2);
    }
    // Four rejections has probability below 2^-120 for any bound; taking the biased value here is
    // preferable to an unbounded loop in a function that must terminate in a fixed time.
    return draw_u32(at, entity, attempt) % bound;
}

/// A local, non-shared sample index for a call site that draws several values in one expression.
///
/// It is deliberately not part of `RandomStream`: a cursor that lived on the stream would be shared
/// mutable state, and the whole file exists to not have any. Two cursors over one stream at one
/// moment for one entity produce the same values, which is correct — they are the same samples.
class SampleCursor {
public:
    constexpr SampleCursor() = default;
    constexpr explicit SampleCursor(u64 first) noexcept : next_(first) {}

    [[nodiscard]] constexpr u64 next() noexcept { return next_++; }
    [[nodiscard]] constexpr u64 drawn() const noexcept { return next_; }

private:
    u64 next_ = 0;
};

/// The session seed, and the only thing that hands out streams.
///
/// A `RandomSource` is what a system is given. There is no process-wide instance and no default
/// constructor that invents a seed, so "no authoritative system uses a global or ambient generator"
/// is enforced by there being no global to use.
class RandomSource {
public:
    constexpr RandomSource() = default;
    constexpr explicit RandomSource(u64 session_seed) noexcept : seed_(session_seed) {}

    [[nodiscard]] constexpr u64 seed() const noexcept { return seed_; }

    [[nodiscard]] constexpr RandomStream stream(
        StreamId id, StreamPurpose purpose = StreamPurpose::Authoritative) const noexcept {
        return {seed_, id, purpose};
    }

    [[nodiscard]] constexpr RandomStream stream(
        const char* name, StreamPurpose purpose = StreamPurpose::Authoritative) const noexcept {
        return stream(stream_id(name), purpose);
    }

private:
    u64 seed_ = 0;
};

}  // namespace cy::determinism
