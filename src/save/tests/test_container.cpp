// The container: round trips, preservation, migration, and the diagnostics of a bad save.
// Tasks 6.2 and 6.3.

#include <cy/save/container.h>
#include <cy/test/test.h>

#include "fixtures.h"

#include <cstring>
#include <string_view>

using namespace cy;
using namespace cy::save;
using namespace cy::save::test;

namespace {

constexpr RegionKey kVillage{0x0A00'0000'0000'0001ULL};

/// The schema registry the load path is given. Health is at version 2 with one migration from 1.
constexpr u32 kOldRevives = 6;  ///< The identifier version 1 wrote `revives` under.

Status rename_revives(serialize::ValueRecord& record, void* context) noexcept {
    (void)context;
    return record.retarget(reflect::FieldId(kOldRevives), reflect::FieldId(kHealthRevives));
}

Status build_schemas(serialize::SchemaRegistry& schemas) noexcept {
    if (Status declared = schemas.declare(reflect::TypeId(kHealthTypeId), 2); !declared) {
        return declared;
    }
    serialize::Migration step;
    step.type = reflect::TypeId(kHealthTypeId);
    step.from_version = 1;
    step.to_version = 2;
    step.kind = serialize::MigrationClass::Automatic;
    step.name = "health.revives-renamed";
    step.apply = rename_revives;
    return schemas.add_migration(step);
}

u32 revives_of(const Overlay& overlay, PersistentId id) noexcept {
    const ComponentDelta* delta =
        overlay.find_component(kVillage, id, reflect::TypeId(kHealthTypeId));
    if (delta == nullptr) {
        return 0xFFFF'FFFFU;
    }
    u32 value = 0;
    const Span<const u8> bytes = delta->record.bytes(reflect::FieldId(kHealthRevives));
    if (!serialize::decode_scalar(serialize::WireType::U32, bytes.data(),
                                  static_cast<u32>(bytes.size()), &value)) {
        return 0xFFFF'FFFEU;
    }
    return value;
}

void record_health(Overlay& overlay, PersistentId id, u32 revives) {
    Health health;
    health.revives = revives;
    CY_REQUIRE(overlay.record_component(kVillage, id, health_type(), &health, 2).has_value());
}

}  // namespace

CY_TEST_CASE("a region round-trips through a chunk") {
    Overlay written(test_allocator());
    record_health(written, entity(1), 3);
    record_health(written, entity(2), 7);
    CY_REQUIRE(written.destroy_entity(kVillage, entity(3)).has_value());
    CY_REQUIRE(written.create_entity(kVillage, entity(4), AssetId(5, 6), entity(1)).has_value());

    Array<u8> bytes(test_allocator());
    CY_REQUIRE(encode_region(written, kVillage, bytes).has_value());

    Overlay read(test_allocator());
    LoadPolicy policy;
    LoadReport report;
    CY_REQUIRE(decode_chunk(bytes.span(), policy, read, report).has_value());
    CY_CHECK_FALSE(report.failed());
    CY_CHECK_EQ(report.chunks_read, 1U);
    CY_CHECK_EQ(report.entries_applied, 4U);

    CY_CHECK_EQ(read.entry_count(), 4U);
    CY_CHECK_EQ(revives_of(read, entity(1)), 3U);
    CY_CHECK_EQ(revives_of(read, entity(2)), 7U);
    CY_CHECK_EQ(read.find_entry(kVillage, entity(3))->kind, EntryKind::Tombstone);
    const Entry* created = read.find_entry(kVillage, entity(4));
    CY_REQUIRE(created != nullptr);
    CY_CHECK_EQ(created->kind, EntryKind::Created);
    CY_CHECK(created->template_asset == AssetId(5, 6));
    CY_CHECK(created->owner == entity(1));
}

CY_TEST_CASE("an encoded region is byte-identical on a second write") {
    Overlay overlay(test_allocator());
    record_health(overlay, entity(1), 3);

    Array<u8> first(test_allocator());
    Array<u8> second(test_allocator());
    CY_REQUIRE(encode_region(overlay, kVillage, first).has_value());
    CY_REQUIRE(encode_region(overlay, kVillage, second).has_value());
    CY_REQUIRE_EQ(first.size(), second.size());
    CY_CHECK_EQ(std::memcmp(first.data(), second.data(), first.size()), 0);
}

CY_TEST_CASE("a scope fragment round-trips in its own chunk") {
    Overlay written(test_allocator());
    Settings settings;
    settings.gamma = 2.2F;
    CY_REQUIRE(written.record_fragment(Scope::Profile, settings_type(), &settings, 1).has_value());

    Array<u8> bytes(test_allocator());
    CY_REQUIRE(encode_fragments(written, Scope::Profile, bytes).has_value());

    Overlay read(test_allocator());
    LoadPolicy policy;
    LoadReport report;
    CY_REQUIRE(decode_chunk(bytes.span(), policy, read, report).has_value());
    CY_CHECK(read.find_fragment(Scope::Profile, reflect::TypeId(kSettingsTypeId)) != nullptr);
    CY_CHECK_EQ(read.entry_count(), 0U);
}

CY_TEST_CASE("a record of a type this build does not know is carried through, not stripped") {
    // "Unknown data is preserved": an editor or a build without a plugin does not silently strip
    // that plugin's data from every file it touches.
    Overlay written(test_allocator());
    serialize::ValueRecord plugin(test_allocator());
    plugin.set_type(reflect::TypeId(7777));
    plugin.set_schema_version(4);
    const u64 payload = 0xDEAD'BEEF'0000'0001ULL;
    CY_REQUIRE(
        plugin.set_scalar(reflect::FieldId(11), serialize::WireType::U64, &payload, sizeof(payload))
            .has_value());
    CY_REQUIRE(written.record_component(kVillage, entity(1), reflect::TypeId(7777), 4, plugin)
                   .has_value());

    Array<u8> bytes(test_allocator());
    CY_REQUIRE(encode_region(written, kVillage, bytes).has_value());

    serialize::SchemaRegistry schemas(test_allocator());
    CY_REQUIRE(build_schemas(schemas).has_value());
    LoadPolicy policy;
    policy.schemas = &schemas;
    LoadReport report;
    Overlay read(test_allocator());
    CY_REQUIRE(decode_chunk(bytes.span(), policy, read, report).has_value());

    CY_CHECK_EQ(report.records_unknown, 1U);
    CY_CHECK_EQ(report.fields_preserved, 1U);
    const ComponentDelta* delta = read.find_component(kVillage, entity(1), reflect::TypeId(7777));
    CY_REQUIRE(delta != nullptr);
    CY_CHECK_EQ(delta->schema_version, 4U);
    u64 restored = 0;
    const Span<const u8> field = delta->record.bytes(reflect::FieldId(11));
    CY_REQUIRE(serialize::decode_scalar(serialize::WireType::U64, field.data(),
                                        static_cast<u32>(field.size()), &restored)
                   .has_value());
    CY_CHECK_EQ(restored, payload);
}

CY_TEST_CASE("a save written against an older schema migrates on the way in") {
    // "A campaign survives an update": a type's schema changes and existing saves migrate through
    // the registered chain. The migration runs on the value record, and no version-1 type exists
    // in this translation unit or any other.
    Overlay written(test_allocator());
    serialize::ValueRecord old(test_allocator());
    old.set_type(reflect::TypeId(kHealthTypeId));
    old.set_schema_version(1);
    const u32 revives = 11;
    CY_REQUIRE(old.set_scalar(reflect::FieldId(kOldRevives), serialize::WireType::U32, &revives,
                              sizeof(revives))
                   .has_value());
    CY_REQUIRE(written.record_component(kVillage, entity(1), reflect::TypeId(kHealthTypeId), 1, old)
                   .has_value());

    Array<u8> bytes(test_allocator());
    CY_REQUIRE(encode_region(written, kVillage, bytes).has_value());

    serialize::SchemaRegistry schemas(test_allocator());
    CY_REQUIRE(build_schemas(schemas).has_value());
    LoadPolicy policy;
    policy.schemas = &schemas;
    LoadReport report;
    Overlay read(test_allocator());
    CY_REQUIRE(decode_chunk(bytes.span(), policy, read, report).has_value());

    CY_CHECK_EQ(report.records_migrated, 1U);
    CY_CHECK_EQ(revives_of(read, entity(1)), 11U);
    CY_CHECK_EQ(
        read.find_component(kVillage, entity(1), reflect::TypeId(kHealthTypeId))->schema_version,
        2U);
}

CY_TEST_CASE("a manifest round-trips, and reports its size by scope") {
    Manifest written(test_allocator());
    written.generation = 7;
    std::memcpy(written.build_id, "1.2.3+abc", 10);
    written.project = AssetId(1, 2);
    written.save = AssetId(3, 4);
    written.campaign = AssetId(5, 6);
    written.simulation_point = 90'210;
    written.session_seed = 0xFEED'FACEULL;
    written.content_version = assets::content_hash("cooked", 6);
    CY_REQUIRE(written.require_plugin("weather", 3).has_value());

    ChunkRef world;
    world.scope = Scope::World;
    world.region = kVillage;
    world.hash = assets::content_hash("village", 7);
    world.size = 512;
    world.entry_count = 4;
    CY_REQUIRE(written.add_chunk(world).has_value());
    ChunkRef profile;
    profile.scope = Scope::Profile;
    profile.hash = assets::content_hash("profile", 7);
    profile.size = 64;
    profile.entry_count = 1;
    CY_REQUIRE(written.add_chunk(profile).has_value());

    Array<u8> bytes(test_allocator());
    CY_REQUIRE(encode_manifest(written, bytes).has_value());

    Manifest read(test_allocator());
    LoadReport report;
    CY_REQUIRE(decode_manifest(bytes.span(), read, report).has_value());
    CY_CHECK_EQ(read.generation, 7U);
    CY_CHECK_EQ(std::string_view(read.build_id), std::string_view("1.2.3+abc"));
    CY_CHECK(read.project == AssetId(1, 2));
    CY_CHECK_EQ(read.simulation_point, 90'210U);
    CY_CHECK_EQ(read.session_seed, 0xFEED'FACEULL);
    CY_CHECK(read.content_version == written.content_version);
    CY_REQUIRE_EQ(read.chunks.size(), 2U);
    CY_REQUIRE_EQ(read.plugins.size(), 1U);
    CY_CHECK_EQ(std::string_view(read.plugins[0].name), std::string_view("weather"));
    CY_CHECK_EQ(read.plugins[0].version, 3U);
    CY_CHECK_EQ(read.chunk_bytes_in(Scope::World), 512U);
    CY_CHECK_EQ(read.chunk_bytes_in(Scope::Profile), 64U);
    CY_CHECK_EQ(read.total_chunk_bytes(), 576U);
    // Sorted by (scope, region) on insertion, so the manifest is byte-identical for equal content.
    CY_CHECK_EQ(read.chunks[0].scope, Scope::Profile);
}

CY_TEST_CASE("an incompatible build is named rather than reported as corruption") {
    Manifest manifest(test_allocator());
    std::memcpy(manifest.build_id, "2.0.0", 6);
    LoadPolicy policy;
    policy.compatibility = Compatibility::ExactBuild;
    policy.build_id = "2.0.1";
    LoadReport report;
    CY_CHECK_FALSE(check_compatibility(manifest, policy, report).has_value());
    CY_CHECK_EQ(report.failure, LoadFailure::IncompatibleBuild);
    CY_CHECK_EQ(std::string_view(report.detail), std::string_view("2.0.0"));

    // The same save under a migratable policy loads.
    LoadReport permissive;
    policy.compatibility = Compatibility::Migratable;
    CY_CHECK(check_compatibility(manifest, policy, permissive).has_value());
    CY_CHECK_FALSE(permissive.failed());
}

CY_TEST_CASE("a same-major policy accepts a patch release and refuses a major one") {
    Manifest manifest(test_allocator());
    std::memcpy(manifest.build_id, "2.0.0", 6);
    LoadPolicy policy;
    policy.compatibility = Compatibility::SameMajorVersion;
    policy.build_id = "2.4.9";
    LoadReport report;
    CY_CHECK(check_compatibility(manifest, policy, report).has_value());

    policy.build_id = "3.0.0";
    LoadReport refused;
    CY_CHECK_FALSE(check_compatibility(manifest, policy, refused).has_value());
    CY_CHECK_EQ(refused.failure, LoadFailure::IncompatibleBuild);
}

CY_TEST_CASE("a content version mismatch is detected rather than misapplied") {
    // world-partition-and-streaming: "WHEN a save's content version does not match the installed
    // content THEN the mismatch SHALL be detected and reported, not silently applied."
    Manifest manifest(test_allocator());
    manifest.content_version = assets::content_hash("cooked-a", 8);
    LoadPolicy policy;
    policy.content_version = assets::content_hash("cooked-b", 8);
    LoadReport report;
    CY_CHECK_FALSE(check_compatibility(manifest, policy, report).has_value());
    CY_CHECK_EQ(report.failure, LoadFailure::MissingContent);
}

namespace {

bool no_plugins_installed(void* user, const PluginRequirement& plugin) noexcept {
    (void)user;
    (void)plugin;
    return false;
}

}  // namespace

CY_TEST_CASE("a missing plugin is named, and is not reported as corruption") {
    Manifest manifest(test_allocator());
    CY_REQUIRE(manifest.require_plugin("weather", 3).has_value());
    LoadPolicy policy;
    policy.plugin_present = no_plugins_installed;
    LoadReport report;
    CY_CHECK_FALSE(check_compatibility(manifest, policy, report).has_value());
    CY_CHECK_EQ(report.failure, LoadFailure::MissingPlugin);
    CY_CHECK_EQ(std::string_view(report.detail), std::string_view("weather"));
    CY_CHECK_EQ(std::string_view(load_failure_name(report.failure)),
                std::string_view("missing-plugin"));
}

CY_TEST_CASE("a save from a newer format is refused rather than reinterpreted") {
    Manifest manifest(test_allocator());
    manifest.format_version = kSaveFormatVersion + 1;
    LoadPolicy policy;
    LoadReport report;
    CY_CHECK_FALSE(check_compatibility(manifest, policy, report).has_value());
    CY_CHECK_EQ(report.failure, LoadFailure::IncompatibleFormat);
}
