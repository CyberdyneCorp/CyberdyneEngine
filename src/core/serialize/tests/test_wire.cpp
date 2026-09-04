// The byte level: byte order, bounds, and stepping over what is not understood.

#include <cy/core/memory/array.h>
#include <cy/core/serialize/wire.h>
#include <cy/test/test.h>

#include "fixtures.h"

using namespace cy;
using namespace cy::serialize;
using cy::serialize::test::test_allocator;

CY_TEST_CASE("integers go out least significant byte first, whatever the host holds") {
    Array<u8> bytes(test_allocator());
    ByteWriter writer(bytes);
    CY_REQUIRE(writer.write_u32(0x1122'3344U).has_value());

    CY_REQUIRE_EQ(bytes.size(), 4U);
    CY_CHECK_EQ(bytes[0], 0x44U);
    CY_CHECK_EQ(bytes[1], 0x33U);
    CY_CHECK_EQ(bytes[2], 0x22U);
    CY_CHECK_EQ(bytes[3], 0x11U);
}

CY_TEST_CASE("a scalar written and decoded reproduces its value") {
    Array<u8> bytes(test_allocator());
    ByteWriter writer(bytes);

    const f32 original = -1234.5678F;
    CY_REQUIRE(writer.write_scalar(WireType::F32, &original, sizeof(original)).has_value());
    CY_REQUIRE_EQ(bytes.size(), 4U);

    f32 recovered = 0.0F;
    CY_REQUIRE(decode_scalar(WireType::F32, bytes.data(), 4, &recovered).has_value());
    CY_CHECK_EQ(recovered, original);
}

CY_TEST_CASE("every fixed-width wire type reports the width it writes") {
    Array<u8> bytes(test_allocator());
    ByteWriter writer(bytes);

    const u64 value = 0xFEDC'BA98'7654'3210ULL;
    CY_REQUIRE(writer.write_scalar(WireType::U64, &value, sizeof(value)).has_value());
    CY_CHECK_EQ(bytes.size(), wire_type_width(WireType::U64));
}

CY_TEST_CASE("a read past the end is an error, not a read") {
    const u8 data[3] = {1, 2, 3};
    ByteReader reader(data, sizeof(data));

    const Expected<u32, Error> value = reader.read_u32();
    CY_REQUIRE_FALSE(value.has_value());
    CY_CHECK_EQ(value.error().code, ErrorCode::OutOfRange);
    // The cursor did not move, so a caller that reports the error is not also obliged to rewind.
    CY_CHECK_EQ(reader.offset(), 0U);
}

CY_TEST_CASE("skipping steps over exactly what it was told to") {
    const u8 data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ByteReader reader(data, sizeof(data));

    CY_REQUIRE(reader.skip(5).has_value());
    CY_CHECK_EQ(reader.remaining(), 3U);
    const Expected<u8, Error> next = reader.read_u8();
    CY_REQUIRE(next.has_value());
    CY_CHECK_EQ(next.value(), 6U);
    CY_CHECK(reader.skip(2).has_value());
    CY_CHECK_FALSE(reader.skip(1).has_value());
}

CY_TEST_CASE("a reference is not a scalar, and saying so is an error rather than a guess") {
    Array<u8> bytes(test_allocator());
    ByteWriter writer(bytes);
    const u32 local = 7;
    CY_CHECK_FALSE(writer.write_scalar(WireType::LocalRef, &local, sizeof(local)).has_value());
    CY_CHECK(is_reference(WireType::LocalRef));
    CY_CHECK(is_reference(WireType::ExternalRef));
    CY_CHECK_FALSE(is_reference(WireType::U32));
}

CY_TEST_CASE("a field kind with no wire mapping decays to bytes rather than being dropped") {
    CY_CHECK_EQ(wire_type_of(reflect::FieldKind::Unsupported), WireType::Bytes);
    CY_CHECK_EQ(wire_type_of(reflect::FieldKind::F64), WireType::F64);
}

CY_TEST_CASE("patching rewrites in place and refuses to run off the end") {
    Array<u8> bytes(test_allocator());
    ByteWriter writer(bytes);
    CY_REQUIRE(writer.write_u32(0).has_value());
    CY_REQUIRE(writer.patch_u32(0, 0xAABB'CCDDU).has_value());
    CY_CHECK_EQ(bytes[0], 0xDDU);
    CY_CHECK_EQ(bytes[3], 0xAAU);
    CY_CHECK_FALSE(writer.patch_u32(1, 0).has_value());
}
