// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_REPLAY_H
#define CY_SAMPLE_RTS_API_REPLAY_H
// replay.h — a played game's command log, written to a file and read back. `--play --record`,
// `--replay`.
//
// A LOCKSTEP GAME IS ITS ORDERS. Every peer of the session executes the same command log and
// computes the same battle, bit for bit (lockstep_session.h). So a game a person played is fully
// described by the orders the session was given — the player's clicks and the opponent's thoughts
// alike, both `Lockstep.order` commands — and the tick each was given for. That is all this file
// keeps: no input, no frame times, no positions. Replaying it records the same orders into a fresh
// session on the same ticks, and the session's digest at the end must be the one the played game
// ended in; `integration.rts_api_play` holds the two to each other.
//
// THE FORMAT is text, one line a record, so a reader can see what was asked:
//
//   cyrts-replay 1
//   ticks 1830
//   digest 5a1f...           the issuing session's, after the last tick
//   follower 5a1f...         the following peer's, which saw only the log
//   order <tick> <kind> <group> <target x raw> <target z raw>     one an order, in log order
//   end
//
// The targets are the raw `Fixed` bits the issuer converted a click into once; a replay never
// converts anything.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/game_backend/lockstep_session.h>

#include <string>
#include <vector>

namespace sample::rts {

/// One order of a recorded game.
struct ReplayOrder {
    /// The session tick it was given for.
    cy::u64 tick = 0;
    cy::game_backend::LockstepOrderPayload order;
};

/// A recorded game.
struct ReplayLog {
    std::vector<ReplayOrder> orders;
    /// Ticks the recorded session ran, and the digests its two peers ended in.
    cy::u64 ticks = 0;
    cy::u64 digest = 0;
    cy::u64 follower_digest = 0;
};

/// The issuing session's log as a `ReplayLog`, with its tick count and digests.
[[nodiscard]] cy::Expected<ReplayLog, cy::Error> capture_replay(
    const cy::game_backend::LockstepSession& session, cy::u64 follower_digest) noexcept;

/// Write `log` to `path`, and read it back.
[[nodiscard]] cy::Status write_replay(const std::string& path, const ReplayLog& log) noexcept;
[[nodiscard]] cy::Status read_replay(const std::string& path, ReplayLog& out) noexcept;

/// Record into `session` every order of `log` given for the tick the session is about to run,
/// from `cursor` on, and move `cursor` past them. What a replay does in place of the game's
/// orders, before each tick.
[[nodiscard]] cy::Status feed_replay(const ReplayLog& log, cy::usize& cursor,
                                     cy::game_backend::LockstepSession& session) noexcept;

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_REPLAY_H
