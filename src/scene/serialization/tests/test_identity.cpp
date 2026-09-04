// The claim M1's identity work exists for: an override addresses identifiers, and survives a
// rename.

#include <cy/core/serialize/migration.h>
#include <cy/scene/serialization/format.h>
#include <cy/scene/serialization/resolve.h>
#include <cy/test/test.h>

#include <string_view>

#include "fixtures.h"

using namespace cy;
using namespace cy::scene::serialization;
using namespace cy::scene::serialization::test;

namespace {

constexpr u64 kPrefabAsset = 0xB0B;
constexpr u64 kSceneAsset = 0x5CE7E;

/// A prefab of one entity carrying health.
[[nodiscard]] Status build_prefab(Document& document) noexcept {
    document.kind = AssetKind::Prefab;
    document.id = asset(kPrefabAsset);
    const Expected<DocumentEntity*, Error> entity = document.add_entity(kNoLocalId, "Robot");
    if (!entity) {
        return make_unexpected(entity.error());
    }
    return set_health(**entity, 100.0F, 100.0F);
}

[[nodiscard]] Expected<f32, Error> maximum_of(const ResolvedGraph& graph, LocalId entity,
                                              u32 field) noexcept {
    const ResolvedEntity* found = graph.find(entity);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "no such entity");
    }
    const ResolvedComponent* component =
        find_resolved_component(*found, reflect::TypeId(kHealthType));
    if (component == nullptr) {
        return fail(ErrorCode::NotFound, "no health");
    }
    const Span<const u8> bytes = component->record.bytes(reflect::FieldId(field));
    if (bytes.size() != sizeof(f32)) {
        return fail(ErrorCode::NotFound, "the field is absent");
    }
    f32 value = 0.0F;
    if (Status decoded =
            serialize::decode_scalar(serialize::WireType::F32, bytes.data(), 4, &value);
        !decoded) {
        return make_unexpected(decoded.error());
    }
    return value;
}

}  // namespace

CY_TEST_CASE("renaming an entity does not touch the overrides that target it") {
    Document prefab(test_allocator());
    CY_REQUIRE(build_prefab(prefab).has_value());

    Document scene(test_allocator());
    scene.kind = AssetKind::Scene;
    scene.id = asset(kSceneAsset);
    const Expected<Instance*, Error> instance =
        scene.add_instance(asset(kPrefabAsset), kNoLocalId, "A");
    CY_REQUIRE(instance.has_value());

    Override item(scene.allocator());
    item.set_op(OverrideOp::SetField);
    item.set_target(
        OverrideTarget{LocalId(1), reflect::TypeId(kHealthType), reflect::FieldId(kHealthMaximum)});
    item.payload().set_type(reflect::TypeId(kHealthType));
    const f32 maximum = 400.0F;
    CY_REQUIRE(
        item.payload()
            .set_scalar(reflect::FieldId(kHealthMaximum), serialize::WireType::F32, &maximum, 4)
            .has_value());
    CY_REQUIRE((*instance)->overrides().add(std::move(item)).has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(prefab).has_value());
    CY_REQUIRE(library.add(scene).has_value());
    ResolveOptions options;
    CY_REQUIRE(populate_mapping(library, scene, *instance.value(), options).has_value());

    // The entity is renamed inside the prefab. A name path such as `Root.Robot.Health.Max` would
    // now resolve to nothing; three identifiers do not care.
    DocumentEntity* entity = prefab.find_entity(LocalId(1));
    CY_REQUIRE(entity != nullptr);
    const Expected<TextRef, Error> renamed = prefab.intern("Sentinel");
    CY_REQUIRE(renamed.has_value());
    entity->name = renamed.value();

    ResolveReport report(test_allocator());
    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(resolve(library, asset(kSceneAsset), options, graph, report).has_value());
    CY_CHECK(report.conflicts.empty());
    const Expected<f32, Error> value =
        maximum_of(graph, (*instance)->local_of(LocalId(1)), kHealthMaximum);
    CY_REQUIRE(value.has_value());
    CY_CHECK_EQ(value.value(), 400.0F);
}

