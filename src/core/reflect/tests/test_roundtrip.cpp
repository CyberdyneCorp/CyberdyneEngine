// Round-trip goldens over reflected data. Task 1.2.5.
//
// The goldens are committed under golden/. Each one is hexadecimal text rather than binary so that
// a change to the encoding is a readable diff, and each carries a comment saying what it is for.
//
// What the three of them together assert is the identity claim, not the encoding:
//
//   health_canonical.hex   what the writer emits today, byte for byte.
//   health_reordered.hex   the same values in a different field order, plus a field this build has
//                          never declared. It loads to the same values, because a record addresses
//                          fields by FieldId and never by offset or by name.
//   unknown_type.hex       a record whose TypeId is not registered. It survives a load and a save.

#include <cy/core/reflect/demo/types.reflect.h>
#include <cy/core/reflect/reflect.h>
#include <cy/test/test.h>

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace {

/// Read one committed golden. A `#` begins a comment; everything else is hexadecimal.
std::vector<cy::u8> golden(const char* name) {
    std::string path = std::string(CY_REFLECT_GOLDEN_DIR) + "/" + name;
    std::FILE* handle = std::fopen(path.c_str(), "rb");
    CY_REQUIRE(handle != nullptr);  // CY_REFLECT_GOLDEN_DIR comes from cy_add_test

    std::vector<cy::u8> bytes;
    int high = -1;
    bool in_comment = false;
    for (int character = std::fgetc(handle); character != EOF; character = std::fgetc(handle)) {
        if (character == '#') {
            in_comment = true;
        } else if (character == '\n') {
            in_comment = false;
        } else if (!in_comment && std::isxdigit(character) != 0) {
            const int digit =
                (character <= '9') ? character - '0' : (std::tolower(character) - 'a' + 10);
            if (high < 0) {
                high = digit;
            } else {
                bytes.push_back(static_cast<cy::u8>((high << 4) | digit));
                high = -1;
            }
        }
    }
    std::fclose(handle);
    CY_CHECK_EQ(high, -1);  // an odd number of digits is a corrupt golden
    return bytes;
}

cy::demo::Health sample() {
    cy::demo::Health health;
    health.maximum = 250.0F;
    health.current = 137.5F;
    health.displayed = 7.25F;  // Transient: it must not reach the record
    health.last_damage = 2;
    health.icon = 0x0123456789ABCDEFULL;
    return health;
}

}  // namespace

CY_TEST_CASE("a written record matches the committed golden byte for byte") {
    const auto& health = cy::reflect::type_of<cy::demo::Health>();
    const cy::demo::Health written = sample();

    cy::reflect::ByteBuffer buffer;
    CY_REQUIRE(cy::reflect::write_record(health, &written, buffer).has_value());

    const std::vector<cy::u8> expected = golden("health_canonical.hex");
    CY_REQUIRE_EQ(buffer.size(), expected.size());
    for (cy::usize index = 0; index < expected.size(); ++index) {
        CY_CHECK_EQ(buffer.data()[index], expected[index]);
    }
}

CY_TEST_CASE("a value survives a round trip unchanged") {
    const auto& health = cy::reflect::type_of<cy::demo::Health>();
    const cy::demo::Health written = sample();

    cy::reflect::ByteBuffer buffer;
    CY_REQUIRE(cy::reflect::write_record(health, &written, buffer).has_value());

    cy::demo::Health read;
    read.displayed = 99.0F;
    CY_REQUIRE(cy::reflect::read_record(health, buffer.data(), buffer.size(), &read).has_value());

    CY_CHECK_EQ(read.maximum, written.maximum);
    CY_CHECK_EQ(read.current, written.current);
    CY_CHECK_EQ(read.last_damage, written.last_damage);
    CY_CHECK_EQ(read.icon, written.icon);

    // The transient field was neither written nor overwritten.
    CY_CHECK_EQ(read.displayed, 99.0F);
}

CY_TEST_CASE("a record written with another field order loads identically") {
    const auto& health = cy::reflect::type_of<cy::demo::Health>();
    const std::vector<cy::u8> bytes = golden("health_reordered.hex");

    cy::demo::Health read;
    CY_REQUIRE(cy::reflect::read_record(health, bytes.data(), bytes.size(), &read).has_value());

    CY_CHECK_EQ(read.maximum, 250.0F);
    CY_CHECK_EQ(read.current, 137.5F);
    CY_CHECK_EQ(read.last_damage, 2);
    CY_CHECK_EQ(read.icon, 0x0123456789ABCDEFULL);
}

CY_TEST_CASE("a record for an unregistered type is preserved rather than destroyed") {
    const std::vector<cy::u8> bytes = golden("unknown_type.hex");
    cy::reflect::TypeRegistry registry;
    CY_REQUIRE(cy::reflect::register_generated_types(registry).has_value());

    const auto header = cy::reflect::peek_record(bytes.data(), bytes.size());
    CY_REQUIRE(header.has_value());
    CY_CHECK_EQ(header->total_size(), bytes.size());
    CY_CHECK(registry.find(header->type) == nullptr);  // nothing in this build declares it

    cy::reflect::ByteBuffer resaved;
    CY_REQUIRE(cy::reflect::write_opaque(bytes.data(), bytes.size(), resaved).has_value());
    CY_REQUIRE_EQ(resaved.size(), bytes.size());
    for (cy::usize index = 0; index < bytes.size(); ++index) {
        CY_CHECK_EQ(resaved.data()[index], bytes[index]);
    }
}

CY_TEST_CASE("a record written for one type is refused by another") {
    const auto& placement = cy::reflect::type_of<cy::demo::Placement>();
    const std::vector<cy::u8> bytes = golden("health_canonical.hex");

    cy::demo::Placement read;
    const auto applied = cy::reflect::read_record(placement, bytes.data(), bytes.size(), &read);
    CY_REQUIRE_FALSE(applied.has_value());
    CY_CHECK_EQ(applied.error().code, cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("a truncated record is an error and not a partial write") {
    const auto& health = cy::reflect::type_of<cy::demo::Health>();
    const std::vector<cy::u8> bytes = golden("health_canonical.hex");

    cy::demo::Health read;
    const auto applied = cy::reflect::read_record(health, bytes.data(), bytes.size() - 4, &read);
    CY_REQUIRE_FALSE(applied.has_value());
    CY_CHECK_EQ(applied.error().code, cy::ErrorCode::BufferTooSmall);
}

CY_TEST_CASE("every reflected type round-trips its own defaults") {
    cy::reflect::TypeRegistry registry;
    CY_REQUIRE(cy::reflect::register_generated_types(registry).has_value());

    for (const cy::reflect::TypeInfo* type : registry) {
        std::vector<cy::u8> storage(type->size);
        std::vector<cy::u8> restored(type->size);
        type->construct(storage.data());
        type->construct(restored.data());

        cy::reflect::ByteBuffer buffer;
        CY_REQUIRE(cy::reflect::write_record(*type, storage.data(), buffer).has_value());
        CY_REQUIRE(cy::reflect::read_record(*type, buffer.data(), buffer.size(), restored.data())
                       .has_value());

        for (cy::u32 index = 0; index < type->field_count; ++index) {
            const cy::reflect::FieldInfo& field = type->fields[index];
            for (cy::u32 byte = 0; byte < field.size; ++byte) {
                CY_CHECK_EQ(storage[field.offset + byte], restored[field.offset + byte]);
            }
        }
        type->destruct(storage.data());
        type->destruct(restored.data());
    }
}
