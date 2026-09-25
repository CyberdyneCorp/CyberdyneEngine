// SPDX-License-Identifier: MIT
// The save inspector and the semantic save diff. `close-save-system-gaps`.
//
// `save-and-persistence` — "Save diagnostics and inspection". Every case here is named "a save is
// inspected..." or "a semantic diff...", which is the filter M11.a's `save-inspector` criterion
// runs: renaming one is a stale filter the criterion reports, not a silent loss of coverage.

#include <cy/core/reflect/registry.h>
#include <cy/save/archive.h>
#include <cy/save/inspect.h>
#include <cy/save/storage.h>
#include <cy/test/test.h>

#include "inspect_fixture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

using namespace cy;
using namespace cy::save;
using namespace cy::save::test;

namespace {

struct Campaign {
    MemoryBackend store{test_allocator()};
    SaveArchive archive{test_allocator()};
    reflect::TypeRegistry types;
    InspectOptions options;

    Campaign() {
        CY_REQUIRE(write_fixture_campaign(store, test_allocator()).has_value());
        CY_REQUIRE(archive.open(store).has_value());
        CY_REQUIRE(fixture_types(types).has_value());
        options.types = &types;
    }
};

const RegionUsage* region_usage(const SaveInspection& inspection, RegionKey key) noexcept {
    for (const RegionUsage& usage : inspection.regions) {
        if (usage.region == key) {
            return &usage;
        }
    }
    return nullptr;
}

const ComponentUsage* component_usage(const SaveInspection& inspection, u32 type) noexcept {
    for (const ComponentUsage& usage : inspection.components) {
        if (usage.type == reflect::TypeId(type)) {
            return &usage;
        }
    }
    return nullptr;
}

const PluginUsage* plugin_usage(const SaveInspection& inspection,
                                std::string_view module) noexcept {
    for (const PluginUsage& usage : inspection.plugins) {
        if (module == usage.module) {
            return &usage;
        }
    }
    return nullptr;
}

const ScopeUsage* scope_usage(const SaveInspection& inspection, Scope scope) noexcept {
    for (const ScopeUsage& usage : inspection.scopes) {
        if (usage.scope == scope) {
            return &usage;
        }
    }
    return nullptr;
}

const FieldOrigin* origin_of(const Array<FieldOrigin>& origins, PersistentId entity, u32 type,
                             u32 field) noexcept {
    for (const FieldOrigin& origin : origins) {
        if (!origin.fragment && origin.entity == entity && origin.type == reflect::TypeId(type) &&
            origin.field == reflect::FieldId(field)) {
            return &origin;
        }
    }
    return nullptr;
}

const FieldOrigin* entry_origin_of(const Array<FieldOrigin>& origins,
                                   PersistentId entity) noexcept {
    for (const FieldOrigin& origin : origins) {
        if (!origin.fragment && origin.entity == entity &&
            origin.reason == FieldReason::EntryRecord) {
            return &origin;
        }
    }
    return nullptr;
}

const FieldOrigin* fragment_origin_of(const Array<FieldOrigin>& origins, u32 field) noexcept {
    for (const FieldOrigin& origin : origins) {
        if (origin.fragment && origin.field == reflect::FieldId(field)) {
            return &origin;
        }
    }
    return nullptr;
}

bool has_item(const SaveDiff& diff, DiffKind kind, PersistentId entity, u32 type = 0,
              u32 field = 0) noexcept {
    return std::ranges::any_of(diff.items, [&](const DiffItem& item) {
        return item.kind == kind && item.entity == entity && item.type == reflect::TypeId(type) &&
               item.field == reflect::FieldId(field);
    });
}

std::string_view text_of(const Array<char>& text) noexcept {
    return {text.data(), text.size()};
}

}  // namespace

