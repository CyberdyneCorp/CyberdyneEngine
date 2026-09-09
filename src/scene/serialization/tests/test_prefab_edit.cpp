// Apply and extract. M8.b task 11.1.
//
// THE CRITERION EVERY CASE HERE IS BUILT ON is prefab_edit.h's: an operation that moves authored
// data across an asset boundary must not change what the container resolves to, and `diff` over two
// resolved graphs is the whole-graph way to say so. A case that asserted on one field would pass
// while the operation dropped a component nobody thought to check.

#include <cy/core/serialize/wire.h>
#include <cy/scene/serialization/prefab_edit.h>
#include <cy/scene/serialization/resolve.h>
#include <cy/test/test.h>

#include <utility>

#include "fixtures.h"

using namespace cy;
using namespace cy::scene::serialization;
using namespace cy::scene::serialization::test;

namespace {

constexpr u64 kTurretAsset = 0x7011E7;
constexpr u64 kSceneAsset = 0x5CE7E;
constexpr u64 kExtractedAsset = 0xE47AC7;
constexpr u64 kSecondSceneAsset = 0x5CE7F;

/// A turret prefab: a base with health, a yaw, and a muzzle bolted to the yaw.
[[nodiscard]] Status build_turret(Document& document) noexcept {
    document.kind = AssetKind::Prefab;
    document.id = asset(kTurretAsset);

    const Expected<DocumentEntity*, Error> base = document.add_entity(kNoLocalId, "Base");
    if (!base) {
        return make_unexpected(base.error());
    }
    if (Status placed = set_placement(**base, cy::Transform::identity()); !placed) {
        return placed;
    }
    if (Status healthy = set_health(**base, 100.0F, 100.0F); !healthy) {
        return healthy;
    }

    const Expected<DocumentEntity*, Error> yaw = document.add_entity((*base)->id, "Yaw");
    if (!yaw) {
        return make_unexpected(yaw.error());
    }
    if (Status placed = set_placement(**yaw, cy::Transform::from_translation(cy::Vec3{0, 1, 0}));
        !placed) {
        return placed;
    }

    const Expected<DocumentEntity*, Error> muzzle = document.add_entity((*yaw)->id, "Muzzle");
    if (!muzzle) {
        return make_unexpected(muzzle.error());
    }
    return set_placement(**muzzle, cy::Transform::from_translation(cy::Vec3{0, 0, 2}));
}

[[nodiscard]] ResolveOptions fixture_options() noexcept {
    ResolveOptions options;
    options.transform =
        TransformBinding{reflect::TypeId(kPlacementType), reflect::FieldId(kPlacementLocal)};
    return options;
}

/// Resolve `id` out of `library` into `out`. The report is discarded: every case here that cares
/// about it makes its own.
[[nodiscard]] Status resolve_into(const Library& library, AssetId id, ResolvedGraph& out) noexcept {
    ResolveReport report(test_allocator());
    return resolve(library, id, fixture_options(), out, report);
}

/// `Override` setting one scalar field, ready to add.
[[nodiscard]] Expected<Override, Error> field_override(Allocator& allocator, LocalId entity,
                                                       u32 type, u32 field, const void* value,
                                                       u32 size,
                                                       serialize::WireType wire) noexcept {
    Override item(allocator);
    item.set_op(OverrideOp::SetField);
    item.set_target(OverrideTarget{entity, reflect::TypeId(type), reflect::FieldId(field)});
    item.payload().set_type(reflect::TypeId(type));
    if (Status written = item.payload().set_scalar(reflect::FieldId(field), wire, value, size);
        !written) {
        return make_unexpected(written.error());
    }
    return item;
}

/// The `Health.maximum` of one resolved entity.
[[nodiscard]] Expected<f32, Error> maximum_of(const ResolvedGraph& graph, LocalId entity) noexcept {
    const ResolvedEntity* found = graph.find(entity);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "no such entity in the graph");
    }
    const ResolvedComponent* component =
        find_resolved_component(*found, reflect::TypeId(kHealthType));
    if (component == nullptr) {
        return fail(ErrorCode::NotFound, "no health on that entity");
    }
    f32 value = 0.0F;
    const Span<const u8> bytes = component->record.bytes(reflect::FieldId(kHealthMaximum));
    if (bytes.size() != sizeof(value)) {
        return fail(ErrorCode::NotFound, "no maximum on that entity");
    }
    if (Status decoded =
            serialize::decode_scalar(serialize::WireType::F32, bytes.data(), 4, &value);
        !decoded) {
        return make_unexpected(decoded.error());
    }
    return value;
}

