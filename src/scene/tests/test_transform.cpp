// The transform model: propagation, dirty subtrees, world writes and interpolation. Task 3.1.3.

#include <cy/test/test.h>

#include <cy/scene/coherence.h>
#include <cy/scene/propagation.h>
#include <cy/scene/tree.h>

#include "fixtures.h"

using cy::scene::test::Fixture;
using cy::scene::test::make_child;

namespace {

[[nodiscard]] bool near_vec(cy::Vec3 left, cy::Vec3 right) noexcept {
    return cy::nearly_equal(left, right, 1.0e-4F);
}

}  // namespace

CY_TEST_CASE("a root's world transform is its local transform, exactly") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::Node root_child = make_child(fixture.tree, fixture.tree.root(), "A");
    const cy::Transform placed = cy::Transform::from_translation(cy::Vec3{3.0F, -1.0F, 0.5F});
    CY_REQUIRE(root_child.set_local_transform(placed).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());

    // The tree root is at the identity, so a first-level node's world transform is its local one.
    // The equality is exact, not close: propagation assigns rather than multiplying by an identity.
    CY_CHECK(root_child.world_transform() == placed);
}

CY_TEST_CASE("a child's world transform is the parent chain composed with its local") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::Node parent = make_child(fixture.tree, fixture.tree.root(), "Parent");
    cy::scene::Node child = make_child(fixture.tree, parent, "Child");

    CY_REQUIRE(parent.set_local_transform(cy::Transform::from_translation(cy::Vec3{10.0F, 0, 0}))
                   .has_value());
    CY_REQUIRE(child.set_local_transform(cy::Transform::from_translation(cy::Vec3{0, 5.0F, 0}))
                   .has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());

    CY_CHECK(near_vec(child.world_transform().translation, cy::Vec3{10.0F, 5.0F, 0.0F}));
    CY_CHECK(cy::scene::coherent(fixture.tree));
}

CY_TEST_CASE("only the moved node's subtree is recomputed") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    // A wide, deep tree: eight branches of ten, so a subtree is a small fraction of the whole.
    cy::Array<cy::scene::Node> branches(cy::scene::test::allocator());
    for (cy::u32 index = 0; index < 8; ++index) {
        const cy::scene::Node branch = make_child(fixture.tree, fixture.tree.root(), "Branch");
        CY_REQUIRE(branch.valid());
        CY_REQUIRE(cy::scene::test::make_chain(fixture.tree, branch, 9).valid());
        CY_REQUIRE(branches.push_back(branch).has_value());
    }
    const cy::u32 total = fixture.tree.stats().nodes;
    CY_CHECK_EQ(total, 1U + (8U * 10U));

    cy::scene::PropagationStats first;
    CY_REQUIRE(fixture.tree.propagate(cy::scene::PropagationPhase::Simulation, &first).has_value());
    // Everything is dirty on the first pass, by construction: a new node has never been propagated.
    CY_CHECK_EQ(first.transforms_recomputed, total);

    // Nothing moved. The second pass visits the roots, finds no subtree bit, and stops.
    cy::scene::PropagationStats quiet;
    CY_REQUIRE(fixture.tree.propagate(cy::scene::PropagationPhase::Simulation, &quiet).has_value());
    CY_CHECK_EQ(quiet.transforms_recomputed, 0U);
    CY_CHECK_EQ(quiet.nodes_visited, 1U);
    CY_CHECK_EQ(quiet.subtrees_skipped, 1U);

    // One node in the middle of one branch moves. Its subtree is recomputed and nothing else is.
    cy::scene::Node middle = branches[3].child(0).child(0);
    CY_REQUIRE(middle.valid());
    CY_REQUIRE(
        middle.set_local_transform(cy::Transform::from_translation(cy::Vec3{1, 0, 0})).has_value());

    cy::scene::PropagationStats moved;
    CY_REQUIRE(fixture.tree.propagate(cy::scene::PropagationPhase::Simulation, &moved).has_value());
    // Its subtree is itself plus the seven links below it, and nothing else is recomputed. The walk
    // visits more than it recomputes — the root, all eight branches because the root descends to
    // its children, and the one link above the moved node — and stops at each of the seven quiet
    // branches without entering it. That is the shape the requirement asks for: recomputation is
    // the subtree, and the visit is `depth + subtree` plus the siblings met on the way.
    CY_CHECK_EQ(moved.transforms_recomputed, 8U);
    CY_CHECK_EQ(moved.nodes_visited, 18U);
    CY_CHECK_EQ(moved.subtrees_skipped, 7U);
    CY_CHECK_LT(moved.nodes_visited, total);
    CY_CHECK(cy::scene::coherent(fixture.tree));
}

