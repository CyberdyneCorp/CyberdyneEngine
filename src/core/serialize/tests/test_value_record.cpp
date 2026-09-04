// The value record: ordering, the classification table, and unknown data surviving by construction.

#include <cy/core/reflect/field_index.h>
#include <cy/core/serialize/value_record.h>
#include <cy/test/test.h>

#include "fixtures.h"

using namespace cy;
using namespace cy::serialize;
using namespace cy::serialize::test;

namespace {

[[nodiscard]] Expected<reflect::FieldIndex, Error> index_of(const reflect::TypeInfo& type) {
    reflect::FieldIndex index;
    if (Status built = index.build(type); !built) {
        return make_unexpected(built.error());
    }
    return index;
}

}  // namespace

CY_TEST_CASE("fields are held in identifier order however they were inserted") {
    ValueRecord record(test_allocator());
    const u32 value = 1;
    CY_REQUIRE(record.set_scalar(reflect::FieldId(9), WireType::U32, &value, 4).has_value());
    CY_REQUIRE(record.set_scalar(reflect::FieldId(3), WireType::U32, &value, 4).has_value());
    CY_REQUIRE(record.set_scalar(reflect::FieldId(7), WireType::U32, &value, 4).has_value());
    CY_REQUIRE(record.set_scalar(reflect::FieldId(1), WireType::U32, &value, 4).has_value());

    CY_REQUIRE_EQ(record.size(), 4U);
    CY_CHECK_EQ(record.fields()[0].id.value(), 1U);
    CY_CHECK_EQ(record.fields()[1].id.value(), 3U);
    CY_CHECK_EQ(record.fields()[2].id.value(), 7U);
    CY_CHECK_EQ(record.fields()[3].id.value(), 9U);
}

CY_TEST_CASE("writing a field twice replaces it rather than adding a second") {
    ValueRecord record(test_allocator());
    u32 first = 11;
    u32 second = 22;
    CY_REQUIRE(record.set_scalar(reflect::FieldId(4), WireType::U32, &first, 4).has_value());
    CY_REQUIRE(record.set_scalar(reflect::FieldId(4), WireType::U32, &second, 4).has_value());

    CY_REQUIRE_EQ(record.size(), 1U);
    u32 read = 0;
    const Span<const u8> bytes = record.bytes(reflect::FieldId(4));
    CY_REQUIRE(decode_scalar(WireType::U32, bytes.data(), 4, &read).has_value());
    CY_CHECK_EQ(read, 22U);
}

CY_TEST_CASE("the Asset purpose writes Authoring and nothing else") {
    Health health;
    health.maximum = 250.0F;
    health.current = 37.0F;
    health.revives = 2;
    health.fraction = 0.148F;
    health.debug_counter = 99;

    ValueRecord record(test_allocator());
    CY_REQUIRE(record_from_object(health_type(), &health, Purpose::Asset, record).has_value());

    CY_CHECK(record.contains(reflect::FieldId(kHealthMaximum)));
    CY_CHECK_FALSE(record.contains(reflect::FieldId(kHealthCurrent)));
    CY_CHECK_FALSE(record.contains(reflect::FieldId(kHealthRevives)));
    // Derived is never serialised, and Transient is excluded from everything.
    CY_CHECK_FALSE(record.contains(reflect::FieldId(kHealthFraction)));
    CY_CHECK_FALSE(record.contains(reflect::FieldId(kHealthDebugCounter)));
}

CY_TEST_CASE("the Persistence purpose writes PersistentState and nothing else") {
    Health health;
    health.revives = 4;

    ValueRecord record(test_allocator());
    CY_REQUIRE(
        record_from_object(health_type(), &health, Purpose::Persistence, record).has_value());
    CY_REQUIRE_EQ(record.size(), 1U);
    CY_CHECK(record.contains(reflect::FieldId(kHealthRevives)));
}

