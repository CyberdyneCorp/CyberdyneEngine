// Prefabs: instance overrides, variants, exposed parameters, conflicts, provenance and diff.

#include <cy/core/serialize/migration.h>
#include <cy/scene/serialization/resolve.h>
#include <cy/test/test.h>

#include <cstring>

#include "fixtures.h"

using namespace cy;
using namespace cy::scene::serialization;
using namespace cy::scene::serialization::test;

namespace {

constexpr u64 kTurretAsset = 0x7011E7;
constexpr u64 kVariantAsset = 0x7011E8;
constexpr u64 kSceneAsset = 0x5CE7E;

/// A turret prefab: a base, a yaw that rotates, and a muzzle bolted to the yaw.
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
    (*yaw)->motion = MotionKind::Dynamic;
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

/// A `Height` parameter on the turret, driving the base's and the yaw's placements.
[[nodiscard]] Status expose_height(Document& turret, f32 initial) noexcept {
    const Expected<ExposedParameter*, Error> parameter =
        turret.add_parameter("Height", serialize::WireType::F32);
    if (!parameter) {
        return make_unexpected(parameter.error());
    }
    cy::Transform placement = cy::Transform::from_translation(cy::Vec3{0, initial, 0});
    if (Status kept =
            (*parameter)
                ->default_value()
                .append(Span<const u8>(reinterpret_cast<const u8*>(&placement), sizeof(placement)));
        !kept) {
        return kept;
    }
    // One parameter, three fields on two entities — the specification's "one parameter, many
    // fields" case, at the smallest size that still proves it.
    for (const u32 entity : {2U, 3U}) {
        if (Status bound =
                (*parameter)
                    ->bindings()
                    .push_back(ParameterBinding{LocalId(entity), reflect::TypeId(kPlacementType),
                                                reflect::FieldId(kPlacementLocal)});
            !bound) {
            return bound;
        }
    }
    return ok();
}

[[nodiscard]] Expected<cy::Transform, Error> placement_of(const ResolvedGraph& graph,
                                                          LocalId entity) noexcept {
    const ResolvedEntity* found = graph.find(entity);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "no such entity");
    }
    return read_transform_of(*found, TransformBinding{reflect::TypeId(kPlacementType),
                                                      reflect::FieldId(kPlacementLocal)});
}

[[nodiscard]] Expected<f32, Error> maximum_of(const ResolvedGraph& graph, LocalId entity) noexcept {
    const ResolvedEntity* found = graph.find(entity);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "no such entity");
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

/// `Override` for a field, ready to add.
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

}  // namespace

CY_TEST_CASE("an instance stores only its differences and the resolve applies them") {
    Document turret(test_allocator());
    CY_REQUIRE(build_turret(turret).has_value());

    Document scene(test_allocator());
    scene.kind = AssetKind::Scene;
    scene.id = asset(kSceneAsset);
    const Expected<Instance*, Error> instance =
        scene.add_instance(asset(kTurretAsset), kNoLocalId, "TurretA");
    CY_REQUIRE(instance.has_value());

    const f32 maximum = 400.0F;
    Expected<Override, Error> item =
        field_override(scene.allocator(), LocalId(1), kHealthType, kHealthMaximum, &maximum,
                       sizeof(maximum), serialize::WireType::F32);
    CY_REQUIRE(item.has_value());
    CY_REQUIRE((*instance)->overrides().add(std::move(item.value())).has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(turret).has_value());
    CY_REQUIRE(library.add(scene).has_value());

    ResolveOptions options;
    options.transform =
        TransformBinding{reflect::TypeId(kPlacementType), reflect::FieldId(kPlacementLocal)};
    ResolveReport report(test_allocator());
    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(populate_mapping(library, scene, *instance.value(), options).has_value());
    CY_REQUIRE(resolve(library, asset(kSceneAsset), options, graph, report).has_value());

    // The scene file records one property override, not a copy of the subtree.
    CY_CHECK_EQ((*instance)->overrides().size(), 1U);
    CY_CHECK_EQ(graph.entities().size(), 3U);
    CY_CHECK_EQ(report.overrides_applied, 1U);

    const LocalId base = (*instance)->local_of(LocalId(1));
    const Expected<f32, Error> value = maximum_of(graph, base);
    CY_REQUIRE(value.has_value());
    CY_CHECK_EQ(value.value(), 400.0F);

    // And the provenance says where it came from, and what it would have been.
    const Provenance* provenance =
        graph.provenance_of(base, reflect::TypeId(kHealthType), reflect::FieldId(kHealthMaximum));
    CY_REQUIRE(provenance != nullptr);
    CY_CHECK_EQ(provenance->source, ValueSource::Instance);
    CY_CHECK(provenance->overridden);
    const Span<const u8> inherited =
        graph.inherited_value(base, reflect::TypeId(kHealthType), reflect::FieldId(kHealthMaximum));
    CY_REQUIRE_EQ(inherited.size(), 4U);
    f32 was = 0.0F;
    CY_REQUIRE(
        serialize::decode_scalar(serialize::WireType::F32, inherited.data(), 4, &was).has_value());
    CY_CHECK_EQ(was, 100.0F);
}