/// A three-entity scene: a `Root`, a `Turret` under it with a child `Barrel`, and a `Spotter`
/// beside the turret whose `Target` points INTO the subtree — which is the reference extraction has
/// to keep resolving.
struct ExtractFixture {
    explicit ExtractFixture(Allocator& allocator) noexcept : scene(allocator) {}

    Document scene;
    LocalId root;
    LocalId turret;
    LocalId barrel;
    LocalId spotter;

    [[nodiscard]] Status build() noexcept {
        scene.kind = AssetKind::Scene;
        scene.id = asset(kSceneAsset);

        const Expected<DocumentEntity*, Error> root_entity = scene.add_entity(kNoLocalId, "Root");
        if (!root_entity) {
            return make_unexpected(root_entity.error());
        }
        root = (*root_entity)->id;
        if (Status placed = set_placement(**root_entity, cy::Transform::identity()); !placed) {
            return placed;
        }

        const Expected<DocumentEntity*, Error> turret_entity = scene.add_entity(root, "Turret");
        if (!turret_entity) {
            return make_unexpected(turret_entity.error());
        }
        turret = (*turret_entity)->id;
        if (Status placed =
                set_placement(**turret_entity, cy::Transform::from_translation(cy::Vec3{3, 0, 0}));
            !placed) {
            return placed;
        }
        if (Status healthy = set_health(**turret_entity, 250.0F, 250.0F); !healthy) {
            return healthy;
        }

        const Expected<DocumentEntity*, Error> barrel_entity = scene.add_entity(turret, "Barrel");
        if (!barrel_entity) {
            return make_unexpected(barrel_entity.error());
        }
        barrel = (*barrel_entity)->id;
        if (Status placed =
                set_placement(**barrel_entity, cy::Transform::from_translation(cy::Vec3{0, 0, 1}));
            !placed) {
            return placed;
        }

        const Expected<DocumentEntity*, Error> spotter_entity = scene.add_entity(root, "Spotter");
        if (!spotter_entity) {
            return make_unexpected(spotter_entity.error());
        }
        spotter = (*spotter_entity)->id;
        if (Status placed = set_placement(**spotter_entity, cy::Transform::identity()); !placed) {
            return placed;
        }
        return set_target(**spotter_entity, barrel);
    }
};

}  // namespace

CY_TEST_CASE("applying an instance's overrides updates the prefab and clears the list") {
    Document turret(test_allocator());
    CY_REQUIRE(build_turret(turret).has_value());

    Document scene(test_allocator());
    scene.kind = AssetKind::Scene;
    scene.id = asset(kSceneAsset);
    const Expected<Instance*, Error> first =
        scene.add_instance(asset(kTurretAsset), kNoLocalId, "A");
    const Expected<Instance*, Error> second =
        scene.add_instance(asset(kTurretAsset), kNoLocalId, "B");
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());

    const f32 maximum = 400.0F;
    Expected<Override, Error> item =
        field_override(scene.allocator(), LocalId(1), kHealthType, kHealthMaximum, &maximum,
                       sizeof(maximum), serialize::WireType::F32);
    CY_REQUIRE(item.has_value());
    CY_REQUIRE((*first)->overrides().add(std::move(item.value())).has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(turret).has_value());
    CY_REQUIRE(library.add(scene).has_value());
    CY_REQUIRE(populate_mapping(library, scene, *first.value(), fixture_options()).has_value());
    CY_REQUIRE(populate_mapping(library, scene, *second.value(), fixture_options()).has_value());

    ResolvedGraph before(test_allocator());
    CY_REQUIRE(resolve_into(library, asset(kSceneAsset), before).has_value());
    const LocalId a_base = (*first)->local_of(LocalId(1));
    const LocalId b_base = (*second)->local_of(LocalId(1));
    CY_CHECK_EQ(maximum_of(before, a_base).value(), 400.0F);
    CY_CHECK_EQ(maximum_of(before, b_base).value(), 100.0F);

    ApplyReport report;
    CY_REQUIRE(apply_instance_overrides(library, scene, **first, report).has_value());
    CY_CHECK_EQ(report.applied, 1U);
    CY_CHECK_EQ(report.conflicted, 0U);
    CY_CHECK_EQ(report.fields_set, 1U);

    // "the prefab asset SHALL be updated and the instance's override list cleared"
    CY_CHECK((*first)->overrides().empty());
    ResolvedGraph after(test_allocator());
    CY_REQUIRE(resolve_into(library, asset(kSceneAsset), after).has_value());
    CY_CHECK_EQ(maximum_of(after, a_base).value(), 400.0F);

    // "with other instances picking up the change" — and the ONLY thing that changed in the whole
    // scene is that one field on the other instance.
    CY_CHECK_EQ(maximum_of(after, b_base).value(), 400.0F);
    Array<GraphDifference> differences(test_allocator());
    CY_REQUIRE(diff(before, after, differences).has_value());
    CY_REQUIRE_EQ(differences.size(), 1U);
    CY_CHECK_EQ(differences[0].kind, GraphDifference::Kind::FieldChanged);
    CY_CHECK(differences[0].entity == b_base);
    CY_CHECK(differences[0].field == reflect::FieldId(kHealthMaximum));
}

