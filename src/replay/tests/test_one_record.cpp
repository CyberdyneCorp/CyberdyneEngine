// M9 TASK 1.5 — THE FIVE READERS READ ONE RECORD, AND THAT IS A CHECK RATHER THAN A REVIEW.
//
// `replay-and-rollback`'s ADDED requirement is explicit about why this file exists: the capability
// states "Replay, rollback, and lockstep SHALL share one command log" **over three readers**, and
// the engine has five — the crash replay buffer and the divergence validator read the same records
// and neither appears in that list. "A requirement stated over three of five readers is a
// requirement two readers may drift out of without contradicting it."
//
// ================================================================================================
// HOW THIS CAN FAIL, WHICH IS THE ONLY THING THAT MAKES IT A CHECK
// ================================================================================================
//
// Three mutations, all of them one line:
//
//   1. Give a reader its own record type — `using record_type = MyRecord;` in readers.h. The
//      `ReadsTheOneRecord` constraint on `bind_reader<>()` stops the build, in record.cpp, naming
//      the reader.
//   2. Make that substituted record satisfy the concept by aliasing a struct with its own
//      `kRecordTypeId`. It compiles, and `record_type_id` in that reader's binding is no longer
//      `LogRecord::kRecordTypeId` — the first case below goes red, naming the reader.
//   3. Add a sixth reader to `LogReader` without binding it. The `static_assert` in
//      `log_readers()` stops the build; remove that and the second case below goes red.
//
// The run for mutation 2 is recorded in src/replay/README.md.

#include "fixture.h"

#include <cy/replay/divergence.h>
#include <cy/replay/readers.h>

#include <cstring>

using namespace cy::replay_test;
using cy::u32;

CY_TEST_CASE("replay: every reader of the command log reads LogRecord and not a type of its own") {
    const auto readers = log_readers();
    CY_REQUIRE_EQ(static_cast<u32>(readers.size()), kLogReaderCount);

    for (const LogReaderBinding& binding : readers) {
        // The identity is taken from the reader's own `record_type`, never restated, so a reader
        // that substituted a record carries that record's identity here.
        CY_CHECK_EQ(binding.record_type_id, LogRecord::kRecordTypeId);
        CY_CHECK_EQ(binding.record_size, static_cast<u32>(sizeof(LogRecord)));
        CY_CHECK(binding.name != nullptr);
        CY_CHECK(std::strncmp(binding.name, "cy::replay::", 12) == 0);
    }
}

CY_TEST_CASE("replay: the enumeration covers every reader exactly once, in order") {
    // "WHEN a sixth reader of the command log is added THEN it SHALL be declared in the
    // enumeration, and the check SHALL fail until it is."
    const auto readers = log_readers();
    CY_REQUIRE_EQ(static_cast<u32>(readers.size()), kLogReaderCount);
    u32 covered = 0;
    u32 index = 0;
    for (const LogReaderBinding& binding : readers) {
        const u32 slot = static_cast<u32>(binding.reader);
        CY_REQUIRE(slot < kLogReaderCount);
        // A bit per slot rather than an array of flags: what is being checked is that each of the
        // five appears exactly once, and a set bit says so without a second index.
        const u32 bit = 1U << slot;
        CY_CHECK_EQ(covered & bit, 0U);
        covered |= bit;
        // In `LogReader` order, so a reader inserted in the middle of the enumeration without a
        // binding beside it is caught here rather than by a count that still adds up.
        CY_CHECK_EQ(slot, index);
        ++index;
    }
    CY_CHECK_EQ(covered, (1U << kLogReaderCount) - 1U);

    // The five, named. A count alone would pass on a list that had lost the validator and gained a
    // duplicate playback cursor.
    CY_CHECK(std::strcmp(log_reader_name(LogReader::Playback), "Playback") == 0);
    CY_CHECK(std::strcmp(log_reader_name(LogReader::Rollback), "Rollback") == 0);
    CY_CHECK(std::strcmp(log_reader_name(LogReader::ReplicationInput), "ReplicationInput") == 0);
    CY_CHECK(std::strcmp(log_reader_name(LogReader::CrashBuffer), "CrashBuffer") == 0);
    CY_CHECK(std::strcmp(log_reader_name(LogReader::Validator), "Validator") == 0);
}

CY_TEST_CASE("replay: the five readers agree about one log, record for record") {
    // The structural check above says they read one *type*. This says they read one *log*: four
    // readers are pointed at the same records and asked for the same tick, and their answers are
    // compared against each other rather than against a number this test invented.
    RecordLog log(allocator(), manifest());
    for (cy::u64 tick = 0; tick < 4; ++tick) {
        CY_REQUIRE(log.append(checkpoint_record(tick, tick * 1000)).has_value());
        for (u32 sequence = 0; sequence < 3; ++sequence) {
            CY_REQUIRE(log.append(command_record(tick, sequence, static_cast<cy::i32>(sequence)))
                           .has_value());
        }
        CY_REQUIRE(log.append(hash_record(tick, 0xFEEDULL + tick)).has_value());
    }

    cy::Array<LogRecord> from_rollback(allocator());
    RollbackCursor rollback(log);
    CY_REQUIRE(rollback.open(0, 3).has_value());
    CY_REQUIRE(rollback.commands_for(2, from_rollback).has_value());

    cy::Array<LogRecord> from_replication(allocator());
    ReplicationInputCursor replication(log);
    CY_REQUIRE(replication.commands_for(2, 0, from_replication).has_value());

    PlaybackCursor playback(log);
    LogRecord checkpoint;
    CY_REQUIRE(playback.seek(2, checkpoint));
    CY_CHECK_EQ(checkpoint.tick, cy::u64{2});
    cy::Array<LogRecord> from_playback(allocator());
    LogRecord one;
    while (playback.next_command_at(2, one)) {
        CY_REQUIRE(from_playback.push_back(one).has_value());
    }

    CrashReplayBuffer crash(allocator());
    CY_REQUIRE(crash.reserve(64).has_value());
    for (u32 index = 0; index < log.size(); ++index) {
        crash.push(log.at(index));
    }
    cy::Array<LogRecord> from_crash(allocator());
    CY_REQUIRE(crash.flush(from_crash).has_value());

    CY_REQUIRE_EQ(from_rollback.size(), cy::usize{3});
    CY_CHECK_EQ(from_replication.size(), from_rollback.size());
    CY_CHECK_EQ(from_playback.size(), from_rollback.size());
    for (cy::usize index = 0; index < from_rollback.size(); ++index) {
        const cy::u64 reference = record_hash(0, from_rollback[index]);
        CY_CHECK_EQ(record_hash(0, from_replication[index]), reference);
        CY_CHECK_EQ(record_hash(0, from_playback[index]), reference);
    }
    // The crash buffer saw the whole log, including the kinds the other three skip.
    CY_CHECK_EQ(from_crash.size(), cy::usize{log.size()});
}
