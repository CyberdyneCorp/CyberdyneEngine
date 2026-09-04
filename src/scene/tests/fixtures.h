#pragma once
// The world-and-tree pair every scene suite starts from, and the components they share.
//
// A `World` and a `SceneTree` are both non-movable and the tree binds to the world, so a fixture is
// a struct holding both by value rather than a factory returning one. `initialize()` is separate
// from construction in both, for the same reason: it allocates, and a constructor under
// -fno-exceptions cannot say that it could not.

#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/type_info.h>
#include <cy/ecs/world.h>
#include <cy/scene/tree.h>

namespace cy::scene::test {

/// The scene layer's nodes are entities, so its allocations belong to the domain the ECS's do:
/// `MemoryDomain::World` is the one M1's budget tree names for a loaded world.
[[nodiscard]] inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// A world with a scene tree over it, both initialized.
struct Fixture {
    Fixture() noexcept : world(allocator()), tree(world) {}

    /// Both halves brought up. Spelled as a call rather than done in the constructor so a failure
    /// is a failed assertion in the test rather than a half-built object.
    [[nodiscard]] bool start() noexcept {
        return world.initialize().has_value() && tree.initialize().has_value();
    }

    ecs::World world;
    SceneTree tree;
};

/// A game component, so the suites can test composition without inventing a renderer. The
/// descriptor is hand-written for the reason src/ecs/tests/fixtures.h records: the reflection
/// generator's annotated-header list is not this module's to edit at M2.
struct Health {
    i32 value = 100;
};

[[nodiscard]] inline const reflect::TypeInfo& health_type() noexcept {
    static reflect::TypeInfo info;
    info.name = "cy::scene::test::Health";
    info.id = reflect::TypeId(9101);
    info.size = static_cast<u32>(sizeof(Health));
    info.alignment = static_cast<u32>(alignof(Health));
    info.trivially_relocatable = true;
    return info;
}

/// A second one, so a template can list more than one component.
struct Loudness {
    f32 decibels = 0.0F;
};

[[nodiscard]] inline const reflect::TypeInfo& loudness_type() noexcept {
    static reflect::TypeInfo info;
    info.name = "cy::scene::test::Loudness";
    info.id = reflect::TypeId(9102);
    info.size = static_cast<u32>(sizeof(Loudness));
    info.alignment = static_cast<u32>(alignof(Loudness));
    info.trivially_relocatable = true;
    return info;
}

/// Create a child of `parent` named `name`, requiring success. Returns a null node on failure so a
/// caller's own assertion reports it.
[[nodiscard]] inline Node make_child(SceneTree& tree, Node parent, const char* name) noexcept {
    Expected<Node, Error> node = tree.create_node(Name::intern(name), parent);
    return node ? *node : Node();
}

/// A chain of `depth` nodes below `parent`, each named `link<n>`. Returns the deepest.
[[nodiscard]] inline Node make_chain(SceneTree& tree, Node parent, u32 depth) noexcept {
    Node current = parent;
    for (u32 index = 0; index < depth; ++index) {
        Expected<Node, Error> node = tree.create_node(Name::intern("link"), current);
        if (!node) {
            return {};
        }
        current = *node;
    }
    return current;
}

}  // namespace cy::scene::test