CY_TEST_CASE("an override an apply cannot place is marked and kept, never discarded") {
    Document turret(test_allocator());
    CY_REQUIRE(build_turret(turret).has_value());

    Document scene(test_allocator());
    scene.kind = AssetKind::Scene;
    scene.id = asset(kSceneAsset);
    const Expected<Instance*, Error> instance =
        scene.add_instance(asset(kTurretAsset), kNoLocalId, "A");
    CY_REQUIRE(instance.has_value());

    const f32 maximum = 400.0F;
    // Entity 40 is not in the turret and never was.
    Expected<Override, Error> gone =
        field_override(scene.allocator(), LocalId(40), kHealthType, kHealthMaximum, &maximum,
                       sizeof(maximum), serialize::WireType::F32);
    CY_REQUIRE(gone.has_value());
    CY_REQUIRE((*instance)->overrides().add(std::move(gone.value())).has_value());
    // And one that does apply, so the pass is not simply refusing everything.
    Expected<Override, Error> good =
        field_override(scene.allocator(), LocalId(1), kHealthType, kHealthMaximum, &maximum,
                       sizeof(maximum), serialize::WireType::F32);
    CY_REQUIRE(good.has_value());
    CY_REQUIRE((*instance)->overrides().add(std::move(good.value())).has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(turret).has_value());
    CY_REQUIRE(library.add(scene).has_value());

    ApplyReport report;
    CY_REQUIRE(apply_instance_overrides(library, scene, **instance, report).has_value());
    CY_CHECK_EQ(report.applied, 1U);
    CY_CHECK_EQ(report.conflicted, 1U);

    CY_REQUIRE_EQ((*instance)->overrides().size(), 1U);
    CY_CHECK_EQ((*instance)->overrides()[0].conflict(), ConflictKind::MissingEntity);
    CY_CHECK_EQ((*instance)->overrides().conflict_count(), 1U);
    // The payload it carried is still there: a conflict keeps the work a designer did.
    CY_CHECK((*instance)->overrides()[0].payload().contains(reflect::FieldId(kHealthMaximum)));
}