CY_TEST_CASE("a save is inspected: manifest, scope inventory, entity counts and sizes") {
    Campaign campaign;
    SaveInspection inspection(test_allocator());
    CY_REQUIRE(inspect_save(campaign.archive, 0, campaign.options, inspection).has_value());

    CY_CHECK_EQ(inspection.generation, 3U);
    CY_CHECK_EQ(inspection.manifest.simulation_point, 300U);
    CY_CHECK_EQ(std::string_view(inspection.manifest.build_id), std::string_view("11.5.0+fixture"));
    CY_CHECK_FALSE(inspection.restore.failed());
    CY_CHECK_EQ(inspection.generations.size(), 3U);

    // Generation 3: e1 and e10 modified, e20 still a tombstone, e2 and e30 gone.
    CY_CHECK_EQ(inspection.modified, 2U);
    CY_CHECK_EQ(inspection.created, 0U);
    CY_CHECK_EQ(inspection.tombstoned, 1U);
    CY_CHECK_EQ(inspection.fragments, 1U);

    // Size by region is the container's own encoding, so with no journal it is the manifest's.
    CY_REQUIRE_EQ(inspection.regions.size(), 3U);
    for (const RegionUsage& usage : inspection.regions) {
        const ChunkRef* chunk = inspection.manifest.find_chunk(Scope::World, usage.region);
        CY_REQUIRE(chunk != nullptr);
        CY_CHECK_EQ(usage.bytes, static_cast<u64>(chunk->size));
    }
    CY_CHECK_EQ(region_usage(inspection, kFixtureSummit)->tombstoned, 1U);
    CY_CHECK_EQ(region_usage(inspection, kFixtureVillage)->modified, 1U);

    // Size by scope: the world's regions and the profile's one fragment chunk, and they add up.
    const ScopeUsage* world = scope_usage(inspection, Scope::World);
    const ScopeUsage* profile = scope_usage(inspection, Scope::Profile);
    CY_REQUIRE(world != nullptr);
    CY_REQUIRE(profile != nullptr);
    CY_CHECK_EQ(world->chunks, 3U);
    CY_CHECK_EQ(profile->chunks, 1U);
    CY_CHECK_EQ(profile->bytes, inspection.manifest.chunk_bytes_in(Scope::Profile));
    CY_CHECK_EQ(world->bytes, inspection.manifest.chunk_bytes_in(Scope::World));
    CY_CHECK_EQ(scope_usage(inspection, Scope::Campaign), nullptr);
    CY_CHECK_EQ(inspection.total_bytes(), inspection.manifest.total_chunk_bytes());

    // Size by component and by plugin, through the schema's owning module.
    const ComponentUsage* health = component_usage(inspection, kHealthTypeId);
    const ComponentUsage* plugin_state = component_usage(inspection, kPluginTypeId);
    CY_REQUIRE(health != nullptr);
    CY_REQUIRE(plugin_state != nullptr);
    CY_CHECK_EQ(health->records, 1U);
    CY_CHECK_EQ(std::string_view(health->name), std::string_view("cy::save::test::Health"));
    CY_CHECK_EQ(std::string_view(plugin_state->module), std::string_view(kUndeclaredModule));
    CY_CHECK_GT(plugin_state->bytes, 0U);
    CY_REQUIRE(plugin_usage(inspection, "cy_building") != nullptr);
    CY_CHECK_EQ(plugin_usage(inspection, "cy_building")->records, 1U);
    CY_CHECK_EQ(plugin_usage(inspection, kUndeclaredModule)->bytes, plugin_state->bytes);

    Array<char> text(test_allocator());
    CY_REQUIRE(render_inspection(inspection, campaign.options, text).has_value());
    const std::string_view printed = text_of(text);
    CY_CHECK(printed.find("entities modified=2 created=0 tombstoned=1 fragments=1") !=
             std::string_view::npos);
    CY_CHECK(printed.find("manifest simulation-point=300") != std::string_view::npos);
    CY_CHECK(printed.find("size plugin=cy_gameplay") != std::string_view::npos);
    CY_CHECK(printed.find("restore ok") != std::string_view::npos);
}