CY_TEST_CASE("an edit to the prefab reaches every instance that does not override it") {
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

    Library library(test_allocator());
    CY_REQUIRE(library.add(turret).has_value());
    CY_REQUIRE(library.add(scene).has_value());
    ResolveOptions options;
    CY_REQUIRE(populate_mapping(library, scene, *first.value(), options).has_value());
    CY_REQUIRE(populate_mapping(library, scene, *second.value(), options).has_value());

    // Edit the prefab.
    DocumentEntity* base = turret.find_entity(LocalId(1));
    CY_REQUIRE(base != nullptr);
    CY_REQUIRE(set_health(*base, 555.0F, 100.0F).has_value());

    ResolveReport report(test_allocator());
    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(resolve(library, asset(kSceneAsset), options, graph, report).has_value());

    for (const Instance* instance : {first.value(), second.value()}) {
        const Expected<f32, Error> value = maximum_of(graph, instance->local_of(LocalId(1)));
        CY_REQUIRE(value.has_value());
        CY_CHECK_EQ(value.value(), 555.0F);
    }
}

CY_TEST_CASE("an override whose component is gone becomes a conflict, retained and resolvable") {
    Document turret(test_allocator());
    CY_REQUIRE(build_turret(turret).has_value());

    Document scene(test_allocator());
    scene.kind = AssetKind::Scene;
    scene.id = asset(kSceneAsset);
    const Expected<Instance*, Error> instance =
        scene.add_instance(asset(kTurretAsset), kNoLocalId, "A");
    CY_REQUIRE(instance.has_value());
    const f32 maximum = 400.0F;
    Expected<Override, Error> item =
        field_override(scene.allocator(), LocalId(1), kHealthType, kHealthMaximum, &maximum,
                       sizeof(maximum), serialize::WireType::F32);
    CY_REQUIRE(item.has_value());
    CY_REQUIRE((*instance)->overrides().add(std::move(item.value())).has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(turret).has_value());
    CY_REQUIRE(library.add(scene).has_value());
    ResolveOptions options;
    CY_REQUIRE(populate_mapping(library, scene, *instance.value(), options).has_value());

    // The component the override targeted is deleted from the prefab.
    DocumentEntity* base = turret.find_entity(LocalId(1));
    CY_REQUIRE(base != nullptr);
    CY_CHECK(base->remove(reflect::TypeId(kHealthType)));

    ResolveReport report(test_allocator());
    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(resolve(library, asset(kSceneAsset), options, graph, report).has_value());

    // Retained, surfaced, and not dropped — in any configuration.
    CY_REQUIRE_EQ(report.conflicts.size(), 1U);
    CY_CHECK_EQ(report.conflicts[0].kind, ConflictKind::MissingComponent);
    CY_REQUIRE_EQ((*instance)->overrides().size(), 1U);
    CY_CHECK((*instance)->overrides()[0].conflicted());
    CY_CHECK_EQ((*instance)->overrides().conflict_count(), 1U);

    // Restoring the structure resolves it, and the override applies again.
    CY_REQUIRE(set_health(*base, 100.0F, 100.0F).has_value());
    ResolveReport again(test_allocator());
    ResolvedGraph second(test_allocator());
    CY_REQUIRE(resolve(library, asset(kSceneAsset), options, second, again).has_value());
    CY_CHECK(again.conflicts.empty());
    CY_CHECK_FALSE((*instance)->overrides()[0].conflicted());
}

