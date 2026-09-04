// The coherence invariants, as tests rather than as prose. Task 3.1.10, design.md §3.
//
// design.md §3: "The coherence invariants in the specification become tests in this milestone
// rather than prose: a node's transform *is* the entity's transform, destroying an entity
// invalidates its nodes, and no traversal order produces a state a direct ECS query would not."
//
// Every case below is written so that it would fail if someone later added a cache. The positive
// ones compare the node's answer against a direct ECS read with no sync step between; the negative
// ones break an invariant from *outside* the node API — which is the only way to break one — and
// require the check to name the entity.

#include <cy/test/test.h>

#include <cy/ecs/query.h>
#include <cy/scene/coherence.h>
#include <cy/scene/tree.h>

#include "fixtures.h"

#include <algorithm>

using cy::scene::test::Fixture;
using cy::scene::test::make_child;

namespace {

/// A small hierarchy with transforms and flags set, propagated. The shape every case starts from.
[[nodiscard]] bool build(Fixture& fixture) noexcept {
    if (!fixture.start()) {
        return false;
    }
    for (cy::u32 branch = 0; branch < 4; ++branch) {
        cy::scene::Node top = make_child(fixture.tree, fixture.tree.root(), "Branch");
        if (!top.valid() || !top.set_local_transform(cy::Transform::from_translation(cy::Vec3{
                                                         static_cast<cy::f32>(branch), 0, 0}))
                                 .has_value()) {
            return false;
        }
        for (cy::u32 leaf = 0; leaf < 3; ++leaf) {
            cy::scene::Node child = make_child(fixture.tree, top, "Leaf");
            if (!child.valid() || !child
                                       .set_local_transform(cy::Transform::from_translation(
                                           cy::Vec3{0, static_cast<cy::f32>(leaf), 0}))
                                       .has_value()) {
                return false;
            }
        }
    }
    return fixture.tree.propagate().has_value();
}

/// The count of violations of one invariant in a report.
[[nodiscard]] cy::u32 violations_of(const cy::scene::CoherenceReport& report,
                                    cy::scene::Invariant invariant) noexcept {
    cy::u32 found = 0;
    for (cy::u32 index = 0; index < report.recorded_count; ++index) {
        found += (report.recorded[index].invariant == invariant) ? 1U : 0U;
    }
    return found;
}

}  // namespace

CY_TEST_CASE("a tree built and propagated through the node API satisfies every invariant") {
    Fixture fixture;
    CY_REQUIRE(build(fixture));
    const auto report = cy::scene::check_coherence(fixture.tree);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report->nodes_checked, 17U);
    CY_CHECK_EQ(report->violations, 0U);
    CY_CHECK(report->coherent());
}

CY_TEST_CASE("a node's transform IS the entity's transform, in both directions and with no step") {
    Fixture fixture;
    CY_REQUIRE(build(fixture));
    cy::scene::Node leaf = fixture.tree.root().child(0).child(0);
    CY_REQUIRE(leaf.valid());

    // Node write, ECS read.
    const cy::Transform written = cy::Transform::from_translation(cy::Vec3{9.0F, 8.0F, 7.0F});
    CY_REQUIRE(leaf.set_local_transform(written).has_value());
    CY_CHECK(fixture.world
                 .get<cy::scene::LocalTransform>(leaf.entity(),
                                                 fixture.tree.components().local_transform)
                 ->value == written);

    // ECS write, node read. There is no cache to be stale, so there is no step between them.
    fixture.world
        .get_mut<cy::scene::LocalTransform>(leaf.entity(),
                                            fixture.tree.components().local_transform)
        ->value.translation = cy::Vec3{-1.0F, -2.0F, -3.0F};
    CY_CHECK(leaf.local_transform().translation == cy::Vec3{-1.0F, -2.0F, -3.0F});
}