CY_TEST_CASE("a save is inspected: why each field is in it, and when it became dirty") {
    Campaign campaign;
    SaveInspection inspection(test_allocator());
    CY_REQUIRE(inspect_save(campaign.archive, 0, campaign.options, inspection).has_value());
    Array<FieldOrigin> origins(test_allocator());
    CY_REQUIRE(explain_fields(campaign.archive, inspection, campaign.options, origins).has_value());

    // The component, the field, its persistence trait, the entity — and when.
    const FieldOrigin* revives = origin_of(origins, entity(1), kHealthTypeId, kHealthRevives);
    CY_REQUIRE(revives != nullptr);
    CY_CHECK_EQ(revives->reason, FieldReason::SaveGameTrait);
    CY_CHECK(has_trait(revives->traits, Trait::SaveGame));
    CY_CHECK_EQ(std::string_view(revives->component), std::string_view("cy::save::test::Health"));
    CY_CHECK_EQ(std::string_view(revives->field_name), std::string_view("revives"));
    CY_CHECK_EQ(revives->region, kFixtureVillage);
    // Unchanged since the oldest generation the store keeps: "at or before" it, not "at" it.
    CY_CHECK_EQ(revives->dirty_since_generation, 1U);
    CY_CHECK_EQ(revives->dirty_since_tick, 100U);
    CY_CHECK(revives->since_oldest_retained);

    // Changed in generation 3, so dirty since generation 3 exactly.
    const FieldOrigin* integrity =
        origin_of(origins, entity(10), kStructureTypeId, kStructureIntegrity);
    CY_REQUIRE(integrity != nullptr);
    CY_CHECK_EQ(integrity->dirty_since_generation, 3U);
    CY_CHECK_EQ(integrity->dirty_since_tick, 300U);
    CY_CHECK_FALSE(integrity->since_oldest_retained);
    const FieldOrigin* material =
        origin_of(origins, entity(10), kStructureTypeId, kStructureMaterial);
    CY_REQUIRE(material != nullptr);
    CY_CHECK_EQ(material->dirty_since_generation, 1U);

    // A tombstone is in the save because the entity was destroyed, since generation 2.
    const FieldOrigin* tombstone = entry_origin_of(origins, entity(20));
    CY_REQUIRE(tombstone != nullptr);
    CY_CHECK_EQ(tombstone->kind, EntryKind::Tombstone);
    CY_CHECK_EQ(tombstone->dirty_since_generation, 2U);
    CY_CHECK_EQ(tombstone->dirty_since_tick, 200U);

    // A plugin's state this schema does not declare is here because the load preserved it.
    const FieldOrigin* wetness = origin_of(origins, entity(1), kPluginTypeId, kPluginWetness);
    CY_REQUIRE(wetness != nullptr);
    CY_CHECK_EQ(wetness->reason, FieldReason::PreservedUnknownType);
    CY_CHECK_EQ(std::string_view(wetness->module), std::string_view(kUndeclaredModule));

    // A profile fragment's field routes through its scope attribute, and carries the Profile trait.
    const FieldOrigin* language = fragment_origin_of(origins, kSettingsLanguage);
    CY_REQUIRE(language != nullptr);
    CY_CHECK_EQ(language->scope, Scope::Profile);
    CY_CHECK(has_trait(language->traits, Trait::Profile));
    CY_CHECK_EQ(language->dirty_since_generation, 2U);

    // Nothing derived, runtime-only or transient is explained, because none of it is in the save.
    CY_CHECK_EQ(origin_of(origins, entity(1), kHealthTypeId, kHealthFraction), nullptr);
    CY_CHECK_EQ(origin_of(origins, entity(1), kHealthTypeId, kHealthCurrent), nullptr);

    Array<char> text(test_allocator());
    CY_REQUIRE(render_origins(origins.span(), text).has_value());
    const std::string_view printed = text_of(text);
    CY_CHECK(printed.find("component=9401:cy::save::test::Health field=3:revives wire=U32 "
                          "module=cy_gameplay trait=save-game+replay reason=save-game-trait "
                          "dirty-since=<=1 tick=100") != std::string_view::npos);
    CY_CHECK(printed.find("kind=tombstone reason=entry-record dirty-since=2 tick=200") !=
             std::string_view::npos);
}

CY_TEST_CASE("a save is inspected: why a refused save would not be restored") {
    Campaign campaign;
    campaign.options.policy.compatibility = Compatibility::ExactBuild;
    campaign.options.policy.build_id = "11.6.0";
    SaveInspection inspection(test_allocator());
    CY_REQUIRE(inspect_save(campaign.archive, 0, campaign.options, inspection).has_value());

    // Named — the build that wrote it — and the contents are still readable to the inspector.
    CY_CHECK_EQ(inspection.restore.failure, LoadFailure::IncompatibleBuild);
    CY_CHECK_EQ(std::string_view(inspection.restore.subject), std::string_view("11.5.0+fixture"));
    CY_CHECK_FALSE(inspection.contents.failed());
    CY_CHECK_EQ(inspection.modified, 2U);

    // A failed migration names the type and the version it could not be carried from.
    serialize::SchemaRegistry schemas(test_allocator());
    CY_REQUIRE(schemas.declare(reflect::TypeId(kHealthTypeId), 3).has_value());
    InspectOptions migrating = campaign.options;
    migrating.policy = LoadPolicy();
    migrating.policy.schemas = &schemas;
    SaveInspection stuck(test_allocator());
    CY_REQUIRE(inspect_save(campaign.archive, 0, migrating, stuck).has_value());
    CY_CHECK_EQ(stuck.restore.failure, LoadFailure::MigrationFailed);
    CY_CHECK_EQ(std::string_view(stuck.restore.subject), std::string_view("type 9401"));
    CY_CHECK_EQ(stuck.restore.subject_version, 1U);

    Array<char> text(test_allocator());
    CY_REQUIRE(render_inspection(inspection, campaign.options, text).has_value());
    CY_CHECK(text_of(text).find("restore refused reason=incompatible-build") !=
             std::string_view::npos);
    CY_CHECK(text_of(text).find("names=\"11.5.0+fixture\"") != std::string_view::npos);
}

