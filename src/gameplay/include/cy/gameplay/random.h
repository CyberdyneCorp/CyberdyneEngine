#pragma once
// Deterministic random streams for gameplay, over M2's seeded streams. Task 4.4.5.
//
// `gameplay-framework` — "Deterministic random streams": randomness comes from **named streams
// derived from the session seed**, not from a global generator; streams are independent, "so that
// consuming randomness in one system does not perturb another's sequence — which is what makes a
// change in one feature alter unrelated outcomes in replay"; and streams used for presentation only
// are declared as such.
//
// ================================================================================================
// THIS IS A THIN FILE ON PURPOSE
// ================================================================================================
//
// `core-determinism`'s `RandomSource`, `RandomStream`, `StreamId` and `StreamPurpose` already are
// the mechanism, and they are already right: a stream is an immutable value, a draw is a pure
// function of `(seed, stream, epoch, tick, subject, index)`, and there is no global instance and no
// default constructor that invents a seed. `gameplay-framework` says "Stream derivation,
// counter-based sampling, and inspection are defined in `simulation-and-determinism`; this
// capability requires their use."
//
// So this file **requires their use** and adds nothing that could be got wrong twice. What it does
// add is the binding to a session: the seed comes from `GameSession`, so a gameplay stream cannot
// be derived from anything else, and the per-participant and per-rule substreams are named here so
// that two features do not invent two spellings for the same idea.
//
// THE INDEPENDENCE PROPERTY IS THE ONE TO PROTECT. If a combat stream and a loot stream shared a
// generator, adding one die roll to combat would shift every subsequent loot outcome — and a replay
// recorded before the change would diverge for a reason that has nothing to do with loot. Counter-
// based sampling gives independence by construction, and `tests/test_random.cpp` measures it.

#include <cy/core/base/types.h>
#include <cy/core/determinism/random.h>
#include <cy/gameplay/context.h>

namespace cy::gameplay {

using determinism::RandomStream;
using determinism::SampleCursor;
using determinism::StreamId;
using determinism::StreamPurpose;

/// Every gameplay stream of one session.
///
/// Constructed from a `GameSession`, so there is no path to a stream that is not the session's. A
/// copy is a value and costs nothing; there is no shared state to protect and therefore no lock.
class GameplayRandom {
public:
    constexpr GameplayRandom() = default;
    constexpr explicit GameplayRandom(u64 session_seed) noexcept : source_(session_seed) {}
    explicit GameplayRandom(const GameSession& session) noexcept : source_(session.seed()) {}

    [[nodiscard]] constexpr u64 seed() const noexcept { return source_.seed(); }

    /// A named stream. `stream_id("combat.crit")` is a compile-time constant, so this costs nothing
    /// at run time and the name never reaches the evaluation path.
    [[nodiscard]] constexpr RandomStream stream(
        StreamId id, StreamPurpose purpose = StreamPurpose::Authoritative) const noexcept {
        return source_.stream(id, purpose);
    }
    [[nodiscard]] constexpr RandomStream stream(
        const char* name, StreamPurpose purpose = StreamPurpose::Authoritative) const noexcept {
        return source_.stream(name, purpose);
    }

    /// A stream beneath another, keyed by a participant. Two participants draw from independent
    /// sequences without either a string concatenation or a per-participant registry.
    [[nodiscard]] constexpr RandomStream per_participant(
        StreamId parent, ParticipantId participant,
        StreamPurpose purpose = StreamPurpose::Authoritative) const noexcept {
        return source_.stream(determinism::substream(parent, participant.bits()), purpose);
    }

    /// A stream beneath another, keyed by any integer: a rule index, an ability, a world region.
    [[nodiscard]] constexpr RandomStream keyed(
        StreamId parent, u64 key,
        StreamPurpose purpose = StreamPurpose::Authoritative) const noexcept {
        return source_.stream(determinism::substream(parent, key), purpose);
    }

    /// A stream whose values are **not** part of authoritative state: a muzzle flash's jitter, an
    /// idle animation's variation. Declared here rather than at the draw, because a stream that is
    /// authoritative on Tuesday and presentation on Wednesday is the bug the classification exists
    /// to name — `core-determinism` says so and this spelling makes the declaration hard to skip.
    [[nodiscard]] constexpr RandomStream presentation(const char* name) const noexcept {
        return source_.stream(name, StreamPurpose::Presentation);
    }

private:
    determinism::RandomSource source_;
};

}  // namespace cy::gameplay
