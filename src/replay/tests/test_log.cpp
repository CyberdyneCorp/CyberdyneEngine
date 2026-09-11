// M9 TASK 1.1 — the log's order, its index, its compression and its compatibility verdict.

#include "fixture.h"

#include <cstring>

using namespace cy::replay_test;
using cy::u32;
using cy::u64;
using cy::u8;

CY_TEST_CASE("replay: the log records in tick order and refuses to sort") {
    RecordLog log(allocator(), manifest());
    CY_REQUIRE(log.append(command_record(0, 0, 1)).has_value());
    CY_REQUIRE(log.append(command_record(5, 0, 2)).has_value());
    CY_REQUIRE(log.append(command_record(5, 1, 3)).has_value());
    // The order records arrive in *is* the order the simulation consumed them. A log that sorted
    // would be a log with an opinion about what happened.
    CY_CHECK_FALSE(log.append(command_record(4, 0, 4)).has_value());
    CY_CHECK_EQ(log.size(), 3U);
}

CY_TEST_CASE("replay: the index finds the nearest checkpoint without scanning the log") {
    RecordLog log(allocator(), manifest());
    // Six chunks' worth of ticks, a checkpoint every 100.
    for (u64 tick = 0; tick < 400; ++tick) {
        if (tick % 100 == 0) {
            CY_REQUIRE(log.append(checkpoint_record(tick, tick * 8)).has_value());
        }
        CY_REQUIRE(log.append(command_record(tick, 0, static_cast<cy::i32>(tick))).has_value());
    }
    CY_CHECK_GT(log.chunk_count(), 1U);
    // 400 ticks at 64 to a chunk is seven chunks, the last of them short. Written as the ceiling
    // rather than as 6, because the division is what a reader would expect and the ceiling is what
    // the index actually holds.
    CY_CHECK_EQ(log.chunk_count(), (400U + kChunkTicks - 1U) / kChunkTicks);

    LogRecord checkpoint;
    CY_REQUIRE(log.nearest_checkpoint(250, checkpoint));
    CY_CHECK_EQ(checkpoint.tick, u64{200});
    CY_CHECK_EQ(checkpoint.value, u64{1600});
    CY_REQUIRE(log.nearest_checkpoint(100, checkpoint));
    CY_CHECK_EQ(checkpoint.tick, u64{100});
    CY_REQUIRE(log.nearest_checkpoint(399, checkpoint));
    CY_CHECK_EQ(checkpoint.tick, u64{300});

    // A seek to the first chunk has nothing before it, and saying so is better than returning the
    // first checkpoint after the target.
    RecordLog late(allocator(), manifest());
    CY_REQUIRE(late.append(command_record(0, 0, 1)).has_value());
    CY_REQUIRE(late.append(checkpoint_record(10, 1)).has_value());
    CY_CHECK_FALSE(late.nearest_checkpoint(5, checkpoint));

    // `lower_bound` agrees with a scan, which is the only comparison that means anything.
    for (const u64 target : {u64{0}, u64{1}, u64{63}, u64{64}, u64{199}, u64{399}, u64{400}}) {
        u32 scanned = log.size();
        for (u32 index = 0; index < log.size(); ++index) {
            if (log.at(index).tick >= target) {
                scanned = index;
                break;
            }
        }
        CY_CHECK_EQ(log.lower_bound(target), scanned);
    }
}

CY_TEST_CASE("replay: the run-length encoding round-trips and is deterministic") {
    // A record is a fixed-size struct that is mostly zeroes in its unused payload, which is the
    // shape run-length encoding is good at. Two identical inputs must give identical bytes: a
    // compressor that was not deterministic would break the file digest a lockstep session
    // compares.
    cy::Array<u8> input(allocator());
    for (u32 index = 0; index < 600; ++index) {
        CY_REQUIRE(input.push_back(static_cast<u8>(index < 200 ? 0 : (index % 7))).has_value());
    }
    cy::Array<u8> first(allocator());
    cy::Array<u8> second(allocator());
    CY_REQUIRE(compress(input.span(), first).has_value());
    CY_REQUIRE(compress(input.span(), second).has_value());
    CY_REQUIRE_EQ(first.size(), second.size());
    CY_CHECK_EQ(std::memcmp(first.data(), second.data(), first.size()), 0);
    CY_CHECK_LT(first.size(), input.size());

    cy::Array<u8> expanded(allocator());
    CY_REQUIRE(decompress(first.span(), input.size(), expanded).has_value());
    CY_REQUIRE_EQ(expanded.size(), input.size());
    CY_CHECK_EQ(std::memcmp(expanded.data(), input.data(), input.size()), 0);

    // A stream that expands to the wrong size is refused rather than accepted short.
    cy::Array<u8> wrong(allocator());
    CY_CHECK_FALSE(decompress(first.span(), input.size() + 1, wrong).has_value());
    // Truncated control bytes are refused too.
    cy::Array<u8> truncated(allocator());
    CY_CHECK_FALSE(
        decompress(cy::Span<const u8>(first.data(), 1), input.size(), truncated).has_value());
}

