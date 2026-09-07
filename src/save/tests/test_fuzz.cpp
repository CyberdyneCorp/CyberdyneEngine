// Malformed saves: truncated at every length, corrupted at every byte. Task 6.2.
//
// `save-and-persistence` — "Save performance and testing": fuzzing with "truncated chunks,
// corrupted hashes, unknown fields, older schemas, missing plugins, and duplicate identities, all
// of which SHALL fail diagnostically rather than crash". The unknown fields, older schemas and
// missing plugins are in test_container.cpp beside the round trips they modify; the two exhaustive
// sweeps are here.
//
// WHY THIS IS AN INTEGRATION SUITE AND NOT A UNIT ONE. Each case decodes a few hundred variants of
// one chunk, and in the debug profile that is three times the unit budget. `testing-and-quality`'s
// taxonomy is about cost rather than subject and says exactly what to do about it: a test this
// expensive belongs in the next suite up. It was moved rather than shrunk, because the claim being
// made is "at EVERY truncation" and "at EVERY byte".

#include <cy/save/container.h>
#include <cy/test/test.h>

#include "fixtures.h"

#include <cstring>

using namespace cy;
using namespace cy::save;
using namespace cy::save::test;

namespace {

constexpr RegionKey kVillage{0x0A00'0000'0000'0001ULL};

void record_health(Overlay& overlay, PersistentId id, u32 revives) {
    Health health;
    health.revives = revives;
    CY_REQUIRE(overlay.record_component(kVillage, id, health_type(), &health, 2).has_value());
}

}  // namespace

CY_TEST_CASE("a truncated chunk fails diagnostically at every truncation") {
    // "Fuzzing with truncated chunks ... all of which SHALL fail diagnostically rather than crash."
    Overlay written(test_allocator());
    record_health(written, entity(1), 3);
    record_health(written, entity(2), 4);
    Array<u8> bytes(test_allocator());
    CY_REQUIRE(encode_region(written, kVillage, bytes).has_value());

    for (usize length = 0; length < bytes.size(); ++length) {
        Overlay read(test_allocator());
        LoadPolicy policy;
        LoadReport report;
        const Status decoded =
            decode_chunk(Span<const u8>(bytes.data(), length), policy, read, report);
        CY_CHECK_FALSE(decoded.has_value());
        CY_CHECK(report.failed());
    }
}

CY_TEST_CASE("a corrupted byte fails diagnostically rather than crashing") {
    Overlay written(test_allocator());
    record_health(written, entity(1), 3);
    Array<u8> bytes(test_allocator());
    CY_REQUIRE(encode_region(written, kVillage, bytes).has_value());

    for (usize index = 0; index < bytes.size(); ++index) {
        Array<u8> damaged(test_allocator());
        CY_REQUIRE(damaged.append(bytes.span()).has_value());
        damaged[index] ^= 0xFFU;

        Overlay read(test_allocator());
        LoadPolicy policy;
        LoadReport report;
        // Either it is rejected, or the damage fell in a payload byte and the record still parses.
        // What must never happen is a crash, and what must never be reported is success with a
        // failure recorded.
        const Status decoded = decode_chunk(damaged.span(), policy, read, report);
        CY_CHECK_EQ(decoded.has_value(), !report.failed());
    }
}
