// Parent-child relationships. Task 2.9.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/command_buffer.h>
#include <cy/ecs/world.h>

#include "fixtures.h"

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Ecs);
}

}  // namespace

CY_TEST_CASE("both sides of a reparent change together") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());

    auto first_parent = world.create();
    auto second_parent = world.create();
    auto child = world.create();
    CY_REQUIRE(first_parent.has_value());
    CY_REQUIRE(second_parent.has_value());
    CY_REQUIRE(child.has_value());

    CY_REQUIRE(world.set_parent(*child, *first_parent).has_value());
    CY_CHECK_EQ(world.parent_of(*child), *first_parent);
    CY_REQUIRE_EQ(world.children_of(*first_parent).size(), 1u);
    CY_CHECK_EQ(world.children_of(*first_parent)[0], *child);
    CY_CHECK_EQ(world.children_of(*second_parent).size(), 0u);

    CY_REQUIRE(world.set_parent(*child, *second_parent).has_value());
    // The old parent's Children, the new parent's Children and the child's Parent, in one call.
    // There is no window in which a child names a parent whose Children does not name it back.
    CY_CHECK_EQ(world.parent_of(*child), *second_parent);
    CY_CHECK_EQ(world.children_of(*first_parent).size(), 0u);
    CY_REQUIRE_EQ(world.children_of(*second_parent).size(), 1u);
    CY_CHECK_EQ(world.children_of(*second_parent)[0], *child);

    CY_REQUIRE(world.set_parent(*child, cy::ecs::kNoEntity).has_value());
    CY_CHECK_FALSE(world.parent_of(*child).valid());
    CY_CHECK_EQ(world.children_of(*second_parent).size(), 0u);
}

CY_TEST_CASE("destroying a parent destroys its whole subtree by default") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());

    auto root = world.create();
    CY_REQUIRE(root.has_value());
    cy::ecs::Entity children[3];
    cy::ecs::Entity grandchildren[3];
    for (int index = 0; index < 3; ++index) {
        auto child = world.create();
        auto grandchild = world.create();
        CY_REQUIRE(child.has_value());
        CY_REQUIRE(grandchild.has_value());
        children[index] = *child;
        grandchildren[index] = *grandchild;
        CY_REQUIRE(world.set_parent(children[index], *root).has_value());
        CY_REQUIRE(world.set_parent(grandchildren[index], children[index]).has_value());
    }
    CY_CHECK_EQ(world.entity_count(), 7u);

    CY_REQUIRE(world.destroy(*root).has_value());
    CY_CHECK_EQ(world.entity_count(), 0u);
    for (int index = 0; index < 3; ++index) {
        CY_CHECK_FALSE(world.is_alive(children[index]));
        CY_CHECK_FALSE(world.is_alive(grandchildren[index]));
    }
}

CY_TEST_CASE("the opt-out reparents the children to the destroyed entity's own parent") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());

    auto grandparent = world.create();
    auto parent = world.create();
    auto child = world.create();
    CY_REQUIRE(grandparent.has_value());
    CY_REQUIRE(parent.has_value());
    CY_REQUIRE(child.has_value());
    CY_REQUIRE(world.set_parent(*parent, *grandparent).has_value());
    CY_REQUIRE(world.set_parent(*child, *parent).has_value());

    CY_REQUIRE(world.destroy(*parent, cy::ecs::DestroyPolicy::ReparentChildren).has_value());
    CY_CHECK_FALSE(world.is_alive(*parent));
    CY_CHECK(world.is_alive(*child));
    CY_CHECK_EQ(world.parent_of(*child), *grandparent);
    CY_REQUIRE_EQ(world.children_of(*grandparent).size(), 1u);
    CY_CHECK_EQ(world.children_of(*grandparent)[0], *child);
}

CY_TEST_CASE("a reparent that would close a cycle is refused") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());

    auto root = world.create();
    auto child = world.create();
    auto grandchild = world.create();
    CY_REQUIRE(root.has_value());
    CY_REQUIRE(child.has_value());
    CY_REQUIRE(grandchild.has_value());
    CY_REQUIRE(world.set_parent(*child, *root).has_value());
    CY_REQUIRE(world.set_parent(*grandchild, *child).has_value());

    // A cycle would make every traversal above this non-terminating, so it is refused where the
    // edge is made rather than discovered where it is walked.
    const auto refused = world.set_parent(*root, *grandchild);
    CY_CHECK_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::InvalidArgument);
    CY_CHECK_FALSE(world.set_parent(*root, *root).has_value());
}

CY_TEST_CASE("a hierarchy with more children than the inline capacity still holds together") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());

    auto root = world.create();
    CY_REQUIRE(root.has_value());
    cy::Array<cy::ecs::Entity> children(allocator());
    for (cy::u32 index = 0; index < 40; ++index) {
        auto child = world.create();
        CY_REQUIRE(child.has_value());
        CY_REQUIRE(world.set_parent(*child, *root).has_value());
        CY_REQUIRE(children.push_back(*child).has_value());
    }
    // Well past kInlineChildren: the Children buffer has spilled to the heap and still holds every
    // edge, and the heap block is released when the root goes away.
    CY_CHECK_EQ(world.children_of(*root).size(), 40u);
    for (const cy::ecs::Entity child : children) {
        CY_CHECK_EQ(world.parent_of(child), *root);
    }

    CY_REQUIRE(world.destroy(*root).has_value());
    CY_CHECK_EQ(world.entity_count(), 0u);
}

CY_TEST_CASE("add_child through a command buffer applies at the flush point") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());

    cy::ecs::CommandBuffer commands(world);
    CY_REQUIRE(world.attach(commands).has_value());

    auto parent = commands.create();
    auto child = commands.create();
    CY_REQUIRE(parent.has_value());
    CY_REQUIRE(child.has_value());
    CY_REQUIRE(commands.add_child(*parent, *child).has_value());

    CY_CHECK_EQ(world.entity_count(), 0u);
    CY_REQUIRE(world.flush().has_value());
    CY_CHECK_EQ(world.entity_count(), 2u);

    // Both placeholders were resolved before the edge was made, so a hierarchy can be built
    // entirely out of entities that did not exist when it was described.
    const cy::ecs::Entity real_parent = world.entities().at(0);
    const cy::ecs::Entity real_child = world.entities().at(1);
    CY_CHECK_EQ(world.parent_of(real_child), real_parent);
    CY_REQUIRE_EQ(world.children_of(real_parent).size(), 1u);
    CY_CHECK_EQ(world.children_of(real_parent)[0], real_child);
}