CY_TEST_CASE("a variant inherits a later change to its base") {
    Document turret(test_allocator());
    CY_REQUIRE(build_turret(turret).has_value());

    Document variant(test_allocator());
    variant.kind = AssetKind::Prefab;
    variant.id = asset(kVariantAsset);
    variant.set_base(asset(kTurretAsset));

    Library library(test_allocator());
    CY_REQUIRE(library.add(turret).has_value());
    CY_REQUIRE(library.add(variant).has_value());
    ResolveOptions options;
    CY_REQUIRE(populate_base_mapping(library, variant, options).has_value());

    // The variant overrides nothing, so a change to the base reaches it.
    DocumentEntity* base = turret.find_entity(LocalId(1));
    CY_REQUIRE(base != nullptr);
    CY_REQUIRE(set_health(*base, 777.0F, 100.0F).has_value());

    ResolveReport report(test_allocator());
    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(resolve(library, asset(kVariantAsset), options, graph, report).has_value());

    const LocalId mapped = mapped_local(variant.base_mapping().span(), LocalId(1));
    const Expected<f32, Error> value = maximum_of(graph, mapped);
    CY_REQUIRE(value.has_value());
    CY_CHECK_EQ(value.value(), 777.0F);
}

CY_TEST_CASE("a variant chain deeper than the recommendation warns, naming the chain") {
    Document root(test_allocator());
    root.kind = AssetKind::Prefab;
    root.id = asset(100);

    Document a(test_allocator());
    a.kind = AssetKind::Prefab;
    a.id = asset(101);
    a.set_base(asset(100));
    Document b(test_allocator());
    b.kind = AssetKind::Prefab;
    b.id = asset(102);
    b.set_base(asset(101));
    Document c(test_allocator());
    c.kind = AssetKind::Prefab;
    c.id = asset(103);
    c.set_base(asset(102));
    Document d(test_allocator());
    d.kind = AssetKind::Prefab;
    d.id = asset(104);
    d.set_base(asset(103));

    Library library(test_allocator());
    for (Document* document : {&root, &a, &b, &c, &d}) {
        CY_REQUIRE(library.add(*document).has_value());
    }

    Array<AssetId> chain(test_allocator());
    const Expected<u32, Error> depth = library.variant_depth(asset(104), chain);
    CY_REQUIRE(depth.has_value());
    CY_CHECK_EQ(depth.value(), 4U);
    CY_CHECK_GT(depth.value(), library.recommended_variant_depth());
    CY_CHECK_EQ(chain.size(), 5U);

    const Expected<bool, Error> warned =
        library.variant_depth_exceeds_recommendation(asset(104), chain);
    CY_REQUIRE(warned.has_value());
    CY_CHECK(warned.value());
    // A chain within the recommendation does not warn.
    const Expected<bool, Error> quiet =
        library.variant_depth_exceeds_recommendation(asset(101), chain);
    CY_REQUIRE(quiet.has_value());
    CY_CHECK_FALSE(quiet.value());
}

CY_TEST_CASE("one parameter drives many fields, and an instance sets it by identifier") {
    Document turret(test_allocator());
    CY_REQUIRE(build_turret(turret).has_value());
    CY_REQUIRE(expose_height(turret, 1.0F).has_value());

    Document scene(test_allocator());
    scene.kind = AssetKind::Scene;
    scene.id = asset(kSceneAsset);
    const Expected<Instance*, Error> instance =
        scene.add_instance(asset(kTurretAsset), kNoLocalId, "A");
    CY_REQUIRE(instance.has_value());

    // The argument carries the resolved identifier and no name at all: "parameters SHALL be applied
    // by resolved identifier, not by name lookup".
    const ExposedParameter* declared = turret.find_parameter("Height");
    CY_REQUIRE(declared != nullptr);
    ParameterArgument argument;
    argument.id = declared->id;
    argument.wire = serialize::WireType::Bytes;
    argument.value = Array<u8>(test_allocator());
    const cy::Transform tall = cy::Transform::from_translation(cy::Vec3{0, 3.5F, 0});
    CY_REQUIRE(
        argument.value.append(Span<const u8>(reinterpret_cast<const u8*>(&tall), sizeof(tall)))
            .has_value());
    CY_REQUIRE((*instance)->arguments().push_back(std::move(argument)).has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(turret).has_value());
    CY_REQUIRE(library.add(scene).has_value());
    ResolveOptions options;
    options.transform =
        TransformBinding{reflect::TypeId(kPlacementType), reflect::FieldId(kPlacementLocal)};
    CY_REQUIRE(populate_mapping(library, scene, *instance.value(), options).has_value());

    ResolveReport report(test_allocator());
    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(resolve(library, asset(kSceneAsset), options, graph, report).has_value());

    // Every field bound to the parameter is updated consistently.
    CY_CHECK_EQ(report.parameters_applied, 2U);
    for (const u32 source : {2U, 3U}) {
        const Expected<cy::Transform, Error> placed =
            placement_of(graph, (*instance)->local_of(LocalId(source)));
        CY_REQUIRE(placed.has_value());
        CY_CHECK_EQ(placed->translation.y, 3.5F);
    }
    // The parameter is the prefab's interface, not the scene's: it is consumed by the placement.
    CY_CHECK(graph.parameters().empty());
}