CY_TEST_CASE("the Snapshot purpose carries the running state a restore has to reproduce") {
    Health health;
    health.current = 12.5F;

    ValueRecord record(test_allocator());
    CY_REQUIRE(record_from_object(health_type(), &health, Purpose::Snapshot, record).has_value());
    CY_CHECK(record.contains(reflect::FieldId(kHealthMaximum)));
    CY_CHECK(record.contains(reflect::FieldId(kHealthCurrent)));
    CY_CHECK(record.contains(reflect::FieldId(kHealthRevives)));
    CY_CHECK_FALSE(record.contains(reflect::FieldId(kHealthFraction)));
    CY_CHECK_FALSE(record.contains(reflect::FieldId(kHealthDebugCounter)));
}

CY_TEST_CASE("a record applies to an object and leaves absent fields at their defaults") {
    Health source;
    source.maximum = 500.0F;
    source.current = 1.0F;

    ValueRecord record(test_allocator());
    CY_REQUIRE(record_from_object(health_type(), &source, Purpose::Asset, record).has_value());

    Expected<reflect::FieldIndex, Error> index = index_of(health_type());
    CY_REQUIRE(index.has_value());

    Health target;  // Its defaults are what an absent field must leave alone.
    u32 applied = 0;
    u32 skipped = 0;
    CY_REQUIRE(record_to_object(record, index.value(), &target, &applied, &skipped).has_value());

    CY_CHECK_EQ(applied, 1U);
    CY_CHECK_EQ(skipped, 0U);
    CY_CHECK_EQ(target.maximum, 500.0F);
    CY_CHECK_EQ(target.current, 100.0F);  // Not in the record: the type's default stands.
}

CY_TEST_CASE("a field this build has never heard of is kept and reported, never dropped") {
    ValueRecord record(test_allocator());
    Health health;
    CY_REQUIRE(record_from_object(health_type(), &health, Purpose::Asset, record).has_value());

    // A field written by a build with one more field than this one.
    const u32 stranger = 0xDEAD'BEEFU;
    CY_REQUIRE(record.set_scalar(reflect::FieldId(4242), WireType::U32, &stranger, 4).has_value());

    Expected<reflect::FieldIndex, Error> index = index_of(health_type());
    CY_REQUIRE(index.has_value());

    Health target;
    u32 applied = 0;
    u32 skipped = 0;
    CY_REQUIRE(record_to_object(record, index.value(), &target, &applied, &skipped).has_value());
    CY_CHECK_EQ(applied, 1U);
    CY_CHECK_EQ(skipped, 1U);
    // Applying the record did not consume it: the unknown field is still there to be written back.
    CY_CHECK(record.contains(reflect::FieldId(4242)));
}

CY_TEST_CASE("a width the schema disagrees with is an error rather than a reinterpretation") {
    ValueRecord record(test_allocator());
    record.set_type(reflect::TypeId(kHealthTypeId));
    const u64 too_wide = 1;
    CY_REQUIRE(record.set_scalar(reflect::FieldId(kHealthMaximum), WireType::U64, &too_wide, 8)
                   .has_value());

    Expected<reflect::FieldIndex, Error> index = index_of(health_type());
    CY_REQUIRE(index.has_value());

    Health target;
    const Status applied = record_to_object(record, index.value(), &target);
    CY_REQUIRE_FALSE(applied.has_value());
    CY_CHECK_EQ(applied.error().code, ErrorCode::InvalidArgument);
}

CY_TEST_CASE("overlay is the override composition operator: later layers win, others are kept") {
    ValueRecord base(test_allocator());
    const u32 one = 1;
    const u32 two = 2;
    CY_REQUIRE(base.set_scalar(reflect::FieldId(1), WireType::U32, &one, 4).has_value());
    CY_REQUIRE(base.set_scalar(reflect::FieldId(2), WireType::U32, &one, 4).has_value());

    ValueRecord layer(test_allocator());
    CY_REQUIRE(layer.set_scalar(reflect::FieldId(2), WireType::U32, &two, 4).has_value());

    CY_REQUIRE(base.overlay(layer).has_value());
    CY_REQUIRE_EQ(base.size(), 2U);

    u32 read = 0;
    CY_REQUIRE(
        decode_scalar(WireType::U32, base.bytes(reflect::FieldId(1)).data(), 4, &read).has_value());
    CY_CHECK_EQ(read, 1U);
    CY_REQUIRE(
        decode_scalar(WireType::U32, base.bytes(reflect::FieldId(2)).data(), 4, &read).has_value());
    CY_CHECK_EQ(read, 2U);
}

