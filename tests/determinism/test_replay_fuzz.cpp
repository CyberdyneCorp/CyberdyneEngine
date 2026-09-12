// REPLAY FUZZING: a malformed replay fails diagnostically and never crashes. M10 task 6.4.
//
// `testing-and-quality` puts "replay and save fuzzing, where a malformed save fails diagnostically
// and never crashes" in this suite. `src/save/tests/test_fuzz.cpp` has been the save half since M6
// — it is declared as `determinism.save_fuzz` from src/save/tests/CMakeLists.txt now, so the kind
// holds it rather than a copy of it. THE REPLAY HALF DID NOT EXIST: `read_log()` was exercised only
// by round trips of logs this engine had just written, and a round trip cannot fail the way a file
// off a player's disk fails.
//
// ================================================================================================
// THE INVARIANT, AND WHY IT IS NOT "IT MUST FAIL"
// ================================================================================================
//
// A criterion that only required every mutation to be REFUSED would be satisfied by a `read_log()`
// that refused everything, including valid replays — which is the refusal nobody can ship, and the
// same trap `m9:profile-refused-at-configuration` documents for the configure-time check.
//
// So the claim these cases make is the two-sided one:
//
//   For every single-byte mutation of a valid replay, `read_log()` either REFUSES with a reason, or
//   ACCEPTS a log that hashes to exactly what the original hashed to.
//
// The second branch is real rather than theoretical: a log's file carries slack — the index is read
// past rather than trusted, because `append()` re-derives it — so a byte flipped there changes the
// file without changing the records. That is the correct outcome and this suite says so. What is
// forbidden is the third case: accepting a file whose records are NOT the ones that were written.
//
// ================================================================================================
// WHAT THIS SUITE FOUND ON ITS FIRST RUN, AND THE FIX IS IN src/replay/src/log.cpp
// ================================================================================================
//
// SEVENTY OF THE 1,639 REFUSALS CARRIED `RejectReason::None`. `read_chunk()` appends each decoded
// record to the log, and `RecordLog::append()` refuses a record whose tick goes backwards — which
// is exactly what a corrupted tick field produces — and that branch returned the failure WITHOUT
// setting a reason. So a damaged replay was refused with no verdict at all, against
// `replay-and-rollback`'s "the reason SHALL distinguish a build or content mismatch from a damaged
// file". It survived M9 because nothing ever handed `read_log()` a file the engine had not just
// written itself: a round trip cannot reach that branch. The `without_reason` assertion below is
// the regression test; reverting the fix turns it red at 70.
//
// ================================================================================================
// THE MUTATIONS THAT PROVE THESE ARE CHECKS
// ================================================================================================
//
// Each of the four below was APPLIED to this tree and the suite was WATCHED. Two turn it red and
// two do not, and the two that do not are listed rather than dropped, because which mutations a
// test is blind to is as much a property of it as which it catches.
//
// TURNS IT RED:
//
//   * delete the `out.hash() != recorded_hash` branch from `read_log()` — "a corrupted byte" goes
//     red at `forbidden` = 547 of 1,908, each one a different log accepted as this one.
//   * revert the `RejectReason::Corrupt` assignment beside `out.append()` in `read_chunk()` —
//     `without_reason` goes red at 70, which is the defect above.
//
// DOES NOT, AND THE REASON IS THE SAME BOTH TIMES — the whole-log hash is downstream of them:
//
//   * delete the `out.size() != record_count` branch. Every file that reaches it with the wrong
//     count also hashes differently, so the branch below catches all of them: 1,635 refused, 273
//     accepted as the SAME log, 0 as a different one. The check is redundant for THIS corpus, not
//     useless — it names the failure better than a hash mismatch does.
//   * make `check_header()` accept any magic. `header + 8` still holds the record type id and
//     `header + 12` the record size, so junk is still refused by the next clause: the numbers do
//     not move at all. A criterion resting on the magic alone would have to damage those four bytes
//     and nothing else, which no sweep over a whole file can promise.
//
// And the sweeps are exhaustive over the file rather than over its beginning for the obvious
// reason: shortened to the first sixty-four bytes, only the header is covered.

#include "golden_session.h"

#include <cy/core/memory/array.h>
#include <cy/replay/log.h>
#include <cy/test/test.h>

using namespace cy::determinism_test;
using cy::u64;
using cy::u8;
using cy::usize;

namespace {

/// A valid replay, encoded. Every case starts from this and damages a copy.
[[nodiscard]] bool valid_replay(cy::Array<u8>& bytes, u64& log_hash, cy::u32& records) noexcept {
    Recording recording(cy::replay_test::allocator());
    // The SHORT session, not the committed golden one: both sweeps below decode the whole file once
    // per byte, so their cost is quadratic in its length. `golden_session.h` says so where the
    // constants are.
    if (!record_session(kFuzzTicks, kFuzzCheckpointEvery, recording)) {
        return false;
    }
    log_hash = recording.log_hash;
    records = recording.records;
    for (const u8 byte : recording.bytes) {
        if (!bytes.push_back(byte).has_value()) {
            return false;
        }
    }
    return true;
}

/// Read a span as a replay. Reports which of the three outcomes happened, without the caller having
/// to spell the two-sided invariant out at every call site.
enum class Outcome : cy::u8 {
    Refused,          ///< A reason was given. The ordinary answer for damage.
    AcceptedSameLog,  ///< The mutation landed in slack. Correct, and it must still hash the same.
    AcceptedOtherLog  ///< THE FORBIDDEN ONE: a different log, accepted as if it were this one.
};

[[nodiscard]] Outcome read_outcome(cy::Span<const u8> bytes, u64 expected_hash,
                                   cy::u32 expected_records,
                                   cy::replay::RejectReason& why) noexcept {
    cy::replay::RecordLog out(cy::replay_test::allocator(), cy::replay_test::manifest());
    why = cy::replay::RejectReason::None;
    if (!cy::replay::read_log(bytes, out, why).has_value()) {
        return Outcome::Refused;
    }
    return (out.hash() == expected_hash && out.size() == expected_records)
               ? Outcome::AcceptedSameLog
               : Outcome::AcceptedOtherLog;
}

}  // namespace

