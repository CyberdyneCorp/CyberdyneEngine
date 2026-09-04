// Cooking: cycles rejected, hierarchy flattened by the walk-to-root rule, archetype blocks emitted.

#include <cy/scene/serialization/cook.h>
#include <cy/test/test.h>

#include <cstring>

#include "fixtures.h"

using namespace cy;
using namespace cy::scene::serialization;
using namespace cy::scene::serialization::test;

namespace {

[[nodiscard]] TransformBinding placement_binding() noexcept {
    return TransformBinding{reflect::TypeId(kPlacementType), reflect::FieldId(kPlacementLocal)};
}

/// A world with the fixture components, so a layout table can be built from it.
[[nodiscard]] Status make_world(ecs::World& world) noexcept {
    if (Status started = world.initialize(); !started) {
        return started;
    }
    return register_fixture_components(world);
}

/// A resolved graph built directly, for the cases that are about cooking rather than resolving.
[[nodiscard]] Expected<ResolvedEntity*, Error> add(ResolvedGraph& graph, u32 id, u32 parent,
                                                   MotionKind motion,
                                                   const cy::Transform& local) noexcept {
    Expected<ResolvedEntity*, Error> entity = graph.add(LocalId(id));
    if (!entity) {
        return entity;
    }
    (*entity)->parent = LocalId(parent);
    (*entity)->motion = motion;
    (*entity)->origin = asset(1);
    (*entity)->origin_local = LocalId(id);
    if (Status placed = write_transform_of(**entity, placement_binding(), local, asset(1),
                                           ValueSource::Base, graph.allocator());
        !placed) {
        return make_unexpected(placed.error());
    }
    return entity;
}

}  // namespace

CY_TEST_CASE("placing a prefab inside itself is rejected when the placement is attempted") {
    Document a(test_allocator());
    a.kind = AssetKind::Prefab;
    a.id = asset(1);
    Document b(test_allocator());
    b.kind = AssetKind::Prefab;
    b.id = asset(2);
    const Expected<Instance*, Error> inner = b.add_instance(asset(1), kNoLocalId, "a-in-b");
    CY_REQUIRE(inner.has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(a).has_value());
    CY_REQUIRE(library.add(b).has_value());

    Array<AssetId> chain(test_allocator());
    // A already contains nothing; putting B inside A would close A -> B -> A.
    const Status refused = library.check_placement(a.id, b.id, chain);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::AlreadyExists);
    CY_CHECK_GE(chain.size(), 2U);

    // The direct case is refused too, and reported as a chain rather than as a bare refusal.
    Array<AssetId> self(test_allocator());
    CY_CHECK_FALSE(library.check_placement(a.id, a.id, self).has_value());
    CY_CHECK_EQ(self.size(), 2U);

    // And a placement that closes nothing is allowed.
    Document c(test_allocator());
    c.kind = AssetKind::Prefab;
    c.id = asset(3);
    CY_REQUIRE(library.add(c).has_value());
    CY_CHECK(library.check_placement(a.id, c.id, chain).has_value());
}

CY_TEST_CASE("a cycle already in the graph is reported by validation as a chain") {
    Document a(test_allocator());
    a.kind = AssetKind::Prefab;
    a.id = asset(1);
    Document b(test_allocator());
    b.kind = AssetKind::Prefab;
    b.id = asset(2);
    CY_REQUIRE(a.add_instance(asset(2), kNoLocalId, "b").has_value());
    CY_REQUIRE(b.add_instance(asset(1), kNoLocalId, "a").has_value());

    Library library(test_allocator());
    CY_REQUIRE(library.add(a).has_value());
    CY_REQUIRE(library.add(b).has_value());

    Array<AssetId> chain(test_allocator());
    const Status validated = library.validate(chain);
    CY_REQUIRE_FALSE(validated.has_value());
    CY_CHECK_GE(chain.size(), 3U);
    CY_CHECK_EQ(chain[0], chain[chain.size() - 1]);
}