CY_TEST_CASE("an added entity survives an apply with the id the container already refers to") {
    Document turret(test_allocator());
    CY_REQUIRE(build_turret(turret).has_value());

    Document scene(test_allocator());
    scene.kind = AssetKind::Scene;
    scene.id = asset(kSceneAsset);
    const Expected<Instance*, Error> instance =
        scene.add_instance(asset(kTurretAsset), kNoLocalId, "A");
    CY_REQUIRE(instance.has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(turret).has_value());
    CY_REQUIRE(library.add(scene).has_value());
    CY_REQUIRE(populate_mapping(library, scene, *instance.value(), fixture_options()).has_value());

    // The container invents an entity under the turret's base, and then puts health on it. The two
    // overrides address the SAME container-invented id, which is the case an apply gets wrong if it
    // does not retarget: the first lands, the second conflicts.
    const LocalId invented = scene.allocate_id();
    Override added(scene.allocator());
    added.set_op(OverrideOp::AddEntity);
    added.set_target(OverrideTarget{invented, reflect::TypeId(), reflect::FieldId()});
    added.set_parent(LocalId(1));
    CY_REQUIRE((*instance)->overrides().add(std::move(added)).has_value());

    const f32 maximum = 55.0F;
    Expected<Override, Error> health =
        field_override(scene.allocator(), invented, kHealthType, kHealthMaximum, &maximum,
                       sizeof(maximum), serialize::WireType::F32);
    CY_REQUIRE(health.has_value());
    health->set_op(OverrideOp::AddComponent);
    CY_REQUIRE((*instance)->overrides().add(std::move(health.value())).has_value());

    ResolvedGraph before(test_allocator());
    CY_REQUIRE(resolve_into(library, asset(kSceneAsset), before).has_value());
    CY_CHECK_EQ(before.entities().size(), 4U);
    CY_CHECK_EQ(maximum_of(before, invented).value(), 55.0F);

    ApplyReport report;
    CY_REQUIRE(apply_instance_overrides(library, scene, **instance, report).has_value());
    CY_CHECK_EQ(report.applied, 2U);
    CY_CHECK_EQ(report.conflicted, 0U);
    CY_CHECK_EQ(report.entities_added, 1U);
    CY_CHECK_EQ(report.components_added, 1U);
    CY_CHECK_EQ(turret.entities().size(), 4U);

    // The whole scene resolves to exactly what it did before: the entity is now the prefab's, and
    // the placement still gives it the local id the container was already using.
    ResolvedGraph after(test_allocator());
    CY_REQUIRE(resolve_into(library, asset(kSceneAsset), after).has_value());
    Array<GraphDifference> differences(test_allocator());
    CY_REQUIRE(diff(before, after, differences).has_value());
    CY_CHECK_EQ(differences.size(), 0U);
    CY_CHECK_EQ(maximum_of(after, invented).value(), 55.0F);
}

CY_TEST_CASE("extracting a subtree leaves the scene resolving to exactly what it did") {
    ExtractFixture fixture(test_allocator());
    CY_REQUIRE(fixture.build().has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(fixture.scene).has_value());
    ResolvedGraph before(test_allocator());
    CY_REQUIRE(resolve_into(library, asset(kSceneAsset), before).has_value());
    CY_CHECK_EQ(before.entities().size(), 4U);

    Document extracted(test_allocator());
    ExtractReport report;
    CY_REQUIRE(extract_prefab(fixture.scene, fixture.turret, asset(kExtractedAsset), "Turret",
                              extracted, report)
                   .has_value());
    CY_CHECK_EQ(report.entities, 2U);
    CY_CHECK_EQ(report.instances, 0U);
    CY_CHECK(report.instance.valid());

    // "a new prefab asset SHALL be created and the scene SHALL contain an instance of it"
    CY_CHECK_EQ(extracted.kind, AssetKind::Prefab);
    CY_CHECK_EQ(extracted.entities().size(), 2U);
    CY_CHECK_EQ(fixture.scene.entities().size(), 2U);
    CY_REQUIRE_EQ(fixture.scene.instances().size(), 1U);
    CY_CHECK(fixture.scene.instances()[0].source == asset(kExtractedAsset));

    CY_REQUIRE(library.add(extracted).has_value());
    ResolvedGraph after(test_allocator());
    CY_REQUIRE(resolve_into(library, asset(kSceneAsset), after).has_value());
    Array<GraphDifference> differences(test_allocator());
    CY_REQUIRE(diff(before, after, differences).has_value());
    CY_CHECK_EQ(differences.size(), 0U);

    // "with external references to the subtree rewritten to point at the instance" — the spotter's
    // target still names the barrel, and the barrel is now the instance's.
    const ResolvedEntity* spotter = after.find(fixture.spotter);
    CY_REQUIRE(spotter != nullptr);
    const ResolvedComponent* target =
        find_resolved_component(*spotter, reflect::TypeId(kTargetType));
    CY_REQUIRE(target != nullptr);
    const Expected<u32, Error> referenced =
        target->record.local_reference(reflect::FieldId(kTargetEntity));
    CY_REQUIRE(referenced.has_value());
    CY_CHECK_EQ(referenced.value(), fixture.barrel.value());
    const ResolvedEntity* barrel = after.find(fixture.barrel);
    CY_REQUIRE(barrel != nullptr);
    CY_CHECK(barrel->origin == asset(kExtractedAsset));
}