CY_TEST_CASE("retargeting moves a value to a new identifier and refuses to merge two into one") {
    ValueRecord record(test_allocator());
    const u32 value = 77;
    CY_REQUIRE(record.set_scalar(reflect::FieldId(1), WireType::U32, &value, 4).has_value());
    CY_REQUIRE(record.retarget(reflect::FieldId(1), reflect::FieldId(5)).has_value());

    CY_CHECK_FALSE(record.contains(reflect::FieldId(1)));
    u32 read = 0;
    CY_REQUIRE(decode_scalar(WireType::U32, record.bytes(reflect::FieldId(5)).data(), 4, &read)
                   .has_value());
    CY_CHECK_EQ(read, 77U);

    const u32 other = 9;
    CY_REQUIRE(record.set_scalar(reflect::FieldId(2), WireType::U32, &other, 4).has_value());
    const Status merged = record.retarget(reflect::FieldId(2), reflect::FieldId(5));
    CY_REQUIRE_FALSE(merged.has_value());
    CY_CHECK_EQ(merged.error().code, ErrorCode::AlreadyExists);
}

CY_TEST_CASE("retargeting many fields does not read the pool it is growing") {
    // A regression guard rather than a behaviour test: `retarget` used to hand `set` a span of the
    // record's own byte pool, and `set` appends to that pool. The first append that reallocated
    // read freed memory. Enough fields to guarantee several growths, each value distinct so a stale
    // read shows up as a wrong number rather than as a crash the sanitizers might be the only
    // witness to.
    ValueRecord record(test_allocator());
    for (u32 index = 1; index <= 64; ++index) {
        const u64 value = 0x1000'0000ULL + index;
        CY_REQUIRE(
            record.set_scalar(reflect::FieldId(index), WireType::U64, &value, 8).has_value());
    }
    for (u32 index = 1; index <= 64; ++index) {
        CY_REQUIRE(
            record.retarget(reflect::FieldId(index), reflect::FieldId(1000 + index)).has_value());
    }
    for (u32 index = 1; index <= 64; ++index) {
        u64 read = 0;
        const Span<const u8> bytes = record.bytes(reflect::FieldId(1000 + index));
        CY_REQUIRE_EQ(bytes.size(), 8U);
        CY_REQUIRE(decode_scalar(WireType::U64, bytes.data(), 8, &read).has_value());
        CY_CHECK_EQ(read, 0x1000'0000ULL + index);
    }
}

CY_TEST_CASE("a clone is deep: mutating the copy does not touch the original") {
    ValueRecord original(test_allocator());
    original.set_type(reflect::TypeId(kHealthTypeId));
    original.set_schema_version(3);
    const u32 value = 5;
    CY_REQUIRE(original.set_scalar(reflect::FieldId(1), WireType::U32, &value, 4).has_value());

    ValueRecord copy(test_allocator());
    CY_REQUIRE(original.clone_into(copy).has_value());
    CY_CHECK_EQ(copy.type(), original.type());
    CY_CHECK_EQ(copy.schema_version(), 3U);

    CY_CHECK(copy.remove(reflect::FieldId(1)));
    CY_CHECK(original.contains(reflect::FieldId(1)));
}

CY_TEST_CASE("a local reference round-trips, and reading one as anything else fails") {
    ValueRecord record(test_allocator());
    CY_REQUIRE(record.set_local_reference(reflect::FieldId(1), 4242).has_value());

    const Expected<u32, Error> local = record.local_reference(reflect::FieldId(1));
    CY_REQUIRE(local.has_value());
    CY_CHECK_EQ(local.value(), 4242U);

    const u32 plain = 1;
    CY_REQUIRE(record.set_scalar(reflect::FieldId(2), WireType::U32, &plain, 4).has_value());
    CY_CHECK_FALSE(record.local_reference(reflect::FieldId(2)).has_value());
}