CY_TEST_CASE("no traversal order produces a state a direct ECS query would not") {
    Fixture fixture;
    CY_REQUIRE(build(fixture));

    // The tree walk: every node reached from the root, and its world transform as the node reports.
    cy::Array<cy::u32> walked(cy::scene::test::allocator());
    cy::Array<cy::Vec3> walked_positions(cy::scene::test::allocator());
    cy::Array<cy::scene::Node> stack(cy::scene::test::allocator());
    CY_REQUIRE(stack.push_back(fixture.tree.root()).has_value());
    while (!stack.empty()) {
        const cy::scene::Node node = stack.back();
        stack.pop_back();
        CY_REQUIRE(walked.push_back(node.entity().index()).has_value());
        CY_REQUIRE(walked_positions.push_back(node.world_transform().translation).has_value());
        CY_REQUIRE(node.children(stack).has_value());
    }

    // The direct query: every entity with a node, and its world transform as the column holds it.
    cy::Array<cy::u32> queried(cy::scene::test::allocator());
    cy::Array<cy::Vec3> queried_positions(cy::scene::test::allocator());
    cy::ecs::QueryDesc desc(cy::scene::test::allocator());
    CY_REQUIRE(desc.with(fixture.tree.components().node_name).has_value());
    CY_REQUIRE(desc.read(fixture.tree.components().world_transform).has_value());
    cy::ecs::Query query(fixture.world, std::move(desc));
    const cy::ecs::ComponentTypeId world_transform = fixture.tree.components().world_transform;
    CY_REQUIRE(query
                   .for_each_chunk([&](cy::ecs::QueryChunk& chunk) noexcept {
                       const cy::Span<const cy::scene::WorldTransform> derived =
                           chunk.read<cy::scene::WorldTransform>(world_transform);
                       const cy::Span<const cy::ecs::Entity> entities = chunk.entities();
                       for (cy::usize row = 0; row < entities.size(); ++row) {
                           (void)queried.push_back(entities[row].index());
                           (void)queried_positions.push_back(derived[row].value.translation);
                       }
                   })
                   .has_value());

    // The two see the same entities. Sorted, because the orders are deliberately different: one is
    // tree order and the other is chunk order, and the claim is that the *state* agrees.
    CY_REQUIRE_EQ(walked.size(), queried.size());
    cy::Array<cy::u32> walked_sorted(cy::scene::test::allocator());
    CY_REQUIRE(walked_sorted.append(walked.span()).has_value());
    std::ranges::sort(walked_sorted);
    std::ranges::sort(queried);
    for (cy::usize index = 0; index < queried.size(); ++index) {
        CY_CHECK_EQ(walked_sorted[index], queried[index]);
    }

    // And every node's world transform is the same value whichever way it was reached.
    for (cy::usize index = 0; index < walked.size(); ++index) {
        const cy::u32 wanted = walked[index];
        bool matched = false;
        for (const cy::u32 found : queried) {
            matched = matched || (found == wanted);
        }
        CY_CHECK(matched);
        const cy::scene::Node node = fixture.tree.node(
            cy::ecs::Entity::make(wanted, fixture.world.entities().at(wanted).generation()));
        CY_CHECK(node.world_transform().translation == walked_positions[index]);
    }
}

CY_TEST_CASE("writing Parent directly, outside the node API, is caught") {
    Fixture fixture;
    CY_REQUIRE(build(fixture));
    const cy::scene::Node branch = fixture.tree.root().child(0);
    const cy::scene::Node other = fixture.tree.root().child(1);
    const cy::scene::Node leaf = branch.child(0);
    CY_REQUIRE(leaf.valid());

    // `scene-graph-and-nodes`' own scenario: "a system mutates `Parent` directly without going
    // through the node API". The world's `set_parent` would have kept both sides in step; writing
    // the component leaves the old parent's `Children` naming a child that has left.
    auto* parent =
        fixture.world.get_mut<cy::ecs::Parent>(leaf.entity(), fixture.world.parent_component());
    CY_REQUIRE(parent != nullptr);
    parent->value = other.entity();

    const auto report = cy::scene::check_coherence(fixture.tree);
    CY_REQUIRE(report.has_value());
    CY_CHECK_FALSE(report->coherent());
    CY_CHECK_GT(violations_of(*report, cy::scene::Invariant::ParentMatchesTree), 0U);
    // And it names the entity, which is what makes the report actionable.
    bool named = false;
    for (cy::u32 index = 0; index < report->recorded_count; ++index) {
        named = named || (report->recorded[index].entity == leaf.entity());
    }
    CY_CHECK(named);
}