CY_TEST_CASE("an extracted prefab is an ordinary prefab that places anywhere else") {
    ExtractFixture fixture(test_allocator());
    CY_REQUIRE(fixture.build().has_value());

    Document extracted(test_allocator());
    ExtractReport report;
    CY_REQUIRE(extract_prefab(fixture.scene, fixture.turret, asset(kExtractedAsset), "Turret",
                              extracted, report)
                   .has_value());

    Document other(test_allocator());
    other.kind = AssetKind::Scene;
    other.id = asset(kSecondSceneAsset);
    const Expected<Instance*, Error> placed =
        other.add_instance(asset(kExtractedAsset), kNoLocalId, "Copy");
    CY_REQUIRE(placed.has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(fixture.scene).has_value());
    CY_REQUIRE(library.add(extracted).has_value());
    CY_REQUIRE(library.add(other).has_value());
    CY_REQUIRE(populate_mapping(library, other, *placed.value(), fixture_options()).has_value());

    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(resolve_into(library, asset(kSecondSceneAsset), graph).has_value());
    CY_CHECK_EQ(graph.entities().size(), 2U);
    CY_CHECK_EQ(maximum_of(graph, (*placed)->local_of(fixture.turret)).value(), 250.0F);
}

CY_TEST_CASE("extraction refuses a destination that is not empty or an identity that is not new") {
    ExtractFixture fixture(test_allocator());
    CY_REQUIRE(fixture.build().has_value());

    Document occupied(test_allocator());
    CY_REQUIRE(occupied.add_entity(kNoLocalId, "Squatter").has_value());
    ExtractReport report;
    CY_CHECK(!extract_prefab(fixture.scene, fixture.turret, asset(kExtractedAsset), "Turret",
                             occupied, report)
                  .has_value());

    Document empty(test_allocator());
    CY_CHECK(
        !extract_prefab(fixture.scene, fixture.turret, asset(kSceneAsset), "Turret", empty, report)
             .has_value());
    CY_CHECK(!extract_prefab(fixture.scene, LocalId(9999), asset(kExtractedAsset), "Turret", empty,
                             report)
                  .has_value());
    // Nothing was moved by any of the three refusals.
    CY_CHECK_EQ(fixture.scene.entities().size(), 4U);
    CY_CHECK_EQ(fixture.scene.instances().size(), 0U);
}

// --- The regression the case above found -------------------------------------------------------
//
// `Resolver::apply_one` added an `AddEntity`'s entity to the graph and did NOT add it to the id
// map, so every later override addressing it missed and was recorded as a `MissingEntity`
// conflict — an `AddEntity` you could not then put a component on. Found while writing the apply
// case above, which crashed on the resolve it makes BEFORE applying anything.
CY_TEST_CASE("an override can fill an entity an earlier override added") {
    Document turret(test_allocator());
    CY_REQUIRE(build_turret(turret).has_value());

    Document scene(test_allocator());
    scene.kind = AssetKind::Scene;
    scene.id = asset(kSceneAsset);
    const Expected<Instance*, Error> instance =
        scene.add_instance(asset(kTurretAsset), kNoLocalId, "A");
    CY_REQUIRE(instance.has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(turret).has_value());
    CY_REQUIRE(library.add(scene).has_value());
    // THE MAPPING IS POPULATED FIRST, as an editor's own placement does it, so the id the container
    // invents afterwards is above every id the placement handed out. An id below them is genuinely
    // ambiguous — `IdMap` is keyed by the SOURCE's numbers and an override naming one of those
    // reaches the source's entity — and this case is about the added entity, not about that.
    CY_REQUIRE(populate_mapping(library, scene, *instance.value(), fixture_options()).has_value());

    const LocalId invented = scene.allocate_id();
    Override added(scene.allocator());
    added.set_op(OverrideOp::AddEntity);
    added.set_target(OverrideTarget{invented, reflect::TypeId(), reflect::FieldId()});
    CY_REQUIRE((*instance)->overrides().add(std::move(added)).has_value());

    const f32 maximum = 7.0F;
    Expected<Override, Error> health =
        field_override(scene.allocator(), invented, kHealthType, kHealthMaximum, &maximum,
                       sizeof(maximum), serialize::WireType::F32);
    CY_REQUIRE(health.has_value());
    health->set_op(OverrideOp::AddComponent);
    CY_REQUIRE((*instance)->overrides().add(std::move(health.value())).has_value());

    ResolveReport report(test_allocator());
    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(resolve(library, asset(kSceneAsset), fixture_options(), graph, report).has_value());
    CY_CHECK_EQ(report.overrides_applied, 2U);
    CY_CHECK_EQ(report.conflicts.size(), 0U);
    CY_REQUIRE(maximum_of(graph, invented).has_value());
    CY_CHECK_EQ(maximum_of(graph, invented).value(), 7.0F);
}