CY_TEST_CASE("a field that changes identity carries its overrides with it") {
    // "Overrides migrate with the type": a migration that moves a field's identifier moves the
    // overrides that address it. Dropping them "SHALL be a defect".
    //
    // The old identifier is tombstoned by the manifest and never reissued, which is what makes the
    // migration safe: the number the override still names cannot have become some other field.
    constexpr u32 kOldMaximum = kHealthMaximum;
    constexpr u32 kNewMaximum = 40;

    Document prefab(test_allocator());
    CY_REQUIRE(build_prefab(prefab).has_value());
    // The prefab's data has already migrated: it carries the new identifier.
    DocumentEntity* entity = prefab.find_entity(LocalId(1));
    CY_REQUIRE(entity != nullptr);
    ComponentData* health = entity->find(reflect::TypeId(kHealthType));
    CY_REQUIRE(health != nullptr);
    CY_REQUIRE(health->record.retarget(reflect::FieldId(kOldMaximum), reflect::FieldId(kNewMaximum))
                   .has_value());

    // The override was authored against schema version 1 and still names the old identifier.
    Document scene(test_allocator());
    scene.kind = AssetKind::Scene;
    scene.id = asset(kSceneAsset);
    const Expected<Instance*, Error> instance =
        scene.add_instance(asset(kPrefabAsset), kNoLocalId, "A");
    CY_REQUIRE(instance.has_value());
    Override item(scene.allocator());
    item.set_op(OverrideOp::SetField);
    item.set_target(
        OverrideTarget{LocalId(1), reflect::TypeId(kHealthType), reflect::FieldId(kOldMaximum)});
    item.set_schema_version(1);
    item.payload().set_type(reflect::TypeId(kHealthType));
    const f32 maximum = 900.0F;
    CY_REQUIRE(item.payload()
                   .set_scalar(reflect::FieldId(kNewMaximum), serialize::WireType::F32, &maximum, 4)
                   .has_value());
    CY_REQUIRE((*instance)->overrides().add(std::move(item)).has_value());

    serialize::SchemaRegistry schemas(test_allocator());
    CY_REQUIRE(schemas.declare(reflect::TypeId(kHealthType), 2).has_value());
    const serialize::FieldRemap remaps[] = {
        {reflect::FieldId(kOldMaximum), reflect::FieldId(kNewMaximum)}};
    CY_REQUIRE(
        schemas
            .add_migration(serialize::Migration{
                reflect::TypeId(kHealthType), 1, 2, serialize::MigrationClass::Automatic,
                "maximum moves identity", nullptr, Span<const serialize::FieldRemap>(remaps, 1)})
            .has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(prefab).has_value());
    CY_REQUIRE(library.add(scene).has_value());
    ResolveOptions options;
    options.schemas = &schemas;
    CY_REQUIRE(populate_mapping(library, scene, *instance.value(), options).has_value());

    ResolveReport report(test_allocator());
    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(resolve(library, asset(kSceneAsset), options, graph, report).has_value());

    // The override was migrated to the new identifier and applied, not discarded.
    CY_CHECK(report.conflicts.empty());
    CY_CHECK_EQ(report.overrides_applied, 1U);
    CY_CHECK_EQ((*instance)->overrides()[0].target().field.value(), kNewMaximum);
    const Expected<f32, Error> value =
        maximum_of(graph, (*instance)->local_of(LocalId(1)), kNewMaximum);
    CY_REQUIRE(value.has_value());
    CY_CHECK_EQ(value.value(), 900.0F);
}

CY_TEST_CASE("an override against a field the type no longer has becomes a conflict") {
    Document prefab(test_allocator());
    CY_REQUIRE(build_prefab(prefab).has_value());

    Document scene(test_allocator());
    scene.kind = AssetKind::Scene;
    scene.id = asset(kSceneAsset);
    const Expected<Instance*, Error> instance =
        scene.add_instance(asset(kPrefabAsset), kNoLocalId, "A");
    CY_REQUIRE(instance.has_value());
    Override item(scene.allocator());
    item.set_op(OverrideOp::SetField);
    // Identifier 99 is one the manifest tombstoned: it names a field that used to exist.
    item.set_target(OverrideTarget{LocalId(1), reflect::TypeId(kHealthType), reflect::FieldId(99)});
    item.payload().set_type(reflect::TypeId(kHealthType));
    const f32 value = 1.0F;
    CY_REQUIRE(item.payload()
                   .set_scalar(reflect::FieldId(99), serialize::WireType::F32, &value, 4)
                   .has_value());
    CY_REQUIRE((*instance)->overrides().add(std::move(item)).has_value());

    reflect::TypeRegistry types;
    CY_REQUIRE(types.add(health_type()).has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(prefab).has_value());
    CY_REQUIRE(library.add(scene).has_value());
    ResolveOptions options;
    options.types = &types;
    CY_REQUIRE(populate_mapping(library, scene, *instance.value(), options).has_value());

    ResolveReport report(test_allocator());
    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(resolve(library, asset(kSceneAsset), options, graph, report).has_value());
    CY_REQUIRE_EQ(report.conflicts.size(), 1U);
    CY_CHECK_EQ(report.conflicts[0].kind, ConflictKind::MissingField);
    // Retained, and offered a resolution rather than discarded.
    CY_CHECK((*instance)->overrides()[0].conflicted());
    (*instance)->overrides()[0].resolve_retarget(
        OverrideTarget{LocalId(1), reflect::TypeId(kHealthType), reflect::FieldId(kHealthMaximum)});
    CY_CHECK_FALSE((*instance)->overrides()[0].conflicted());
}

CY_TEST_CASE("a conflicted override survives a save and a load") {
    // "An override SHALL NOT be dropped silently in any build configuration" — including by the
    // round trip through the file it lives in.
    Document scene(test_allocator());
    scene.kind = AssetKind::Scene;
    scene.id = asset(kSceneAsset);
    const Expected<Instance*, Error> instance =
        scene.add_instance(asset(kPrefabAsset), kNoLocalId, "A");
    CY_REQUIRE(instance.has_value());
    Override item(scene.allocator());
    item.set_op(OverrideOp::SetField);
    item.set_target(
        OverrideTarget{LocalId(7), reflect::TypeId(kHealthType), reflect::FieldId(kHealthMaximum)});
    item.set_conflict(ConflictKind::MissingEntity);
    item.payload().set_type(reflect::TypeId(kHealthType));
    const f32 value = 12.0F;
    CY_REQUIRE(
        item.payload()
            .set_scalar(reflect::FieldId(kHealthMaximum), serialize::WireType::F32, &value, 4)
            .has_value());
    CY_REQUIRE((*instance)->overrides().add(std::move(item)).has_value());

    Array<char> text(test_allocator());
    CY_REQUIRE(write_text(scene, text).has_value());
    Document reloaded(test_allocator());
    CY_REQUIRE(read_text(std::string_view(text.data(), text.size()), reloaded).has_value());

    const Instance* restored = reloaded.find_instance((*instance)->id);
    CY_REQUIRE(restored != nullptr);
    CY_REQUIRE_EQ(restored->overrides().size(), 1U);
    CY_CHECK_EQ(restored->overrides()[0].conflict(), ConflictKind::MissingEntity);
    CY_CHECK(restored->overrides()[0].payload().contains(reflect::FieldId(kHealthMaximum)));
}