CY_TEST_CASE(
    "organisational nesting costs nothing: the relationship goes and the transform bakes") {
    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(
        add(graph, 1, 0, MotionKind::Static, cy::Transform::from_translation(cy::Vec3{10, 0, 0}))
            .has_value());
    CY_REQUIRE(
        add(graph, 2, 1, MotionKind::Static, cy::Transform::from_translation(cy::Vec3{0, 5, 0}))
            .has_value());
    CY_REQUIRE(
        add(graph, 3, 2, MotionKind::Static, cy::Transform::from_translation(cy::Vec3{0, 0, 2}))
            .has_value());

    CookReport report(test_allocator());
    CY_REQUIRE(flatten_hierarchy(graph, placement_binding(), report).has_value());

    CY_CHECK_EQ(report.relationships_flattened, 2U);
    CY_CHECK_EQ(report.relationships_retained, 0U);
    for (const ResolvedEntity& entity : graph.entities()) {
        CY_CHECK_FALSE(entity.parent.valid());
    }
    const Expected<cy::Transform, Error> deepest =
        read_transform_of(*graph.find(LocalId(3)), placement_binding());
    CY_REQUIRE(deepest.has_value());
    CY_CHECK_EQ(deepest->translation.x, 10.0F);
    CY_CHECK_EQ(deepest->translation.y, 5.0F);
    CY_CHECK_EQ(deepest->translation.z, 2.0F);
}

CY_TEST_CASE("a moving part keeps its hierarchy") {
    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(add(graph, 1, 0, MotionKind::Static, cy::Transform::identity()).has_value());
    CY_REQUIRE(add(graph, 2, 1, MotionKind::Dynamic, cy::Transform::identity()).has_value());

    CookReport report(test_allocator());
    CY_REQUIRE(flatten_hierarchy(graph, placement_binding(), report).has_value());
    CY_CHECK_EQ(report.relationships_retained, 1U);
    CY_CHECK_EQ(report.relationships_flattened, 0U);
    CY_CHECK(graph.find(LocalId(2))->parent.valid());
}

CY_TEST_CASE("the flattening test is a walk to the root, not a look at one edge") {
    // The spike's trap. A static muzzle under a static-looking chain whose *yaw* rotates: a
    // per-edge test flattens the muzzle out from under the yaw and the failure is visible only in
    // motion.
    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(add(graph, 1, 0, MotionKind::Static, cy::Transform::identity()).has_value());
    CY_REQUIRE(
        add(graph, 2, 1, MotionKind::Dynamic, cy::Transform::identity()).has_value());  // yaw
    CY_REQUIRE(
        add(graph, 3, 2, MotionKind::Static, cy::Transform::identity()).has_value());  // barrel
    CY_REQUIRE(
        add(graph, 4, 3, MotionKind::Static, cy::Transform::identity()).has_value());  // muzzle

    CookReport report(test_allocator());
    CY_REQUIRE(flatten_hierarchy(graph, placement_binding(), report).has_value());

    // Three edges, all retained: the yaw's because it moves, and the two above the barrel and the
    // muzzle because the yaw does.
    CY_CHECK_EQ(report.relationships_retained, 3U);
    CY_CHECK_EQ(report.relationships_flattened, 0U);
    CY_CHECK(graph.find(LocalId(4))->parent.valid());
}

CY_TEST_CASE("the per-entity policy overrides the analysis in both directions") {
    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(add(graph, 1, 0, MotionKind::Static, cy::Transform::identity()).has_value());
    const Expected<ResolvedEntity*, Error> welded =
        add(graph, 2, 1, MotionKind::Dynamic, cy::Transform::identity());
    CY_REQUIRE(welded.has_value());
    (*welded)->flatten = FlattenPolicy::Flatten;
    const Expected<ResolvedEntity*, Error> kept =
        add(graph, 3, 1, MotionKind::Static, cy::Transform::identity());
    CY_REQUIRE(kept.has_value());
    (*kept)->flatten = FlattenPolicy::Keep;

    CookReport report(test_allocator());
    CY_REQUIRE(flatten_hierarchy(graph, placement_binding(), report).has_value());
    CY_CHECK_EQ(report.relationships_flattened, 1U);
    CY_CHECK_EQ(report.relationships_retained, 1U);
}

