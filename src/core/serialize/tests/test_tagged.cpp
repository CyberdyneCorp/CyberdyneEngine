// The tagged form: chunks, records, and the two evolution scenarios the specification names.

#include <cy/core/memory/array.h>
#include <cy/core/reflect/field_index.h>
#include <cy/core/serialize/tagged.h>
#include <cy/test/test.h>

#include "fixtures.h"

using namespace cy;
using namespace cy::serialize;
using namespace cy::serialize::test;

namespace {

constexpr u32 kEntitiesChunk = chunk_tag('E', 'N', 'T', 'S');
constexpr u32 kFutureChunk = chunk_tag('F', 'U', 'T', 'R');

/// Write one object as a one-chunk stream. The shape every case here starts from.
[[nodiscard]] Status write_one(Array<u8>& out, const reflect::TypeInfo& type, const void* object,
                               Purpose purpose, u16 schema_version) noexcept {
    TaggedWriter writer(out);
    if (Status begun = writer.begin_stream(); !begun) {
        return begun;
    }
    if (Status opened = writer.begin_chunk(kEntitiesChunk); !opened) {
        return opened;
    }
    if (Status written = writer.write_object(type, object, purpose, schema_version); !written) {
        return written;
    }
    if (Status closed = writer.end_chunk(); !closed) {
        return closed;
    }
    return writer.end_stream();
}

}  // namespace

CY_TEST_CASE("a stream round-trips an object through a record") {
    Spread source;
    source.flag = true;
    source.small_signed = -12;
    source.medium_signed = -1234;
    source.large_signed = -123456;
    source.huge_signed = -1234567890123LL;
    source.small = 200;
    source.medium = 60000;
    source.large = 4000000000U;
    source.huge = 0xFEDC'BA98'7654'3210ULL;
    source.single = 3.25F;
    source.doubled = -2.5e-9;

    Array<u8> bytes(test_allocator());
    CY_REQUIRE(write_one(bytes, spread_type(), &source, Purpose::Asset, 1).has_value());

    TaggedReader reader(bytes.data(), bytes.size());
    CY_REQUIRE(reader.read_header().has_value());
    CY_REQUIRE_EQ(reader.chunk_count(), 1U);

    const Expected<TaggedChunk, Error> chunk = reader.next_chunk();
    CY_REQUIRE(chunk.has_value());
    CY_CHECK_EQ(chunk->tag, kEntitiesChunk);

    Array<ValueRecord> records(test_allocator());
    CY_REQUIRE(read_records(chunk->payload, records).has_value());
    CY_REQUIRE_EQ(records.size(), 1U);
    CY_CHECK_EQ(records[0].type().value(), kSpreadTypeId);
    CY_CHECK_EQ(records[0].schema_version(), 1U);

    reflect::FieldIndex index;
    CY_REQUIRE(index.build(spread_type()).has_value());

    Spread target;
    CY_REQUIRE(record_to_object(records[0], index, &target).has_value());
    CY_CHECK_EQ(target.flag, source.flag);
    CY_CHECK_EQ(target.small_signed, source.small_signed);
    CY_CHECK_EQ(target.medium_signed, source.medium_signed);
    CY_CHECK_EQ(target.large_signed, source.large_signed);
    CY_CHECK_EQ(target.huge_signed, source.huge_signed);
    CY_CHECK_EQ(target.small, source.small);
    CY_CHECK_EQ(target.medium, source.medium);
    CY_CHECK_EQ(target.large, source.large);
    CY_CHECK_EQ(target.huge, source.huge);
    CY_CHECK_EQ(target.single, source.single);
    CY_CHECK_EQ(target.doubled, source.doubled);
}

