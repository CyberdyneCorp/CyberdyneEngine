// M9 TASK 4.3 — COMPILED SCHEMAS, AND THE COOK ERRORS THAT ARE NOT ROUNDINGS.
//
// The cases below are the specification's own scenarios: "Packed serialisation", "Schema drift is
// caught", "Position is not three floats", "Owner-only field", "Cost is visible while authoring".
//
// EACH REFUSAL IS TESTED BY MAKING THE DECLARATION WRONG IN EXACTLY ONE WAY, so the case cannot
// pass because something else refused first.

#include "fixture.h"

#include <cstring>

using namespace cy::net_test;
using cy::u32;
using cy::u64;
using cy::u8;

namespace {

[[nodiscard]] SampleTransform sample(float x, cy::u16 health) noexcept {
    SampleTransform value;
    value.position[0] = x;
    value.position[1] = 2.5F;
    value.position[2] = -3.25F;
    value.rotation[0] = 0.0F;
    value.rotation[1] = 0.0F;
    value.rotation[2] = 0.0F;
    value.rotation[3] = 1.0F;
    value.health = health;
    value.flags = 0xA5A5'0001U;
    return value;
}

}  // namespace

CY_TEST_CASE("networking: a schema compiles against reflection and keys on FieldId") {
    CompiledSchema compiled(allocator());
    cy::Expected<u32, SchemaRejection> result = compiled.compile(sample_type(), sample_schema());
    CY_REQUIRE(result.has_value());
    CY_CHECK_EQ(result.value(), 4U);
    CY_CHECK_EQ(compiled.field_count(), 4U);
    CY_CHECK_EQ(compiled.instance_size(), static_cast<u32>(sizeof(SampleTransform)));

    // "Position is not three floats": 18 bits per component over a 2 km range is 1 cm precision,
    // and 54 bits rather than 96.
    CY_CHECK_EQ(compiled.field(0).bits, 54U);
    CY_CHECK_LT(compiled.field(0).bits, 96U);
    // Smallest-three: two bits of selector and three components of ten.
    CY_CHECK_EQ(compiled.field(1).bits, 32U);
    CY_CHECK_EQ(compiled.field(2).bits, 10U);
    CY_CHECK_EQ(compiled.field(3).bits, 32U);
    // "Cost is visible while authoring": the mask plus every field.
    CY_CHECK_EQ(compiled.theoretical_bits(), 4U + 54U + 32U + 10U + 32U);
}