CY_TEST_CASE("a reference outside the graph is nulled and counted, not left dangling") {
    ResolvedGraph graph(test_allocator());
    const Expected<ResolvedEntity*, Error> entity =
        add(graph, 1, 0, MotionKind::Static, cy::Transform::identity());
    CY_REQUIRE(entity.has_value());
    const Expected<ResolvedComponent*, Error> component =
        ensure_resolved_component(**entity, reflect::TypeId(kTargetType), graph.allocator());
    CY_REQUIRE(component.has_value());
    CY_REQUIRE((*component)
                   ->record.set_local_reference(reflect::FieldId(kTargetEntity), 4242)
                   .has_value());

    CookReport report(test_allocator());
    CY_REQUIRE(validate_references(graph, report).has_value());
    CY_CHECK_EQ(report.dangling_references, 1U);
    const Expected<u32, Error> nulled =
        (*component)->record.local_reference(reflect::FieldId(kTargetEntity));
    CY_REQUIRE(nulled.has_value());
    CY_CHECK_EQ(nulled.value(), 0U);
}

CY_TEST_CASE("a cook emits archetype blocks with no per-field tags and no names") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"cook", 16384});
    CY_REQUIRE(make_world(world).has_value());
    ComponentLayoutTable layouts(test_allocator());
    CY_REQUIRE(describe_from_world(world, layouts).has_value());

    ResolvedGraph graph(test_allocator());
    // Two archetypes: one entity with a placement only, two with a placement and health.
    CY_REQUIRE(add(graph, 1, 0, MotionKind::Static, cy::Transform::identity()).has_value());
    for (u32 id : {2U, 3U}) {
        const Expected<ResolvedEntity*, Error> entity =
            add(graph, id, 0, MotionKind::Static, cy::Transform::identity());
        CY_REQUIRE(entity.has_value());
        const Expected<ResolvedComponent*, Error> health =
            ensure_resolved_component(**entity, reflect::TypeId(kHealthType), graph.allocator());
        CY_REQUIRE(health.has_value());
        const f32 maximum = 42.0F;
        CY_REQUIRE(
            (*health)
                ->record
                .set_scalar(reflect::FieldId(kHealthMaximum), serialize::WireType::F32, &maximum, 4)
                .has_value());
    }

    CookOptions options;
    options.layouts = &layouts;
    options.resolve.transform = placement_binding();
    CookedAsset cooked(test_allocator());
    CookReport report(test_allocator());
    CY_REQUIRE(cook_resolved(graph, options, cooked, report).has_value());

    CY_CHECK_EQ(report.blocks, 2U);
    CY_CHECK_EQ(report.entities, 3U);
    CY_CHECK_EQ(cooked.entity_count, 3U);
    // The payload is exactly the columns: one Placement per entity plus one Health for two of them.
    CY_CHECK_EQ(report.payload_bytes, (3U * sizeof(Placement)) + (2U * sizeof(Health)));
    // No editor-only data reached the runtime form.
    CY_CHECK(cooked.stream().size() > 0U);
    CY_CHECK_EQ(cooked.row_indices().size(), 3U);
}

CY_TEST_CASE("a cook emits a reference site for every declared entity field") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"cook", 16384});
    CY_REQUIRE(make_world(world).has_value());
    ComponentLayoutTable layouts(test_allocator());
    CY_REQUIRE(describe_from_world(world, layouts).has_value());

    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(add(graph, 1, 0, MotionKind::Static, cy::Transform::identity()).has_value());
    const Expected<ResolvedEntity*, Error> pointer =
        add(graph, 2, 0, MotionKind::Static, cy::Transform::identity());
    CY_REQUIRE(pointer.has_value());
    const Expected<ResolvedComponent*, Error> target =
        ensure_resolved_component(**pointer, reflect::TypeId(kTargetType), graph.allocator());
    CY_REQUIRE(target.has_value());
    CY_REQUIRE(
        (*target)->record.set_local_reference(reflect::FieldId(kTargetEntity), 1).has_value());

    CookOptions options;
    options.layouts = &layouts;
    options.resolve.transform = placement_binding();
    CookedAsset cooked(test_allocator());
    CookReport report(test_allocator());
    CY_REQUIRE(cook_resolved(graph, options, cooked, report).has_value());
    CY_CHECK_EQ(report.reference_sites, 1U);

    // And the slot holds a cook-time index, not an entity id: template index 0 plus the bias.
    serialize::CookedReader reader(cooked.stream().data(), cooked.stream().size());
    CY_REQUIRE(reader.read_header(build_schema_of(layouts)).has_value());
    bool found = false;
    serialize::CookedBlock block(test_allocator());
    while (reader.next_block(block).has_value()) {
        for (const serialize::ReferenceSite& site : block.reference_sites()) {
            const Expected<Span<const u8>, Error> bytes = block.column_bytes(site.column);
            CY_REQUIRE(bytes.has_value());
            u64 slot = 0;
            std::memcpy(&slot, bytes->data() + site.offset, sizeof(slot));
            CY_CHECK_EQ(slot, 1U);  // index 0, biased by one so that zero can mean "no entity"
            found = true;
        }
    }
    CY_CHECK(found);
}