CY_TEST_CASE("a chunk a reader does not know is stepped over exactly") {
    Array<u8> bytes(test_allocator());
    {
        TaggedWriter writer(bytes);
        CY_REQUIRE(writer.begin_stream().has_value());
        CY_REQUIRE(writer.begin_chunk(kFutureChunk).has_value());
        const u8 payload[7] = {1, 2, 3, 4, 5, 6, 7};
        ByteWriter raw(bytes);
        CY_REQUIRE(raw.write_bytes(payload, sizeof(payload)).has_value());
        CY_REQUIRE(writer.end_chunk().has_value());

        Spread spread;
        CY_REQUIRE(writer.begin_chunk(kEntitiesChunk).has_value());
        CY_REQUIRE(writer.write_object(spread_type(), &spread, Purpose::Asset, 1).has_value());
        CY_REQUIRE(writer.end_chunk().has_value());
        CY_REQUIRE(writer.end_stream().has_value());
    }

    TaggedReader reader(bytes.data(), bytes.size());
    CY_REQUIRE(reader.read_header().has_value());
    CY_REQUIRE_EQ(reader.chunk_count(), 2U);

    const Expected<TaggedChunk, Error> unknown = reader.next_chunk();
    CY_REQUIRE(unknown.has_value());
    CY_CHECK_EQ(unknown->tag, kFutureChunk);
    CY_CHECK_EQ(unknown->payload.size(), 7U);

    // The reader that does not recognise the tag simply did not look at the payload, and the next
    // chunk is still exactly where it should be.
    const Expected<TaggedChunk, Error> known = reader.next_chunk();
    CY_REQUIRE(known.has_value());
    CY_CHECK_EQ(known->tag, kEntitiesChunk);
}

CY_TEST_CASE("a scene containing a disabled plugin's component is written back unchanged") {
    // The specification's "editing without a plugin" scenario. The component's type is not
    // registered anywhere in this process; its record must survive the round trip byte for byte.
    Array<u8> original(test_allocator());
    {
        ValueRecord plugin(test_allocator());
        plugin.set_type(reflect::TypeId(777001));
        plugin.set_schema_version(4);
        const u32 value = 0xCAFE'BABEU;
        const f64 other = 1.5;
        CY_REQUIRE(plugin.set_scalar(reflect::FieldId(11), WireType::U32, &value, 4).has_value());
        CY_REQUIRE(plugin.set_scalar(reflect::FieldId(12), WireType::F64, &other, 8).has_value());

        TaggedWriter writer(original);
        CY_REQUIRE(writer.begin_stream().has_value());
        CY_REQUIRE(writer.begin_chunk(kEntitiesChunk).has_value());
        CY_REQUIRE(writer.write_record(plugin).has_value());
        CY_REQUIRE(writer.end_chunk().has_value());
        CY_REQUIRE(writer.end_stream().has_value());
    }

    // Load and save in a build that knows nothing about type 777001.
    Array<ValueRecord> records(test_allocator());
    {
        TaggedReader reader(original.data(), original.size());
        CY_REQUIRE(reader.read_header().has_value());
        const Expected<TaggedChunk, Error> chunk = reader.next_chunk();
        CY_REQUIRE(chunk.has_value());
        CY_REQUIRE(read_records(chunk->payload, records).has_value());
    }
    CY_REQUIRE_EQ(records.size(), 1U);
    CY_CHECK_EQ(records[0].type().value(), 777001U);
    CY_CHECK_EQ(records[0].schema_version(), 4U);

    Array<u8> rewritten(test_allocator());
    {
        TaggedWriter writer(rewritten);
        CY_REQUIRE(writer.begin_stream().has_value());
        CY_REQUIRE(writer.begin_chunk(kEntitiesChunk).has_value());
        CY_REQUIRE(writer.write_record(records[0]).has_value());
        CY_REQUIRE(writer.end_chunk().has_value());
        CY_REQUIRE(writer.end_stream().has_value());
    }

    CY_REQUIRE_EQ(rewritten.size(), original.size());
    for (usize index = 0; index < original.size(); ++index) {
        CY_REQUIRE_EQ(rewritten[index], original[index]);
    }
}