CY_TEST_CASE("a variant re-defaults a parameter and an instance still overrides it") {
    // The spike's awkward composition, at its smallest: `Height` is declared by the turret,
    // re-defaulted by the variant, and set again by the scene instance.
    Document turret(test_allocator());
    CY_REQUIRE(build_turret(turret).has_value());
    CY_REQUIRE(expose_height(turret, 1.0F).has_value());
    const ExposedParameter* declared = turret.find_parameter("Height");
    CY_REQUIRE(declared != nullptr);
    const ParameterId height = declared->id;

    Document variant(test_allocator());
    variant.kind = AssetKind::Prefab;
    variant.id = asset(kVariantAsset);
    variant.set_base(asset(kTurretAsset));

    Library library(test_allocator());
    CY_REQUIRE(library.add(turret).has_value());
    CY_REQUIRE(library.add(variant).has_value());
    ResolveOptions options;
    options.transform =
        TransformBinding{reflect::TypeId(kPlacementType), reflect::FieldId(kPlacementLocal)};
    CY_REQUIRE(populate_base_mapping(library, variant, options).has_value());

    const auto make_argument = [&](f32 height_value) noexcept {
        ParameterArgument argument;
        argument.id = height;
        argument.wire = serialize::WireType::Bytes;
        argument.value = Array<u8>(test_allocator());
        const cy::Transform placement =
            cy::Transform::from_translation(cy::Vec3{0, height_value, 0});
        (void)argument.value.append(
            Span<const u8>(reinterpret_cast<const u8*>(&placement), sizeof(placement)));
        return argument;
    };

    CY_REQUIRE(variant.base_arguments().push_back(make_argument(2.0F)).has_value());

    // Resolved on its own, the variant shows its re-default.
    {
        ResolveReport report(test_allocator());
        ResolvedGraph graph(test_allocator());
        CY_REQUIRE(resolve(library, asset(kVariantAsset), options, graph, report).has_value());
        const LocalId yaw = mapped_local(variant.base_mapping().span(), LocalId(2));
        const Expected<cy::Transform, Error> placed = placement_of(graph, yaw);
        CY_REQUIRE(placed.has_value());
        CY_CHECK_EQ(placed->translation.y, 2.0F);
    }

    // An instance of the variant sets it again, and the instance wins.
    Document scene(test_allocator());
    scene.kind = AssetKind::Scene;
    scene.id = asset(kSceneAsset);
    const Expected<Instance*, Error> instance =
        scene.add_instance(asset(kVariantAsset), kNoLocalId, "A");
    CY_REQUIRE(instance.has_value());
    CY_REQUIRE((*instance)->arguments().push_back(make_argument(3.5F)).has_value());
    CY_REQUIRE(library.add(scene).has_value());
    CY_REQUIRE(populate_mapping(library, scene, *instance.value(), options).has_value());

    ResolveReport report(test_allocator());
    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(resolve(library, asset(kSceneAsset), options, graph, report).has_value());
    const LocalId yaw =
        (*instance)->local_of(mapped_local(variant.base_mapping().span(), LocalId(2)));
    const Expected<cy::Transform, Error> placed = placement_of(graph, yaw);
    CY_REQUIRE(placed.has_value());
    CY_CHECK_EQ(placed->translation.y, 3.5F);
}