CY_TEST_CASE("a shipping cook carries no provenance and a development cook does") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"cook", 16384});
    CY_REQUIRE(make_world(world).has_value());
    ComponentLayoutTable layouts(test_allocator());
    CY_REQUIRE(describe_from_world(world, layouts).has_value());

    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(add(graph, 1, 0, MotionKind::Static, cy::Transform::identity()).has_value());

    CookOptions development;
    development.layouts = &layouts;
    development.resolve.transform = placement_binding();
    CookedAsset with_provenance(test_allocator());
    CookReport first(test_allocator());
    CY_REQUIRE(cook_resolved(graph, development, with_provenance, first).has_value());
    CY_CHECK_EQ(with_provenance.origins().size(), 1U);

    ResolvedGraph again(test_allocator());
    CY_REQUIRE(add(again, 1, 0, MotionKind::Static, cy::Transform::identity()).has_value());
    CookOptions shipping = development;
    shipping.retain_provenance = false;
    CookedAsset stripped(test_allocator());
    CookReport second(test_allocator());
    CY_REQUIRE(cook_resolved(again, shipping, stripped, second).has_value());
    CY_CHECK(stripped.origins().empty());
}

CY_TEST_CASE("a cook configured to fail on conflicts does, and one that is not does not") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"cook", 16384});
    CY_REQUIRE(make_world(world).has_value());
    ComponentLayoutTable layouts(test_allocator());
    CY_REQUIRE(describe_from_world(world, layouts).has_value());

    ResolvedGraph graph(test_allocator());
    CY_REQUIRE(add(graph, 1, 0, MotionKind::Static, cy::Transform::identity()).has_value());

    CookOptions options;
    options.layouts = &layouts;
    options.resolve.transform = placement_binding();
    options.fail_on_conflicts = true;

    CookReport report(test_allocator());
    OverrideTarget target;
    CY_REQUIRE(
        report.resolve.conflicts
            .push_back(ConflictReport{asset(1), kNoLocalId, 0, ConflictKind::MissingEntity, target})
            .has_value());

    CookedAsset cooked(test_allocator());
    const Status failed = cook_resolved(graph, options, cooked, report);
    CY_REQUIRE_FALSE(failed.has_value());
    CY_CHECK_EQ(failed.error().code, ErrorCode::InvalidArgument);
}

CY_TEST_CASE("a component with no layout is ignored rather than carried") {
    ecs::World world(test_allocator(), ecs::WorldConfig{"cook", 16384});
    CY_REQUIRE(make_world(world).has_value());
    ComponentLayoutTable layouts(test_allocator());
    CY_REQUIRE(describe_from_world(world, layouts).has_value());

    ResolvedGraph graph(test_allocator());
    const Expected<ResolvedEntity*, Error> entity =
        add(graph, 1, 0, MotionKind::Static, cy::Transform::identity());
    CY_REQUIRE(entity.has_value());
    const Expected<ResolvedComponent*, Error> plugin =
        ensure_resolved_component(**entity, reflect::TypeId(777001), graph.allocator());
    CY_REQUIRE(plugin.has_value());

    CookOptions options;
    options.layouts = &layouts;
    options.resolve.transform = placement_binding();
    CookedAsset cooked(test_allocator());
    CookReport report(test_allocator());
    CY_REQUIRE(cook_resolved(graph, options, cooked, report).has_value());
    CY_CHECK_EQ(report.unknown_components, 1U);
    CY_CHECK_EQ(report.blocks, 1U);
}