CY_TEST_CASE("a field added after the data was written takes its default") {
    // Write a Health with only the two identifiers an older build had, then load it into the
    // current type, which has five.
    Array<u8> bytes(test_allocator());
    {
        ValueRecord old(test_allocator());
        old.set_type(reflect::TypeId(kHealthTypeId));
        const f32 maximum = 300.0F;
        CY_REQUIRE(old.set_scalar(reflect::FieldId(kHealthMaximum), WireType::F32, &maximum, 4)
                       .has_value());

        TaggedWriter writer(bytes);
        CY_REQUIRE(writer.begin_stream().has_value());
        CY_REQUIRE(writer.begin_chunk(kEntitiesChunk).has_value());
        CY_REQUIRE(writer.write_record(old).has_value());
        CY_REQUIRE(writer.end_chunk().has_value());
        CY_REQUIRE(writer.end_stream().has_value());
    }

    TaggedReader reader(bytes.data(), bytes.size());
    CY_REQUIRE(reader.read_header().has_value());
    const Expected<TaggedChunk, Error> chunk = reader.next_chunk();
    CY_REQUIRE(chunk.has_value());
    Array<ValueRecord> records(test_allocator());
    CY_REQUIRE(read_records(chunk->payload, records).has_value());

    reflect::FieldIndex index;
    CY_REQUIRE(index.build(health_type()).has_value());

    Health target;
    CY_REQUIRE(record_to_object(records[0], index, &target).has_value());
    CY_CHECK_EQ(target.maximum, 300.0F);
    CY_CHECK_EQ(target.revives, 0U);      // Added later: its default stands.
    CY_CHECK_EQ(target.current, 100.0F);  // Never written for the Asset purpose either.
}

CY_TEST_CASE("a wire type this build does not define is a diagnostic, not a guess") {
    Array<u8> bytes(test_allocator());
    {
        TaggedWriter writer(bytes);
        CY_REQUIRE(writer.begin_stream().has_value());
        CY_REQUIRE(writer.begin_chunk(kEntitiesChunk).has_value());
        ByteWriter raw(bytes);
        CY_REQUIRE(raw.write_u32(kHealthTypeId).has_value());
        CY_REQUIRE(raw.write_u16(1).has_value());   // schema version
        CY_REQUIRE(raw.write_u16(1).has_value());   // one field
        CY_REQUIRE(raw.write_u32(8).has_value());   // payload size
        CY_REQUIRE(raw.write_u32(1).has_value());   // field id
        CY_REQUIRE(raw.write_u8(200).has_value());  // a wire type from the future
        CY_REQUIRE(raw.write_u8(0).has_value());
        CY_REQUIRE(raw.write_u16(0).has_value());
        CY_REQUIRE(writer.end_chunk().has_value());
        CY_REQUIRE(writer.end_stream().has_value());
    }

    TaggedReader reader(bytes.data(), bytes.size());
    CY_REQUIRE(reader.read_header().has_value());
    const Expected<TaggedChunk, Error> chunk = reader.next_chunk();
    CY_REQUIRE(chunk.has_value());
    Array<ValueRecord> records(test_allocator());
    const Status read = read_records(chunk->payload, records);
    CY_REQUIRE_FALSE(read.has_value());
    CY_CHECK_EQ(read.error().code, ErrorCode::Unsupported);
}

CY_TEST_CASE("a stream written by a newer format is refused rather than parsed") {
    Array<u8> bytes(test_allocator());
    ByteWriter raw(bytes);
    CY_REQUIRE(raw.write_u32(kTaggedMagic).has_value());
    CY_REQUIRE(raw.write_u16(kTaggedFormatVersion + 1).has_value());
    CY_REQUIRE(raw.write_u16(0).has_value());
    CY_REQUIRE(raw.write_u32(0).has_value());

    TaggedReader reader(bytes.data(), bytes.size());
    const Status header = reader.read_header();
    CY_REQUIRE_FALSE(header.has_value());
    CY_CHECK_EQ(header.error().code, ErrorCode::Unsupported);
}

CY_TEST_CASE("bytes that are not a tagged stream are refused at the magic") {
    const u8 rubbish[16] = {};
    TaggedReader reader(rubbish, sizeof(rubbish));
    const Status header = reader.read_header();
    CY_REQUIRE_FALSE(header.has_value());
    CY_CHECK_EQ(header.error().code, ErrorCode::InvalidArgument);
}
