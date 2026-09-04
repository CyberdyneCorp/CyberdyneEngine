// Schema versions and value-level migration, including the override targets that travel with them.

#include <cy/core/serialize/migration.h>
#include <cy/test/test.h>

#include "fixtures.h"

using namespace cy;
using namespace cy::serialize;
using namespace cy::serialize::test;

namespace {

constexpr u32 kOldHealth = 1;      ///< The single `health` field of version 1.
constexpr u32 kCurrentHealth = 2;  ///< `currentHealth` after the split.
constexpr u32 kMaxHealth = 3;      ///< `maxHealth` after the split.

/// The specification's worked example: one field becomes two, written against the value record with
/// no version-1 type existing anywhere in this translation unit or any other.
Status split_health(ValueRecord& record, void* context) noexcept {
    (void)context;
    const FieldValue* old = record.find(reflect::FieldId(kOldHealth));
    if (old == nullptr) {
        return ok();  // Absent means it took its default; there is nothing to split.
    }
    u32 value = 0;
    const Span<const u8> bytes = record.bytes(*old);
    if (Status decoded =
            decode_scalar(WireType::U32, bytes.data(), static_cast<u32>(bytes.size()), &value);
        !decoded) {
        return decoded;
    }
    if (Status written = record.set_scalar(reflect::FieldId(kCurrentHealth), WireType::U32, &value,
                                           sizeof(value));
        !written) {
        return written;
    }
    if (Status written =
            record.set_scalar(reflect::FieldId(kMaxHealth), WireType::U32, &value, sizeof(value));
        !written) {
        return written;
    }
    (void)record.remove(reflect::FieldId(kOldHealth));
    return ok();
}

}  // namespace

CY_TEST_CASE("a record at the current version is left alone") {
    SchemaRegistry schemas(test_allocator());
    CY_REQUIRE(schemas.declare(reflect::TypeId(kHealthTypeId), 3).has_value());

    ValueRecord record(test_allocator());
    record.set_type(reflect::TypeId(kHealthTypeId));
    record.set_schema_version(3);
    CY_REQUIRE(schemas.migrate(record).has_value());
    CY_CHECK_EQ(record.schema_version(), 3U);
}

CY_TEST_CASE("a chain is walked one step at a time and ends at the current version") {
    SchemaRegistry schemas(test_allocator());
    CY_REQUIRE(schemas.declare(reflect::TypeId(kHealthTypeId), 3).has_value());

    const FieldRemap first[] = {{reflect::FieldId(10), reflect::FieldId(20)}};
    const FieldRemap second[] = {{reflect::FieldId(20), reflect::FieldId(30)}};
    CY_REQUIRE(schemas
                   .add_migration(Migration{reflect::TypeId(kHealthTypeId), 1, 2,
                                            MigrationClass::Automatic, "ten to twenty", nullptr,
                                            Span<const FieldRemap>(first, 1)})
                   .has_value());
    CY_REQUIRE(schemas
                   .add_migration(Migration{reflect::TypeId(kHealthTypeId), 2, 3,
                                            MigrationClass::Automatic, "twenty to thirty", nullptr,
                                            Span<const FieldRemap>(second, 1)})
                   .has_value());

    ValueRecord record(test_allocator());
    record.set_type(reflect::TypeId(kHealthTypeId));
    record.set_schema_version(1);
    const u32 value = 88;
    CY_REQUIRE(record.set_scalar(reflect::FieldId(10), WireType::U32, &value, 4).has_value());

    CY_REQUIRE(schemas.migrate(record).has_value());
    CY_CHECK_EQ(record.schema_version(), 3U);
    CY_CHECK_FALSE(record.contains(reflect::FieldId(10)));
    CY_CHECK(record.contains(reflect::FieldId(30)));
}

CY_TEST_CASE("splitting a field is written against the value record and needs no old type") {
    SchemaRegistry schemas(test_allocator());
    CY_REQUIRE(schemas.declare(reflect::TypeId(kHealthTypeId), 2).has_value());
    CY_REQUIRE(schemas
                   .add_migration(Migration{reflect::TypeId(kHealthTypeId),
                                            1,
                                            2,
                                            MigrationClass::Custom,
                                            "split health",
                                            &split_health,
                                            {}})
                   .has_value());

    ValueRecord record(test_allocator());
    record.set_type(reflect::TypeId(kHealthTypeId));
    record.set_schema_version(1);
    const u32 health = 75;
    CY_REQUIRE(
        record.set_scalar(reflect::FieldId(kOldHealth), WireType::U32, &health, 4).has_value());

    CY_REQUIRE(schemas.migrate(record).has_value());
    CY_CHECK_EQ(record.schema_version(), 2U);
    CY_CHECK_FALSE(record.contains(reflect::FieldId(kOldHealth)));

    u32 current = 0;
    u32 maximum = 0;
    CY_REQUIRE(decode_scalar(WireType::U32, record.bytes(reflect::FieldId(kCurrentHealth)).data(),
                             4, &current)
                   .has_value());
    CY_REQUIRE(
        decode_scalar(WireType::U32, record.bytes(reflect::FieldId(kMaxHealth)).data(), 4, &maximum)
            .has_value());
    CY_CHECK_EQ(current, 75U);
    CY_CHECK_EQ(maximum, 75U);
}