CY_TEST_CASE("fuzz: a truncated replay is refused at every truncation, with a reason") {
    cy::Array<u8> bytes(cy::replay_test::allocator());
    u64 log_hash = 0;
    cy::u32 records = 0;
    CY_REQUIRE(valid_replay(bytes, log_hash, records));
    CY_REQUIRE(bytes.size() > 128);

    // EVERY length, including zero: a reader handed an empty span is the case that reaches a header
    // parse with nothing behind it, and it is the one a length-guessing reader gets wrong.
    cy::u32 accepted = 0;
    cy::u32 without_reason = 0;
    for (usize length = 0; length < bytes.size(); ++length) {
        cy::replay::RejectReason why = cy::replay::RejectReason::None;
        const Outcome outcome =
            read_outcome(cy::Span<const u8>(bytes.data(), length), log_hash, records, why);
        if (outcome != Outcome::Refused) {
            ++accepted;
            continue;
        }
        // "Fails DIAGNOSTICALLY": a refusal with `None` for a reason is a refusal a support queue
        // cannot answer, which is the sentence `RejectReason`'s own comment is written around.
        if (why == cy::replay::RejectReason::None) {
            ++without_reason;
        }
    }
    CY_CHECK_EQ(accepted, 0U);
    CY_CHECK_EQ(without_reason, 0U);

    // And the undamaged file is accepted, so the sweep above is not a report about a reader that
    // refuses everything.
    cy::replay::RejectReason why = cy::replay::RejectReason::None;
    CY_CHECK(read_outcome(bytes.span(), log_hash, records, why) == Outcome::AcceptedSameLog);
}

CY_TEST_CASE("fuzz: a corrupted byte never yields a different log accepted as this one") {
    cy::Array<u8> bytes(cy::replay_test::allocator());
    u64 log_hash = 0;
    cy::u32 records = 0;
    CY_REQUIRE(valid_replay(bytes, log_hash, records));

    // EVERY byte of the file, and the mutation is `^ 0xFF` so a zero byte moves as far as any other
    // — a `+ 1` sweep leaves the difference between a flipped bit and a flipped byte untested at
    // exactly the positions a length or a count is stored in.
    cy::u32 refused = 0;
    cy::u32 slack = 0;
    cy::u32 forbidden = 0;
    cy::u32 without_reason = 0;
    for (usize index = 0; index < bytes.size(); ++index) {
        const u8 original = bytes[index];
        bytes[index] = static_cast<u8>(original ^ 0xFFU);
        cy::replay::RejectReason why = cy::replay::RejectReason::None;
        switch (read_outcome(bytes.span(), log_hash, records, why)) {
            case Outcome::Refused:
                ++refused;
                if (why == cy::replay::RejectReason::None) {
                    ++without_reason;
                }
                break;
            case Outcome::AcceptedSameLog:
                ++slack;
                break;
            case Outcome::AcceptedOtherLog:
                ++forbidden;
                break;
        }
        bytes[index] = original;
    }

    // THE CLAIM. Not "everything was refused" — see the header for why that would be the weaker
    // check — but "nothing was accepted as a log it is not".
    CY_CHECK_EQ(forbidden, 0U);
    CY_CHECK_EQ(without_reason, 0U);
    // And the sweep reached the records rather than bouncing off the header: if every mutation were
    // refused by the magic alone, `refused` would still be the file's length and this suite would
    // be testing four bytes. A file with records in it has more refusals than it has header.
    CY_CHECK_GT(refused, 64U);
    CY_TEST_MESSAGE("corrupted ", bytes.size(), " byte(s): ", refused, " refused, ", slack,
                    " accepted as the same log, ", forbidden, " accepted as a different one");
}

CY_TEST_CASE("fuzz: a file that is not a replay at all is Corrupt, never a mismatch") {
    // `replay-and-rollback`: "the reason SHALL distinguish a build or content mismatch from a
    // damaged file", and `RejectReason`'s comment adds that Corrupt is never INFERRED from a
    // mismatch. The inverse matters just as much and is what this case checks: a file that was
    // never a replay must not be reported as a build mismatch, which would send a player looking
    // for a version of the game to install.
    cy::Array<u8> junk(cy::replay_test::allocator());
    for (cy::u32 index = 0; index < 4096; ++index) {
        // A deterministic pattern rather than a random one: this suite's whole subject is that two
        // runs do the same thing, and a fuzzer seeded from the clock is a test that reports a
        // different result every night.
        CY_REQUIRE(junk.push_back(static_cast<u8>((index * 31U) ^ 0xA5U)).has_value());
    }
    cy::replay::RecordLog out(cy::replay_test::allocator(), cy::replay_test::manifest());
    cy::replay::RejectReason why = cy::replay::RejectReason::None;
    CY_CHECK_FALSE(cy::replay::read_log(junk.span(), out, why).has_value());
    CY_CHECK(why == cy::replay::RejectReason::Corrupt);
    CY_CHECK_EQ(out.size(), 0U);
}