CY_TEST_CASE("writing a world transform derives the local one and keeps it authoritative") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::Node parent = make_child(fixture.tree, fixture.tree.root(), "Parent");
    cy::scene::Node child = make_child(fixture.tree, parent, "Child");
    CY_REQUIRE(parent.set_local_transform(cy::Transform::from_translation(cy::Vec3{10, 0, 0}))
                   .has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());

    const cy::Transform wanted = cy::Transform::from_translation(cy::Vec3{12.0F, 3.0F, 0.0F});
    CY_REQUIRE(child.set_world_transform(wanted).has_value());

    // The local value is what was written; the world value is still the propagation's to produce.
    CY_CHECK(near_vec(child.local_transform().translation, cy::Vec3{2.0F, 3.0F, 0.0F}));
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK(near_vec(child.world_transform().translation, wanted.translation));
}

CY_TEST_CASE("reparenting with keep-world recomputes the local transform") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::Node first = make_child(fixture.tree, fixture.tree.root(), "First");
    cy::scene::Node second = make_child(fixture.tree, fixture.tree.root(), "Second");
    cy::scene::Node child = make_child(fixture.tree, first, "Child");

    CY_REQUIRE(
        first.set_local_transform(cy::Transform::from_translation(cy::Vec3{10, 0, 0})).has_value());
    CY_REQUIRE(second.set_local_transform(cy::Transform::from_translation(cy::Vec3{0, 20, 0}))
                   .has_value());
    CY_REQUIRE(
        child.set_local_transform(cy::Transform::from_translation(cy::Vec3{1, 1, 1})).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    const cy::Vec3 before = child.world_transform().translation;

    CY_REQUIRE(child.set_parent(second, cy::scene::Reparent::KeepWorld).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK(near_vec(child.world_transform().translation, before));
    CY_CHECK(near_vec(child.local_transform().translation, cy::Vec3{11.0F, -19.0F, 1.0F}));

    // The default keeps the local value, so the node moves with its new parent's frame.
    cy::scene::Node other = make_child(fixture.tree, first, "Other");
    CY_REQUIRE(
        other.set_local_transform(cy::Transform::from_translation(cy::Vec3{1, 0, 0})).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_REQUIRE(other.set_parent(second).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK(near_vec(other.world_transform().translation, cy::Vec3{1.0F, 20.0F, 0.0F}));
}

CY_TEST_CASE(
    "interpolation blends the previous and current world transforms, and teleport does not") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::Node node = make_child(fixture.tree, fixture.tree.root(), "Mover");
    CY_REQUIRE(node.set_interpolated(true).has_value());
    CY_CHECK(node.interpolated());

    CY_REQUIRE(
        node.set_local_transform(cy::Transform::from_translation(cy::Vec3{0, 0, 0})).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_REQUIRE(
        node.set_local_transform(cy::Transform::from_translation(cy::Vec3{10, 0, 0})).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());

    // Halfway between the previous tick's placement and this one.
    CY_CHECK(near_vec(node.render_transform(0.5F).translation, cy::Vec3{5.0F, 0.0F, 0.0F}));
    CY_CHECK(near_vec(node.render_transform(1.0F).translation, cy::Vec3{10.0F, 0.0F, 0.0F}));

    // A teleport suppresses the blend for that frame: the history collapses onto the new placement.
    CY_REQUIRE(
        node.set_local_transform(cy::Transform::from_translation(cy::Vec3{100, 0, 0})).has_value());
    CY_REQUIRE(node.teleport().has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK(near_vec(node.render_transform(0.5F).translation, cy::Vec3{100.0F, 0.0F, 0.0F}));

    // And the next tick blends normally again, from the teleported placement.
    CY_REQUIRE(
        node.set_local_transform(cy::Transform::from_translation(cy::Vec3{110, 0, 0})).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK(near_vec(node.render_transform(0.5F).translation, cy::Vec3{105.0F, 0.0F, 0.0F}));

    // A node that never opted in renders at its world transform, with no history column at all.
    cy::scene::Node plain = make_child(fixture.tree, fixture.tree.root(), "Plain");
    CY_REQUIRE(
        plain.set_local_transform(cy::Transform::from_translation(cy::Vec3{4, 0, 0})).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_CHECK_FALSE(plain.interpolated());
    CY_CHECK(near_vec(plain.render_transform(0.25F).translation, cy::Vec3{4.0F, 0.0F, 0.0F}));
}

CY_TEST_CASE("the render phase leaves the interpolation history alone") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    cy::scene::Node node = make_child(fixture.tree, fixture.tree.root(), "Mover");
    CY_REQUIRE(node.set_interpolated(true).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());
    CY_REQUIRE(
        node.set_local_transform(cy::Transform::from_translation(cy::Vec3{10, 0, 0})).has_value());
    CY_REQUIRE(fixture.tree.propagate(cy::scene::PropagationPhase::Simulation).has_value());

    // Two render-phase propagations between ticks must not roll the history forward, or the blend
    // would collapse the moment a frame ran twice between simulation steps.
    CY_REQUIRE(fixture.tree.propagate(cy::scene::PropagationPhase::Render).has_value());
    CY_REQUIRE(fixture.tree.propagate(cy::scene::PropagationPhase::Render).has_value());
    CY_CHECK(near_vec(node.render_transform(0.5F).translation, cy::Vec3{5.0F, 0.0F, 0.0F}));
}