CY_TEST_CASE("replay: a log written and read back is the same log, and the bytes are stable") {
    RecordLog log(allocator(), manifest());
    for (u64 tick = 0; tick < 200; ++tick) {
        if (tick % 64 == 0) {
            CY_REQUIRE(log.append(checkpoint_record(tick, tick)).has_value());
        }
        CY_REQUIRE(log.append(command_record(tick, 0, static_cast<cy::i32>(tick))).has_value());
        CY_REQUIRE(log.append(hash_record(tick, 0x1000ULL + tick)).has_value());
    }

    cy::Array<u8> first(allocator());
    cy::Array<u8> second(allocator());
    CY_REQUIRE(write_log(log, first).has_value());
    CY_REQUIRE(write_log(log, second).has_value());
    CY_REQUIRE_EQ(first.size(), second.size());
    CY_CHECK_EQ(std::memcmp(first.data(), second.data(), first.size()), 0);

    RecordLog restored(allocator(), manifest());
    RejectReason why = RejectReason::Corrupt;
    CY_REQUIRE(read_log(first.span(), restored, why).has_value());
    CY_CHECK(why == RejectReason::None);
    CY_REQUIRE_EQ(restored.size(), log.size());
    CY_CHECK_EQ(restored.hash(), log.hash());
    for (u32 index = 0; index < log.size(); ++index) {
        CY_CHECK_EQ(record_hash(0, restored.at(index)), record_hash(0, log.at(index)));
    }
    // The numbers this case measured, reported rather than merely asserted: a suite that says
    // "passed" and a suite that says what it measured are different amounts of evidence, and these
    // are the figures docs/design/images/m9-one-record-five-readers.png quotes.
    CY_TEST_MESSAGE("records=", log.size(), " encoded_record=", kEncodedRecordSize,
                    "B chunks=", log.chunk_count(), " file=", first.size(),
                    "B plain=", static_cast<cy::usize>(log.size()) * kEncodedRecordSize,
                    "B log_hash=", log.hash());

    // The index is rebuilt from the records rather than trusted from the file: an index that
    // disagreed with the records would be a second opinion about the same thing.
    CY_CHECK_EQ(restored.chunk_count(), log.chunk_count());
    LogRecord checkpoint;
    CY_REQUIRE(restored.nearest_checkpoint(100, checkpoint));
    CY_CHECK_EQ(checkpoint.tick, u64{64});
}

CY_TEST_CASE("replay: a damaged file is Corrupt and is not confused with a mismatch") {
    // "the reason SHALL distinguish a build or content mismatch from a damaged file."
    RecordLog log(allocator(), manifest());
    for (u64 tick = 0; tick < 40; ++tick) {
        CY_REQUIRE(log.append(command_record(tick, 0, 1)).has_value());
    }
    cy::Array<u8> bytes(allocator());
    CY_REQUIRE(write_log(log, bytes).has_value());

    RecordLog out(allocator(), manifest());
    RejectReason why = RejectReason::None;

    cy::Array<u8> not_a_replay(allocator());
    CY_REQUIRE(not_a_replay.append(bytes.span()).has_value());
    not_a_replay[0] ^= 0xFFU;
    CY_CHECK_FALSE(read_log(not_a_replay.span(), out, why).has_value());
    CY_CHECK(why == RejectReason::Corrupt);

    // A damaged payload byte, which the header's digest catches even though every length still
    // adds up.
    cy::Array<u8> damaged(allocator());
    CY_REQUIRE(damaged.append(bytes.span()).has_value());
    damaged[damaged.size() - 2] ^= 0x01U;
    why = RejectReason::None;
    CY_CHECK_FALSE(read_log(damaged.span(), out, why).has_value());
    CY_CHECK(why == RejectReason::Corrupt);

    // Truncated.
    why = RejectReason::None;
    CY_CHECK_FALSE(read_log(cy::Span<const u8>(bytes.data(), 40), out, why).has_value());
    CY_CHECK(why == RejectReason::Corrupt);
}

CY_TEST_CASE("replay: compatibility is three classes and each mismatch has its own reason") {
    const CompatibilityManifest recorded = manifest();
    MigrationWindow window;
    window.oldest_command_schema = 2;
    window.oldest_state_schema = 1;

    CompatibilityManifest current = recorded;
    CompatibilityVerdict verdict = classify(recorded, current, window);
    CY_CHECK(verdict.verdict == Compatibility::Reproducible);
    CY_CHECK(verdict.reason == RejectReason::None);

    // A different build, same content: playable, and not claiming bit-exactness.
    current = recorded;
    current.project_build = 0x1111ULL;
    verdict = classify(recorded, current, window);
    CY_CHECK(verdict.verdict == Compatibility::Compatible);
    CY_CHECK(verdict.reason == RejectReason::BuildMismatch);

    current = recorded;
    current.content_manifest_hash = 0x2222ULL;
    verdict = classify(recorded, current, window);
    CY_CHECK(verdict.verdict == Compatibility::Compatible);
    CY_CHECK(verdict.reason == RejectReason::ContentMismatch);

    // Below the window, and above what this build knows how to read.
    CompatibilityManifest old = recorded;
    old.command_schema_version = 1;
    verdict = classify(old, current, window);
    CY_CHECK(verdict.verdict == Compatibility::Incompatible);
    CY_CHECK(verdict.reason == RejectReason::SchemaOutsideWindow);

    // A replay written by a NEWER build than this one: its schema version is above what this build
    // knows how to read, which is the other end of the same window.
    CompatibilityManifest ahead_of_this_build = recorded;
    ahead_of_this_build.state_schema_version = 9;
    const CompatibilityManifest this_build = recorded;
    verdict = classify(ahead_of_this_build, this_build, window);
    CY_CHECK(verdict.reason == RejectReason::SchemaOutsideWindow);

    // The one mismatch no migration window can cover.
    current = recorded;
    current.tick_rate_numerator = 30;
    verdict = classify(recorded, current, window);
    CY_CHECK(verdict.verdict == Compatibility::Incompatible);
    CY_CHECK(verdict.reason == RejectReason::TickRateMismatch);

    current = recorded;
    current.profile = cy::determinism::DeterminismProfile::ReplayStable;
    verdict = classify(recorded, current, window);
    CY_CHECK(verdict.reason == RejectReason::ProfileMismatch);
}
