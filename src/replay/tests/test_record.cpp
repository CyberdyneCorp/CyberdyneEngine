// M9 TASK 1.1 — the record's encoding, and the two things it must not contain.
//
// NO PADDING, because a `memcpy` of the struct would write whatever last occupied those bytes and
// two runs agreeing about every value would produce different files. NO PROVENANCE IN THE DIGEST,
// because a replay's records carry `Replay` provenance where the originals carried `Human`.

#include "fixture.h"

#include <cstring>

using namespace cy::replay_test;
using cy::u32;
using cy::u64;
using cy::u8;

CY_TEST_CASE("replay: a record round-trips through its encoding field for field") {
    LogRecord original = command_record(41, 7, -19);
    original.epoch.value = 3;
    original.subject = 0xDEAD'BEEFULL;
    original.value = 0x1234'5678'9ABC'DEF0ULL;
    original.payload_size = 4;
    original.payload[0] = 1;
    original.payload[3] = 9;

    cy::Array<u8> bytes(allocator());
    CY_REQUIRE(encode(original, bytes).has_value());
    CY_REQUIRE_EQ(bytes.size(), cy::usize{kEncodedRecordSize});

    LogRecord restored;
    CY_REQUIRE(decode(bytes.span(), restored).has_value());
    CY_CHECK(restored.kind == original.kind);
    CY_CHECK_EQ(restored.epoch.value, original.epoch.value);
    CY_CHECK_EQ(restored.tick, original.tick);
    CY_CHECK_EQ(restored.sequence, original.sequence);
    CY_CHECK_EQ(restored.subject, original.subject);
    CY_CHECK_EQ(restored.value, original.value);
    CY_CHECK_EQ(restored.payload_size, original.payload_size);
    CY_CHECK_EQ(restored.payload[3], u8{9});
    CY_CHECK_EQ(restored.command.type, original.command.type);
    CY_CHECK_EQ(restored.command.tick, original.command.tick);
    CY_CHECK_EQ(restored.command.sequence, original.command.sequence);
    CY_CHECK(restored.command.participant == original.command.participant);
    CY_CHECK(restored.command.source == original.command.source);
    CY_CHECK(restored.command.target == original.command.target);
    CY_CHECK(restored.command.provenance.kind == original.command.provenance.kind);
    CY_CHECK_EQ(restored.command.provenance.source, original.command.provenance.source);
    cy::i32 dx = 0;
    CY_REQUIRE(restored.command.read_payload(dx));
    CY_CHECK_EQ(dx, -19);
    // The moment, as a pair. A tick alone does not name a moment because rollback moves it
    // backwards.
    CY_CHECK(restored.point() == original.point());
}

CY_TEST_CASE("replay: the encoding is a function of the values and never of the padding") {
    // Two records built by different routes to the same values. If the encoder copied the struct,
    // the padding around `payload_size` and the tail of an unset payload would differ between them
    // and the bytes would not match — which is the bug this case exists to keep out.
    LogRecord first;
    first.kind = RecordKind::StateHash;
    first.tick = 9;
    first.value = 0xAAAAULL;

    LogRecord second = command_record(9, 3, 5);
    std::memset(static_cast<void*>(&second), 0xCD, sizeof(LogRecord));  // dirty every byte
    second = LogRecord{};
    second.kind = RecordKind::StateHash;
    second.tick = 9;
    second.value = 0xAAAAULL;

    cy::Array<u8> left(allocator());
    cy::Array<u8> right(allocator());
    CY_REQUIRE(encode(first, left).has_value());
    CY_REQUIRE(encode(second, right).has_value());
    CY_REQUIRE_EQ(left.size(), right.size());
    CY_CHECK_EQ(std::memcmp(left.data(), right.data(), left.size()), 0);
}

CY_TEST_CASE("replay: the record digest excludes provenance, so a replay matches its session") {
    LogRecord live = command_record(12, 1, 4);
    LogRecord replayed = live;
    replayed.command.provenance =
        cy::gameplay::Provenance{cy::gameplay::ControlSourceKind::Replay, 99};
    replayed.command.source = cy::gameplay::ControlSourceId::from_slot(88, 1);

    // Same intent, same number. Anything else and the first repair anyone reaches for is to make
    // the replay lie about where its commands came from.
    CY_CHECK_EQ(record_hash(0, live), record_hash(0, replayed));

    // ...and a difference in intent is still a difference.
    LogRecord other = command_record(12, 1, 5);
    CY_CHECK_NE(record_hash(0, live), record_hash(0, other));
    LogRecord later = command_record(13, 1, 4);
    CY_CHECK_NE(record_hash(0, live), record_hash(0, later));
    LogRecord resequenced = command_record(12, 2, 4);
    CY_CHECK_NE(record_hash(0, live), record_hash(0, resequenced));
}

CY_TEST_CASE("replay: a short or malformed buffer is refused rather than read past") {
    cy::Array<u8> bytes(allocator());
    CY_REQUIRE(encode(command_record(1, 0, 0), bytes).has_value());

    LogRecord out;
    CY_CHECK_FALSE(
        decode(cy::Span<const u8>(bytes.data(), kEncodedRecordSize - 1), out).has_value());

    // A kind outside the enumeration, a payload size past the array, a provenance kind past the
    // enumeration. Each would otherwise produce a plausible-looking record.
    cy::Array<u8> broken(allocator());
    CY_REQUIRE(broken.append(bytes.span()).has_value());
    broken[0] = 99;
    CY_CHECK_FALSE(decode(broken.span(), out).has_value());

    broken[0] = static_cast<u8>(RecordKind::ExternalResult);
    broken[2] = 0xFF;
    broken[3] = 0xFF;
    CY_CHECK_FALSE(decode(broken.span(), out).has_value());

    broken[2] = 0;
    broken[3] = 0;
    broken[64] = 0xFF;
    CY_CHECK_FALSE(decode(broken.span(), out).has_value());
}