CY_TEST_CASE("networking: a removed field is drift and a renamed one is not") {
    // Renaming is invisible here on purpose: the schema names `kFieldHealth` and the reflected
    // field's `name` is metadata. The only way to make this fail is to remove the identifier.
    static FieldSchema fields[1];
    fields[0].field = cy::reflect::FieldId(999'999);
    fields[0].encoder = FieldEncoder::Raw;
    ReplicationSchema drifted;
    drifted.component = "SampleTransform";
    drifted.fields = cy::Span<const FieldSchema>(fields, 1);

    CompiledSchema compiled(allocator());
    cy::Expected<u32, SchemaRejection> result = compiled.compile(sample_type(), drifted);
    CY_REQUIRE_FALSE(result.has_value());
    CY_CHECK(result.error().error == SchemaError::FieldRemoved);
    CY_CHECK_EQ(result.error().field_id.value(), 999'999U);
}

CY_TEST_CASE("networking: a range that cannot represent the field's bounds is a cook error") {
    static FieldSchema fields[1];
    fields[0].field = cy::reflect::FieldId(kFieldPosition);
    fields[0].encoder = FieldEncoder::QuantisedVector;
    // The field declares [-1000, 1000]; this covers half of it. Clamping would be the silent wrong
    // answer — a position that stops moving at the edge of the arena.
    fields[0].parameters = EncoderParameters{-500.0, 500.0, 18};
    ReplicationSchema narrow;
    narrow.component = "SampleTransform";
    narrow.fields = cy::Span<const FieldSchema>(fields, 1);

    CompiledSchema compiled(allocator());
    cy::Expected<u32, SchemaRejection> result = compiled.compile(sample_type(), narrow);
    CY_REQUIRE_FALSE(result.has_value());
    CY_CHECK(result.error().error == SchemaError::RangeCannotRepresent);

    // And the same declaration with an interval that does cover it compiles.
    fields[0].parameters = EncoderParameters{-1000.0, 1000.0, 18};
    CompiledSchema wide(allocator());
    CY_CHECK(wide.compile(sample_type(), narrow).has_value());
}

CY_TEST_CASE("networking: an encoder that does not apply, and a width that does not fit") {
    static FieldSchema fields[1];
    ReplicationSchema schema;
    schema.component = "SampleTransform";
    schema.fields = cy::Span<const FieldSchema>(fields, 1);

    fields[0].field = cy::reflect::FieldId(kFieldHealth);
    fields[0].encoder = FieldEncoder::QuantisedScalar;
    fields[0].parameters = EncoderParameters{0.0, 100.0, 8};
    CompiledSchema wrong_kind(allocator());
    cy::Expected<u32, SchemaRejection> kind = wrong_kind.compile(sample_type(), schema);
    CY_REQUIRE_FALSE(kind.has_value());
    CY_CHECK(kind.error().error == SchemaError::EncoderDoesNotApply);

    fields[0].encoder = FieldEncoder::Bitfield;
    fields[0].parameters = EncoderParameters{0.0, 0.0, 24};  // health is sixteen bits wide
    CompiledSchema too_wide(allocator());
    cy::Expected<u32, SchemaRejection> bits = too_wide.compile(sample_type(), schema);
    CY_REQUIRE_FALSE(bits.has_value());
    CY_CHECK(bits.error().error == SchemaError::BitsOutOfRange);
}

CY_TEST_CASE("networking: a delta round-trips and carries only what changed") {
    CompiledSchema compiled(allocator());
    CY_REQUIRE(compiled.compile(sample_type(), sample_schema()).has_value());

    const SampleTransform baseline = sample(1.0F, 100);
    SampleTransform current = baseline;
    current.health = 87;

    // Owner-only `flags` is excluded for a non-owner, so a change to health alone is one field.
    const ChangeMask mask = compiled.changes(&current, &baseline, false);
    CY_CHECK_EQ(mask, ChangeMask{1} << 2);
    CY_CHECK_EQ(compiled.bits_for(mask), 4U + 10U);

    cy::Array<u8> bytes(allocator());
    BitWriter writer(bytes);
    cy::Expected<u32, cy::Error> spent = compiled.encode_instance(&current, mask, writer);
    CY_REQUIRE(spent.has_value());
    CY_CHECK_EQ(spent.value(), 14U);
    CY_REQUIRE(writer.flush().has_value());
    CY_CHECK_EQ(bytes.size(), cy::usize{2});

    SampleTransform decoded = baseline;
    BitReader reader(bytes.span());
    ChangeMask read_mask = 0;
    CY_REQUIRE(compiled.decode_instance(reader, &decoded, read_mask).has_value());
    CY_CHECK_EQ(read_mask, mask);
    CY_CHECK_EQ(decoded.health, cy::u16{87});
    // The absent fields were left exactly as they were — which is what makes a delta a delta.
    CY_CHECK_EQ(decoded.flags, baseline.flags);
    CY_CHECK_EQ(decoded.position[0], baseline.position[0]);
}

CY_TEST_CASE("networking: quantisation holds its declared precision, and a quaternion survives") {
    CompiledSchema compiled(allocator());
    CY_REQUIRE(compiled.compile(sample_type(), sample_schema()).has_value());

    SampleTransform value = sample(123.456F, 42);
    // A rotation that is not the identity, so a decoder that ignored the payload would fail.
    value.rotation[0] = 0.5F;
    value.rotation[1] = 0.5F;
    value.rotation[2] = 0.5F;
    value.rotation[3] = 0.5F;

    cy::Array<u8> bytes(allocator());
    BitWriter writer(bytes);
    const ChangeMask all = ~ChangeMask{0} >> (64 - 4);
    CY_REQUIRE(compiled.encode_instance(&value, all, writer).has_value());
    CY_REQUIRE(writer.flush().has_value());

    SampleTransform decoded;
    BitReader reader(bytes.span());
    ChangeMask mask = 0;
    CY_REQUIRE(compiled.decode_instance(reader, &decoded, mask).has_value());

    // 2000 metres over 2^18 - 1 steps is 7.6 mm; the declared precision was one centimetre.
    CY_CHECK_NEAR(decoded.position[0], value.position[0], 0.01F);
    CY_CHECK_NEAR(decoded.position[1], value.position[1], 0.01F);
    CY_CHECK_NEAR(decoded.position[2], value.position[2], 0.01F);
    for (u32 index = 0; index < 4; ++index) {
        CY_CHECK_NEAR(decoded.rotation[index], value.rotation[index], 0.01F);
    }
    CY_CHECK_EQ(decoded.health, cy::u16{42});
    CY_CHECK_EQ(decoded.flags, value.flags);
}

CY_TEST_CASE("networking: an owner-only field reaches the owner and nobody else") {
    CompiledSchema compiled(allocator());
    CY_REQUIRE(compiled.compile(sample_type(), sample_schema()).has_value());

    const SampleTransform baseline = sample(1.0F, 100);
    SampleTransform current = baseline;
    current.flags = 0xDEAD'BEEFU;

    CY_CHECK_EQ(compiled.changes(&current, &baseline, false), ChangeMask{0});
    CY_CHECK_EQ(compiled.changes(&current, &baseline, true), ChangeMask{1} << 3);
}

CY_TEST_CASE("networking: the schema set's identity is what a peer is verified against") {
    SchemaSet set(allocator());
    CY_CHECK_EQ(set.identity(), kEmptySchemaSetHash);
    CY_REQUIRE(set.add(sample_type(), sample_schema()).has_value());
    const u64 identity = set.identity();
    CY_CHECK_NE(identity, kEmptySchemaSetHash);
    CY_CHECK_NE(identity, u64{0});

    // A second build that declared the same schema at a different width means something different
    // on the wire, and the identity says so.
    static FieldSchema fields[4];
    const ReplicationSchema original = sample_schema();
    std::memcpy(static_cast<void*>(fields), static_cast<const void*>(original.fields.data()),
                sizeof(fields));
    fields[0].parameters.bits = 16;
    ReplicationSchema coarser = original;
    coarser.fields = cy::Span<const FieldSchema>(fields, 4);

    SchemaSet other(allocator());
    CY_REQUIRE(other.add(sample_type(), coarser).has_value());
    CY_CHECK_NE(other.identity(), identity);
}