CY_TEST_CASE("a prefab may be refactored internally without repairing its instances") {
    // "Internals stay private": the prefab gains an entity and re-binds the parameter, and an
    // instance that sets the parameter keeps working with no change to the instance.
    Document turret(test_allocator());
    CY_REQUIRE(build_turret(turret).has_value());
    CY_REQUIRE(expose_height(turret, 1.0F).has_value());

    Document scene(test_allocator());
    scene.kind = AssetKind::Scene;
    scene.id = asset(kSceneAsset);
    const Expected<Instance*, Error> instance =
        scene.add_instance(asset(kTurretAsset), kNoLocalId, "A");
    CY_REQUIRE(instance.has_value());
    const ExposedParameter* declared = turret.find_parameter("Height");
    CY_REQUIRE(declared != nullptr);
    ParameterArgument argument;
    argument.id = declared->id;
    argument.wire = serialize::WireType::Bytes;
    argument.value = Array<u8>(test_allocator());
    const cy::Transform tall = cy::Transform::from_translation(cy::Vec3{0, 9.0F, 0});
    CY_REQUIRE(
        argument.value.append(Span<const u8>(reinterpret_cast<const u8*>(&tall), sizeof(tall)))
            .has_value());
    CY_REQUIRE((*instance)->arguments().push_back(std::move(argument)).has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(turret).has_value());
    CY_REQUIRE(library.add(scene).has_value());
    ResolveOptions options;
    options.transform =
        TransformBinding{reflect::TypeId(kPlacementType), reflect::FieldId(kPlacementLocal)};
    CY_REQUIRE(populate_mapping(library, scene, *instance.value(), options).has_value());

    // Refactor: a new internal entity, and the parameter re-bound onto it as well.
    const Expected<DocumentEntity*, Error> mount = turret.add_entity(LocalId(1), "Mount");
    CY_REQUIRE(mount.has_value());
    CY_REQUIRE(set_placement(**mount, cy::Transform::identity()).has_value());
    ExposedParameter* parameter = turret.find_parameter(declared->id);
    CY_REQUIRE(parameter != nullptr);
    CY_REQUIRE(parameter->bindings()
                   .push_back(ParameterBinding{(*mount)->id, reflect::TypeId(kPlacementType),
                                               reflect::FieldId(kPlacementLocal)})
                   .has_value());

    ResolveReport report(test_allocator());
    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(resolve(library, asset(kSceneAsset), options, graph, report).has_value());
    // The instance was not repaired and the new field is driven anyway.
    CY_CHECK_EQ(report.parameters_applied, 3U);
}

CY_TEST_CASE("a diff between two resolves reports structure rather than a text delta") {
    Document turret(test_allocator());
    CY_REQUIRE(build_turret(turret).has_value());
    Library library(test_allocator());
    CY_REQUIRE(library.add(turret).has_value());

    ResolveOptions options;
    ResolveReport before_report(test_allocator());
    ResolvedGraph before(test_allocator());
    CY_REQUIRE(resolve(library, asset(kTurretAsset), options, before, before_report).has_value());

    DocumentEntity* base = turret.find_entity(LocalId(1));
    CY_REQUIRE(base != nullptr);
    CY_REQUIRE(set_health(*base, 300.0F, 100.0F).has_value());
    const Expected<DocumentEntity*, Error> extra = turret.add_entity(LocalId(1), "Sensor");
    CY_REQUIRE(extra.has_value());

    ResolveReport after_report(test_allocator());
    ResolvedGraph after(test_allocator());
    CY_REQUIRE(resolve(library, asset(kTurretAsset), options, after, after_report).has_value());

    Array<GraphDifference> differences(test_allocator());
    CY_REQUIRE(diff(before, after, differences).has_value());

    bool saw_field = false;
    bool saw_entity = false;
    for (const GraphDifference& change : differences) {
        saw_field = saw_field || (change.kind == GraphDifference::Kind::FieldChanged &&
                                  change.field == reflect::FieldId(kHealthMaximum));
        saw_entity = saw_entity || (change.kind == GraphDifference::Kind::EntityAdded &&
                                    change.entity == (*extra)->id);
    }
    CY_CHECK(saw_field);
    CY_CHECK(saw_entity);
}