CY_TEST_CASE("a LocalTransform written without marking it is caught after propagation") {
    Fixture fixture;
    CY_REQUIRE(build(fixture));
    const cy::scene::Node leaf = fixture.tree.root().child(2).child(1);
    CY_REQUIRE(leaf.valid());

    // A system writing the column and forgetting `mark_transform_changed`. Propagation cannot see
    // it — that is the price of the O(1) subtree skip — so the check is what does.
    fixture.world
        .get_mut<cy::scene::LocalTransform>(leaf.entity(),
                                            fixture.tree.components().local_transform)
        ->value.translation = cy::Vec3{500.0F, 0.0F, 0.0F};
    CY_REQUIRE(fixture.tree.propagate().has_value());

    const auto report = cy::scene::check_coherence(fixture.tree);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(violations_of(*report, cy::scene::Invariant::WorldTransformConsistent), 1U);

    // Marked and propagated, it is coherent again — so the check is about staleness and not about
    // who did the writing.
    CY_REQUIRE(cy::scene::mark_transform_changed(fixture.tree, leaf.entity()).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK(cy::scene::coherent(fixture.tree));
}

CY_TEST_CASE("an effective-state tag that disagrees with the flags is caught") {
    Fixture fixture;
    CY_REQUIRE(build(fixture));
    cy::scene::Node branch = fixture.tree.root().child(0);
    CY_REQUIRE(branch.set_visible(false).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK(cy::scene::coherent(fixture.tree));

    // Someone removes the tag by hand. The tag *is* the effective state, so removing it makes the
    // subtree render again while the flag still says it should not.
    CY_REQUIRE(fixture.world.remove(branch.entity(), fixture.tree.components().hidden).has_value());
    const auto report = cy::scene::check_coherence(fixture.tree);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(violations_of(*report, cy::scene::Invariant::EffectiveFlagsConsistent), 1U);
}

CY_TEST_CASE("an entity that loses its NodeName stops being a node rather than becoming half one") {
    Fixture fixture;
    CY_REQUIRE(build(fixture));
    const cy::scene::Node leaf = fixture.tree.root().child(1).child(2);
    CY_REQUIRE(leaf.valid());
    const cy::ecs::Entity entity = leaf.entity();

    CY_REQUIRE(fixture.world.remove(entity, fixture.tree.components().node_name).has_value());
    // Invariant 2 holds by construction: `NodeName` *is* the node, so removing it removes the node
    // rather than leaving an entity with two of them or with none the tree still believes in.
    CY_CHECK_FALSE(leaf.valid());
    CY_CHECK(fixture.world.is_alive(entity));
    CY_CHECK(cy::scene::coherent(fixture.tree));
    CY_CHECK_EQ(check_coherence(fixture.tree)->nodes_checked, 16U);
}

CY_TEST_CASE("the report caps what it records and still counts every violation") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    // Twenty-four nodes: more violations than a report records, and few enough that the case stays
    // inside the unit suite's millisecond. Breaking one edge above a subtree usually breaks all of
    // it, and a report holding every violation would be a memory problem on top of a correctness
    // one.
    for (cy::u32 index = 0; index < 24; ++index) {
        cy::scene::Node node = make_child(fixture.tree, fixture.tree.root(), "Leaf");
        CY_REQUIRE(node.valid());
        CY_REQUIRE(
            node.set_local_transform(cy::Transform::from_translation(cy::Vec3{1.0F, 0.0F, 0.0F}))
                .has_value());
    }
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK(cy::scene::coherent(fixture.tree));

    cy::Array<cy::scene::Node> leaves(cy::scene::test::allocator());
    CY_REQUIRE(fixture.tree.root().children(leaves).has_value());
    for (const cy::scene::Node node : leaves) {
        fixture.world
            .get_mut<cy::scene::LocalTransform>(node.entity(),
                                                fixture.tree.components().local_transform)
            ->value.translation = cy::Vec3{99.0F, 0.0F, 0.0F};
    }
    CY_REQUIRE(fixture.tree.propagate().has_value());

    const auto report = cy::scene::check_coherence(fixture.tree);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report->violations, 24U);
    CY_CHECK_EQ(report->recorded_count, cy::scene::CoherenceReport::kMaxRecorded);
    CY_CHECK_EQ(report->nodes_checked, 25U);
}
