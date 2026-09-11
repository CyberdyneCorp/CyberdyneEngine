// M9 TASK 1.5 — what each reader does with the one record.

#include "fixture.h"

#include <cy/replay/readers.h>

using namespace cy::replay_test;
using cy::u32;
using cy::u64;

namespace {

/// Ninety ticks, a checkpoint every twenty, two commands and one hash per tick.
[[nodiscard]] cy::Status fill(RecordLog& log) noexcept {
    for (u64 tick = 0; tick < 90; ++tick) {
        if (tick % 20 == 0) {
            if (cy::Status added = log.append(checkpoint_record(tick, tick)); !added) {
                return added;
            }
        }
        for (u32 sequence = 0; sequence < 2; ++sequence) {
            LogRecord record = command_record(tick, sequence, static_cast<cy::i32>(sequence));
            // Two participants, alternating, so the replication cursor has something to filter.
            record.command.participant =
                cy::gameplay::ParticipantId::from_slot(sequence == 0 ? 2 : 3, 1);
            if (cy::Status added = log.append(record); !added) {
                return added;
            }
        }
        if (cy::Status added = log.append(hash_record(tick, tick)); !added) {
            return added;
        }
    }
    return cy::ok();
}

}  // namespace

CY_TEST_CASE("replay: playback seeks to the nearest checkpoint and produces only commands") {
    RecordLog log(allocator(), manifest());
    CY_REQUIRE(fill(log).has_value());

    PlaybackCursor cursor(log);
    LogRecord checkpoint;
    CY_REQUIRE(cursor.seek(45, checkpoint));
    CY_CHECK_EQ(checkpoint.tick, u64{40});

    // A control source produces commands. The hash and checkpoint records at the same tick are for
    // other readers and must not reach the simulation as intent.
    LogRecord one;
    u32 produced = 0;
    while (cursor.next_command_at(45, one)) {
        CY_CHECK(one.kind == RecordKind::Command);
        ++produced;
    }
    CY_CHECK_EQ(produced, 2U);
    CY_CHECK_EQ(cursor.records_read(), 2U);

    // `next_at` sees every kind, which is what a tool inspecting a replay wants.
    cursor.rewind();
    CY_REQUIRE(cursor.seek(40, checkpoint));
    u32 all = 0;
    while (cursor.next_at(40, one)) {
        ++all;
    }
    CY_CHECK_EQ(all, 4U);  // checkpoint + two commands + hash
}

CY_TEST_CASE("replay: the rollback cursor serves a window and refuses outside it") {
    RecordLog log(allocator(), manifest());
    CY_REQUIRE(fill(log).has_value());

    RollbackCursor cursor(log);
    CY_CHECK_FALSE(cursor.open(10, 5).has_value());  // backward simulation is not attempted
    cy::Array<LogRecord> commands(allocator());
    CY_CHECK_FALSE(cursor.commands_for(5, commands).has_value());  // no window open

    CY_REQUIRE(cursor.open(40, 45).has_value());
    CY_CHECK_FALSE(cursor.commands_for(39, commands).has_value());
    CY_CHECK_FALSE(cursor.commands_for(46, commands).has_value());

    for (u64 tick = 40; tick <= 45; ++tick) {
        CY_REQUIRE(cursor.commands_for(tick, commands).has_value());
    }
    CY_REQUIRE_EQ(commands.size(), cy::usize{12});
    // In the recorded order, never re-sorted: "Re-simulation SHALL be identical in path to normal
    // simulation: the same systems, the same commit boundary, the same commands."
    for (cy::usize index = 0; index + 1 < commands.size(); ++index) {
        const bool ordered = commands[index].tick < commands[index + 1].tick ||
                             (commands[index].tick == commands[index + 1].tick &&
                              commands[index].sequence < commands[index + 1].sequence);
        CY_CHECK(ordered);
    }
}

CY_TEST_CASE("replay: the replication cursor filters by participant and counts what it sent") {
    RecordLog log(allocator(), manifest());
    CY_REQUIRE(fill(log).has_value());

    ReplicationInputCursor cursor(log);
    cy::Array<LogRecord> mine(allocator());
    const u64 participant = cy::gameplay::ParticipantId::from_slot(3, 1).bits();
    CY_REQUIRE(cursor.commands_for(7, participant, mine).has_value());
    CY_REQUIRE_EQ(mine.size(), cy::usize{1});
    CY_CHECK_EQ(mine[0].command.participant.bits(), participant);
    CY_CHECK_EQ(cursor.records_sent(), u64{1});

    // A null participant means every participant, which is what a server relaying to spectators
    // wants.
    cy::Array<LogRecord> everything(allocator());
    CY_REQUIRE(cursor.commands_for(7, 0, everything).has_value());
    CY_CHECK_EQ(everything.size(), cy::usize{2});
    CY_CHECK_EQ(cursor.records_sent(), u64{3});
}

CY_TEST_CASE("replay: the crash ring is bounded, ordered oldest first, and says what it lost") {
    CrashReplayBuffer crash(allocator());
    CY_CHECK_FALSE(crash.reserve(0).has_value());  // a ring of none would report empty as full
    CY_REQUIRE(crash.reserve(8).has_value());

    for (u64 tick = 0; tick < 20; ++tick) {
        crash.push(command_record(tick, 0, static_cast<cy::i32>(tick)));
    }
    CY_CHECK_EQ(crash.size(), 8U);
    CY_CHECK_EQ(crash.pushed(), u64{20});
    // A crash artefact that covers the last eight ticks of a twenty-tick session should say so.
    CY_CHECK_EQ(crash.overwritten(), u64{12});

    cy::Array<LogRecord> flushed(allocator());
    CY_REQUIRE(crash.flush(flushed).has_value());
    CY_REQUIRE_EQ(flushed.size(), cy::usize{8});
    for (cy::usize index = 0; index < flushed.size(); ++index) {
        CY_CHECK_EQ(flushed[index].tick, u64{12} + index);
    }

    // An unreserved ring drops rather than allocates — this is the path a crash handler takes — and
    // still counts, so the artefact can say it saw records it could not keep.
    CrashReplayBuffer unreserved(allocator());
    unreserved.push(command_record(1, 0, 0));
    CY_CHECK_EQ(unreserved.size(), 0U);
    CY_CHECK_EQ(unreserved.pushed(), u64{1});
    cy::Array<LogRecord> nothing(allocator());
    CY_REQUIRE(unreserved.flush(nothing).has_value());
    CY_CHECK_EQ(nothing.size(), cy::usize{0});
}