CY_TEST_CASE("a save is inspected: a corrupt chunk is named and nothing is made up to fill it") {
    Campaign campaign;
    Manifest manifest(test_allocator());
    LoadReport ignored;
    CY_REQUIRE(campaign.archive.read_manifest(3, manifest, ignored).has_value());
    const ChunkRef* quarry = manifest.find_chunk(Scope::World, kFixtureQuarry);
    CY_REQUIRE(quarry != nullptr);
    char name[assets::ContentHash::kTextLength + 1] = {};
    quarry->hash.format(name);
    char key[128] = {};
    (void)std::snprintf(key, sizeof(key), "chunks/%s.cychunk", name);
    CY_REQUIRE(campaign.store.corrupt(key).has_value());

    SaveInspection inspection(test_allocator());
    CY_REQUIRE(inspect_save(campaign.archive, 3, campaign.options, inspection).has_value());
    CY_CHECK_EQ(inspection.restore.failure, LoadFailure::CorruptChunk);
    CY_CHECK(inspection.restore.chunk == quarry->hash);
    CY_CHECK(inspection.contents.failed());
    CY_CHECK_EQ(inspection.overlay.entry_count(), 0U);
    CY_CHECK_EQ(inspection.modified + inspection.created + inspection.tombstoned, 0U);
}

CY_TEST_CASE("a semantic diff names entities created, destroyed and reverted, and fields changed") {
    Campaign campaign;
    SaveInspection first(test_allocator());
    SaveInspection second(test_allocator());
    SaveInspection third(test_allocator());
    CY_REQUIRE(inspect_save(campaign.archive, 1, campaign.options, first).has_value());
    CY_REQUIRE(inspect_save(campaign.archive, 2, campaign.options, second).has_value());
    CY_REQUIRE(inspect_save(campaign.archive, 3, campaign.options, third).has_value());

    SaveDiff early(test_allocator());
    CY_REQUIRE(diff_saves(first, second, early).has_value());
    CY_CHECK(has_item(early, DiffKind::FieldChanged, entity(2), kHealthTypeId, kHealthRevives));
    CY_CHECK(has_item(early, DiffKind::EntityDestroyed, entity(20)));
    CY_CHECK(has_item(early, DiffKind::EntityCreated, entity(30)));
    CY_CHECK_EQ(early.count_of(DiffKind::FragmentChanged), 1U);
    CY_CHECK_EQ(early.count_of(DiffKind::SimulationPointChanged), 1U);
    // Nothing else moved: e1 and e10 are equal in both, so they are absent from the diff.
    CY_CHECK_EQ(early.items.size(), 5U);

    SaveDiff late(test_allocator());
    CY_REQUIRE(diff_saves(second, third, late).has_value());
    CY_CHECK(has_item(late, DiffKind::EntityReverted, entity(2)));
    CY_CHECK(has_item(late, DiffKind::EntityDestroyed, entity(30)));
    CY_CHECK(
        has_item(late, DiffKind::FieldChanged, entity(10), kStructureTypeId, kStructureIntegrity));
    CY_CHECK(has_item(late, DiffKind::FieldAdded, entity(1), kPluginTypeId, kPluginWetness));
    CY_CHECK_FALSE(has_item(late, DiffKind::EntityDestroyed, entity(20)));
    CY_CHECK_EQ(late.count_of(DiffKind::FragmentChanged), 1U);
    CY_CHECK_EQ(late.items.size(), 6U);

    Array<char> text(test_allocator());
    CY_REQUIRE(render_diff(late, &campaign.types, text).has_value());
    CY_CHECK(text_of(text).find("diff field-changed scope=world region=0a00000000000002 "
                                "entity=1000000000000000000000000000000a "
                                "component=9402:cy::save::test::Structure field=2:integrity") !=
             std::string_view::npos);
    CY_CHECK(text_of(text).find("diff total=6") != std::string_view::npos);
}