CY_TEST_CASE("data older than the oldest migration fails, naming what it could not do") {
    SchemaRegistry schemas(test_allocator());
    CY_REQUIRE(schemas.declare(reflect::TypeId(kHealthTypeId), 3).has_value());
    const FieldRemap remap[] = {{reflect::FieldId(20), reflect::FieldId(30)}};
    CY_REQUIRE(schemas
                   .add_migration(Migration{reflect::TypeId(kHealthTypeId), 2, 3,
                                            MigrationClass::Automatic, "two to three", nullptr,
                                            Span<const FieldRemap>(remap, 1)})
                   .has_value());

    ValueRecord record(test_allocator());
    record.set_type(reflect::TypeId(kHealthTypeId));
    record.set_schema_version(1);

    const Status migrated = schemas.migrate(record);
    CY_REQUIRE_FALSE(migrated.has_value());
    CY_CHECK_EQ(migrated.error().code, ErrorCode::NotFound);
}

CY_TEST_CASE("data newer than this build fails rather than being misread") {
    SchemaRegistry schemas(test_allocator());
    CY_REQUIRE(schemas.declare(reflect::TypeId(kHealthTypeId), 2).has_value());

    ValueRecord record(test_allocator());
    record.set_type(reflect::TypeId(kHealthTypeId));
    record.set_schema_version(5);

    const Status migrated = schemas.migrate(record);
    CY_REQUIRE_FALSE(migrated.has_value());
    CY_CHECK_EQ(migrated.error().code, ErrorCode::Unsupported);
}

CY_TEST_CASE("a record of an undeclared type passes through untouched") {
    SchemaRegistry schemas(test_allocator());
    ValueRecord record(test_allocator());
    record.set_type(reflect::TypeId(777001));
    record.set_schema_version(9);
    const u32 value = 1;
    CY_REQUIRE(record.set_scalar(reflect::FieldId(1), WireType::U32, &value, 4).has_value());

    CY_REQUIRE(schemas.migrate(record).has_value());
    CY_CHECK_EQ(record.schema_version(), 9U);
    CY_CHECK(record.contains(reflect::FieldId(1)));
}

CY_TEST_CASE("an override's target is migrated by the same chain as its data") {
    // "A migration that updates an asset but drops the overrides on it SHALL be a defect."
    SchemaRegistry schemas(test_allocator());
    CY_REQUIRE(schemas.declare(reflect::TypeId(kHealthTypeId), 3).has_value());
    const FieldRemap first[] = {{reflect::FieldId(10), reflect::FieldId(20)}};
    const FieldRemap second[] = {{reflect::FieldId(20), reflect::FieldId(30)}};
    CY_REQUIRE(schemas
                   .add_migration(Migration{reflect::TypeId(kHealthTypeId), 1, 2,
                                            MigrationClass::Automatic, "ten to twenty", nullptr,
                                            Span<const FieldRemap>(first, 1)})
                   .has_value());
    CY_REQUIRE(schemas
                   .add_migration(Migration{reflect::TypeId(kHealthTypeId), 2, 3,
                                            MigrationClass::Automatic, "twenty to thirty", nullptr,
                                            Span<const FieldRemap>(second, 1)})
                   .has_value());

    reflect::FieldId target(10);
    CY_REQUIRE(schemas.migrate_field_id(reflect::TypeId(kHealthTypeId), 1, target).has_value());
    CY_CHECK_EQ(target.value(), 30U);

    // A target the chain does not mention is carried through unchanged rather than lost.
    reflect::FieldId untouched(99);
    CY_REQUIRE(schemas.migrate_field_id(reflect::TypeId(kHealthTypeId), 1, untouched).has_value());
    CY_CHECK_EQ(untouched.value(), 99U);
}

CY_TEST_CASE("a chain with a gap is refused at registration, not discovered at load") {
    SchemaRegistry schemas(test_allocator());
    CY_REQUIRE(schemas.declare(reflect::TypeId(kHealthTypeId), 4).has_value());
    const Status skipped = schemas.add_migration(Migration{reflect::TypeId(kHealthTypeId),
                                                           1,
                                                           3,
                                                           MigrationClass::Automatic,
                                                           "one to three",
                                                           nullptr,
                                                           {}});
    CY_REQUIRE_FALSE(skipped.has_value());
    CY_CHECK_EQ(skipped.error().code, ErrorCode::InvalidArgument);
}

CY_TEST_CASE("two answers to what version is current is refused") {
    SchemaRegistry schemas(test_allocator());
    CY_REQUIRE(schemas.declare(reflect::TypeId(kHealthTypeId), 2).has_value());
    CY_REQUIRE(schemas.declare(reflect::TypeId(kHealthTypeId), 2).has_value());
    const Status conflicting = schemas.declare(reflect::TypeId(kHealthTypeId), 3);
    CY_REQUIRE_FALSE(conflicting.has_value());
    CY_CHECK_EQ(conflicting.error().code, ErrorCode::AlreadyExists);
}

CY_TEST_CASE("renaming a field needs no migration at all") {
    // The specification's own scenario: identity is unchanged, so nothing is registered and nothing
    // runs. The record still decodes, and its version does not move.
    SchemaRegistry schemas(test_allocator());
    CY_REQUIRE(schemas.declare(reflect::TypeId(kHealthTypeId), 1).has_value());

    ValueRecord record(test_allocator());
    record.set_type(reflect::TypeId(kHealthTypeId));
    record.set_schema_version(1);
    const f32 maximum = 10.0F;
    CY_REQUIRE(record.set_scalar(reflect::FieldId(kHealthMaximum), WireType::F32, &maximum, 4)
                   .has_value());

    CY_REQUIRE(schemas.migrate(record).has_value());
    CY_CHECK_EQ(schemas.migration_count(), 0U);
    CY_CHECK(record.contains(reflect::FieldId(kHealthMaximum)));
}
