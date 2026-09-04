// The cooked form: packed columns, the reference-site table, and a schema mismatch being fatal.

#include <cy/core/memory/array.h>
#include <cy/core/serialize/cooked.h>
#include <cy/test/test.h>

#include "fixtures.h"

using namespace cy;
using namespace cy::serialize;
using namespace cy::serialize::test;

namespace {

struct Transform {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 z = 0.0F;
};

struct Target {
    u64 entity = 0;  ///< An entity reference, eight bytes, at offset zero.
    u32 slot = 0;
};

[[nodiscard]] u64 schema_of(const reflect::TypeInfo& type, u16 version) noexcept {
    BuildSchemaDigest digest;
    digest.add_type(type, version);
    return digest.value();
}

}  // namespace

CY_TEST_CASE("a block round-trips its columns byte for byte") {
    constexpr u32 kRows = 64;
    Array<u8> payload(test_allocator());
    for (u32 row = 0; row < kRows; ++row) {
        const Transform transform{static_cast<f32>(row), static_cast<f32>(row) * 2.0F, -1.0F};
        CY_REQUIRE(
            payload
                .append(Span<const u8>(reinterpret_cast<const u8*>(&transform), sizeof(transform)))
                .has_value());
    }

    Array<u8> stream(test_allocator());
    {
        CookedBlock block(test_allocator());
        CY_REQUIRE(block.add_column(reflect::TypeId(9201), sizeof(Transform)).has_value());
        block.set_row_count(kRows);
        block.set_payload(payload.span());

        CookedWriter writer(stream);
        CY_REQUIRE(writer.begin_stream(0x1234'5678'9ABC'DEF0ULL).has_value());
        CY_REQUIRE(writer.write_block(block).has_value());
        CY_REQUIRE(writer.end_stream().has_value());
    }

    CookedReader reader(stream.data(), stream.size());
    CY_REQUIRE(reader.read_header(0x1234'5678'9ABC'DEF0ULL).has_value());
    CY_REQUIRE_EQ(reader.block_count(), 1U);

    CookedBlock block(test_allocator());
    CY_REQUIRE(reader.next_block(block).has_value());
    CY_CHECK_EQ(block.row_count(), kRows);
    CY_REQUIRE_EQ(block.columns().size(), 1U);
    CY_CHECK_EQ(block.columns()[0].element_size, sizeof(Transform));

    const Expected<Span<const u8>, Error> bytes = block.column_bytes(0);
    CY_REQUIRE(bytes.has_value());
    CY_REQUIRE_EQ(bytes->size(), payload.size());
    for (usize index = 0; index < payload.size(); ++index) {
        CY_REQUIRE_EQ((*bytes)[index], payload[index]);
    }
}

CY_TEST_CASE(
    "the reference sites survive the round trip, which is what makes fixup a strided pass") {
    constexpr u32 kRows = 8;
    Array<u8> payload(test_allocator());
    CY_REQUIRE(payload.resize(kRows * sizeof(Target)).has_value());

    Array<u8> stream(test_allocator());
    {
        CookedBlock block(test_allocator());
        CY_REQUIRE(block.add_column(reflect::TypeId(9202), sizeof(Target)).has_value());
        CY_REQUIRE(
            block.add_reference_site(0, static_cast<u32>(offsetof(Target, entity))).has_value());
        block.set_row_count(kRows);
        block.set_payload(payload.span());

        CookedWriter writer(stream);
        CY_REQUIRE(writer.begin_stream(1).has_value());
        CY_REQUIRE(writer.write_block(block).has_value());
        CY_REQUIRE(writer.end_stream().has_value());
    }

    CookedReader reader(stream.data(), stream.size());
    CY_REQUIRE(reader.read_header(1).has_value());
    CookedBlock block(test_allocator());
    CY_REQUIRE(reader.next_block(block).has_value());
    CY_REQUIRE_EQ(block.reference_sites().size(), 1U);
    CY_CHECK_EQ(block.reference_sites()[0].column, 0U);
    CY_CHECK_EQ(block.reference_sites()[0].offset, 0U);
}

CY_TEST_CASE("a reference site that does not fit its column is refused when it is declared") {
    CookedBlock block(test_allocator());
    CY_REQUIRE(block.add_column(reflect::TypeId(9202), sizeof(Target)).has_value());
    // `Target` is sixteen bytes once padded, so a reference at offset twelve runs four bytes past
    // its end. The check is what stops a cook from emitting a site that would write over the next
    // row at activation.
    const Status refused = block.add_reference_site(0, 12);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::OutOfRange);
    CY_CHECK_FALSE(block.add_reference_site(1, 0).has_value());
}

CY_TEST_CASE("cooked data produced against another schema is fatal at the header") {
    Array<u8> stream(test_allocator());
    {
        CookedWriter writer(stream);
        CY_REQUIRE(writer.begin_stream(schema_of(health_type(), 1)).has_value());
        CY_REQUIRE(writer.end_stream().has_value());
    }

    CookedReader reader(stream.data(), stream.size());
    const Status header = reader.read_header(schema_of(health_type(), 2));
    CY_REQUIRE_FALSE(header.has_value());
    CY_CHECK_EQ(header.error().code, ErrorCode::Unsupported);
}

CY_TEST_CASE("the build schema identity changes when the layout does and not otherwise") {
    const u64 first = schema_of(health_type(), 1);
    const u64 again = schema_of(health_type(), 1);
    CY_CHECK_EQ(first, again);
    CY_CHECK_NE(first, schema_of(health_type(), 2));
    CY_CHECK_NE(first, schema_of(spread_type(), 1));
}

CY_TEST_CASE("the digest does not depend on the order types are visited in") {
    // The cooker walks the types a scene uses; the runtime walks its registry. Neither can be made
    // to visit in the other's order, so the digest has to be order-independent or the check would
    // fire on data that is perfectly good.
    BuildSchemaDigest forward;
    forward.add_type(health_type(), 1);
    forward.add_type(spread_type(), 4);

    BuildSchemaDigest backward;
    backward.add_type(spread_type(), 4);
    backward.add_type(health_type(), 1);

    CY_CHECK_EQ(forward.value(), backward.value());
    CY_CHECK_EQ(forward.type_count(), 2U);
}

CY_TEST_CASE("a payload that does not match the declared columns is refused") {
    Array<u8> payload(test_allocator());
    CY_REQUIRE(payload.resize(7).has_value());

    CookedBlock block(test_allocator());
    CY_REQUIRE(block.add_column(reflect::TypeId(9201), sizeof(Transform)).has_value());
    block.set_row_count(4);
    block.set_payload(payload.span());

    Array<u8> stream(test_allocator());
    CookedWriter writer(stream);
    CY_REQUIRE(writer.begin_stream(1).has_value());
    const Status written = writer.write_block(block);
    CY_REQUIRE_FALSE(written.has_value());
    CY_CHECK_EQ(written.error().code, ErrorCode::InvalidArgument);
}