CY_TEST_CASE("a semantic diff reports fields in one-sided components") {
    Overlay before(test_allocator());
    Overlay after(test_allocator());
    Health health;
    health.revives = 3;
    Structure structure;
    structure.material = 4;
    structure.integrity = 80;
    CY_REQUIRE(
        before.record_component(kFixtureVillage, entity(2), health_type(), &health, 1).has_value());
    CY_REQUIRE(after.record_component(kFixtureVillage, entity(2), structure_type(), &structure, 1)
                   .has_value());

    SaveDiff diff(test_allocator());
    CY_REQUIRE(diff_overlays(before, after, diff).has_value());
    CY_CHECK(has_item(diff, DiffKind::FieldRemoved, entity(2), kHealthTypeId, kHealthRevives));
    CY_CHECK(
        has_item(diff, DiffKind::FieldAdded, entity(2), kStructureTypeId, kStructureIntegrity));
}

CY_TEST_CASE("a semantic diff of equal state is empty whatever the bytes") {
    Campaign campaign;
    SaveInspection same(test_allocator());
    CY_REQUIRE(inspect_save(campaign.archive, 2, campaign.options, same).has_value());
    SaveDiff none(test_allocator());
    CY_REQUIRE(diff_saves(same, same, none).has_value());
    CY_CHECK(none.empty());

    // The same logical state committed to another store, as a different generation, in another
    // insertion order: every manifest byte and pointer differs, and the diff says nothing changed.
    Overlay reordered(test_allocator());
    Settings settings;
    settings.gamma = 1.0F;
    settings.language = 2;
    CY_REQUIRE(
        reordered.record_fragment(Scope::Profile, settings_type(), &settings, 1).has_value());
    CY_REQUIRE(
        reordered.create_entity(kFixtureVillage, entity(30), AssetId(7, 7), entity(1)).has_value());
    Structure built;
    built.material = 4;
    built.integrity = 100;
    CY_REQUIRE(reordered.record_component(kFixtureVillage, entity(30), structure_type(), &built, 1)
                   .has_value());
    CY_REQUIRE(reordered.destroy_entity(kFixtureSummit, entity(20)).has_value());
    Structure quarry;
    quarry.material = 3;
    quarry.integrity = 90;
    CY_REQUIRE(reordered.record_component(kFixtureQuarry, entity(10), structure_type(), &quarry, 1)
                   .has_value());
    for (const auto& [id, revives] : {std::pair<u64, u32>{2, 5}, std::pair<u64, u32>{1, 1}}) {
        Health health;
        health.revives = revives;
        CY_REQUIRE(
            reordered.record_component(kFixtureVillage, entity(id), health_type(), &health, 1)
                .has_value());
    }
    reordered.set_simulation_point(200);

    MemoryBackend other(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(other).has_value());
    Overlay empty(test_allocator());
    CY_REQUIRE(archive.commit(empty, fixture_identity()).has_value());
    CY_REQUIRE(archive.commit(reordered, fixture_identity()).has_value());
    CY_REQUIRE(archive.commit(reordered, fixture_identity()).has_value());
    SaveInspection elsewhere(test_allocator());
    CY_REQUIRE(inspect_save(archive, 0, campaign.options, elsewhere).has_value());
    CY_CHECK_EQ(elsewhere.generation, 3U);

    SaveDiff semantic(test_allocator());
    CY_REQUIRE(diff_saves(same, elsewhere, semantic).has_value());
    CY_CHECK(semantic.empty());
}

CY_TEST_CASE("a save is inspected: world fragments and world regions are one scope's size") {
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());
    Overlay overlay(test_allocator());
    Structure wall;
    wall.material = 1;
    wall.integrity = 2;
    CY_REQUIRE(overlay.record_component(kFixtureQuarry, entity(10), structure_type(), &wall, 1)
                   .has_value());
    Settings world_settings;
    CY_REQUIRE(
        overlay.record_fragment(Scope::World, settings_type(), &world_settings, 1).has_value());
    CY_REQUIRE(archive.commit(overlay, fixture_identity()).has_value());

    SaveInspection inspection(test_allocator());
    CY_REQUIRE(inspect_save(archive, 0, InspectOptions(), inspection).has_value());
    usize world_rows = 0;
    for (const ScopeUsage& usage : inspection.scopes) {
        world_rows += usage.scope == Scope::World ? 1U : 0U;
    }
    CY_CHECK_EQ(world_rows, 1U);
    const ScopeUsage* world = scope_usage(inspection, Scope::World);
    CY_REQUIRE(world != nullptr);
    CY_CHECK_EQ(world->chunks, 2U);
    CY_CHECK_EQ(world->bytes, inspection.manifest.chunk_bytes_in(Scope::World));
}
