// The state hash's coverage over a scene world. Task 1.2, carried forward from M2.
//
// At M2's close the hash covered **4 of 17 subjects**. The thirteen it did not cover were the
// components registered by name rather than by manifest identifier — the ECS's `Parent` and
// `Children`, and the scene's twelve — and the consequence was not abstract: a divergence in a
// node's name, its parent, its sibling order or its visibility produced the same hash as no
// divergence at all. A replay would have reproduced the wrong world and reported that it matched.
//
// This suite is the claim that it is closed, and it is at tests/integration/ rather than inside a
// module because it is a claim about three of them at once: the ECS's built-ins, the scene's, and
// the runtime's walk over both. Nothing below reaches into an implementation — every case builds a
// world, hashes it, changes one thing a designer could change, and hashes it again.

#include <cy/core/determinism/hash.h>
#include <cy/core/determinism/state_schema.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/values/name.h>
#include <cy/ecs/state_schema.h>
#include <cy/ecs/world.h>
#include <cy/runtime/state_hash.h>
#include <cy/scene/state_schema.h>
#include <cy/scene/tree.h>
#include <cy/test/test.h>

using cy::u32;
using cy::u64;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

/// A world, a scene tree over it, a fully declared schema, and the hash of the two.
///
/// The declaration is the whole subject of this file, so it is spelled out here rather than hidden
/// behind a helper in the engine: three calls, in the order a host makes them — reflection first
/// for the game components, then the two modules whose built-ins reflection cannot describe, then
/// the freeze that says the set is complete.
struct Fixture {
    Fixture() noexcept
        : world(allocator()), tree(world), schema(allocator()), tree_hash(allocator()) {}

    [[nodiscard]] bool start() noexcept {
        if (!world.initialize().has_value() || !tree.initialize().has_value()) {
            return false;
        }
        cy::runtime::SchemaDeclarationReport report;
        if (!cy::runtime::declare_reflected_components(world, schema, report).has_value()) {
            return false;
        }
        if (!cy::ecs::declare_relationship_state(schema, world.parent_component(),
                                                 world.children_component())
                 .has_value()) {
            return false;
        }
        if (!cy::scene::declare_scene_state(schema, tree.components()).has_value()) {
            return false;
        }
        schema.freeze();
        return true;
    }

    /// Hash the world as it stands. Propagation runs first, so the derived state the coherence
    /// checker would demand is up to date — a hash taken mid-propagation is a hash of a world in a
    /// state no consumer ever sees.
    [[nodiscard]] u64 hash() noexcept {
        cy::runtime::WorldHashReport report;
        const bool hashed =
            cy::runtime::hash_world(world, schema, tree_hash, report, nullptr).has_value();
        CY_REQUIRE(hashed);
        last = report;
        return report.hash;
    }

    cy::ecs::World world;
    cy::scene::SceneTree tree;
    cy::determinism::StateSchema schema;
    cy::determinism::StateHashTree tree_hash;
    cy::runtime::WorldHashReport last;
};

/// Two children under one parent, which is the smallest world in which a name, a parent, an order
/// and a visibility are all things that can differ.
struct Populated : Fixture {
    [[nodiscard]] bool build() noexcept {
        if (!start()) {
            return false;
        }
        auto made_parent = tree.create_node(cy::Name::intern("hangar"), tree.root());
        if (!made_parent) {
            return false;
        }
        parent = made_parent.value();
        auto first_child = tree.create_node(cy::Name::intern("gantry"), parent);
        auto second_child = tree.create_node(cy::Name::intern("crane"), parent);
        if (!first_child || !second_child) {
            return false;
        }
        first = first_child.value();
        second = second_child.value();
        return true;
    }

    cy::scene::Node parent;
    cy::scene::Node first;
    cy::scene::Node second;
};

}  // namespace

CY_TEST_CASE("every subject of a scene world is declared, so the hash is silent about nothing") {
    Populated world;
    CY_REQUIRE(world.build());

    const u64 hash = world.hash();
    CY_CHECK_NE(hash, 0ULL);
    // THE HEADLINE. The report counts, over every component type this world has registered,
    // how many have a schema and how many do not. M2 measured thirteen undeclared out of
    // seventeen — every built-in, because reflection could describe none of them.
    CY_CHECK_EQ(world.last.subjects_undeclared, 0U);
    // Fourteen: the ECS's `Parent` and `Children`, and the scene's twelve. A scene world registers
    // no reflected components of its own, which is why the total is exactly the built-ins — the
    // number to change here is the one that changes when a module registers another built-in
    // without declaring it, which is the regression this case exists to catch.
    CY_CHECK_EQ(world.last.subjects_declared, 14U);
    CY_CHECK_GT(world.last.fields_hashed, 0U);
}

CY_TEST_CASE("renaming a node changes the hash") {
    // The first of the four divergences M2's hash could not see. It is also the one that proves the
    // interned-name encoding is doing its job: a `Name` is a handle, and what the schema folds in
    // is the text behind it.
    Populated world;
    CY_REQUIRE(world.build());
    const u64 before = world.hash();

    CY_REQUIRE(world.first.set_name(cy::Name::intern("gantry-two")).has_value());
    CY_CHECK_NE(world.hash(), before);
}

CY_TEST_CASE("reparenting a node changes the hash") {
    Populated world;
    CY_REQUIRE(world.build());
    const u64 before = world.hash();

    // `second` moves from under `parent` to the root. Every value in the world is otherwise what it
    // was, so the difference the hash reports is the edge itself.
    CY_REQUIRE(world.second.set_parent(world.tree.root()).has_value());
    CY_CHECK_NE(world.hash(), before);
}

CY_TEST_CASE("changing sibling order changes the hash") {
    // `ecs-core` leaves the `Children` buffer's order unspecified, so the authored order lives in
    // `ChildOrder` and that is what is hashed. A hash over the buffer would have been a hash of
    // whichever operation last swapped an element into a gap.
    Populated world;
    CY_REQUIRE(world.build());
    const u64 before = world.hash();

    CY_REQUIRE(world.second.set_sibling_index(0).has_value());
    CY_CHECK_NE(world.hash(), before);
}

CY_TEST_CASE("changing a node's visibility changes the hash") {
    Populated world;
    CY_REQUIRE(world.build());
    const u64 before = world.hash();

    CY_REQUIRE(world.first.set_visible(false).has_value());
    CY_CHECK_NE(world.hash(), before);
}

CY_TEST_CASE("the authored transform is hashed and the derived one is recomputed, not hashed") {
    // `simulation-and-determinism`: derived state is recomputed rather than hashed. The schema says
    // `LocalTransform` is authoritative and `WorldTransform` is derived, and the two halves of that
    // are checked together — a schema that declared both as authoritative would pass the first
    // assertion and fail the second.
    Populated world;
    CY_REQUIRE(world.build());
    const u64 before = world.hash();

    CY_REQUIRE(world.first.set_local_transform(cy::Transform::from_translation({3.0F, 0.0F, 0.0F}))
                   .has_value());
    const u64 after_authored = world.hash();
    CY_CHECK_NE(after_authored, before);

    // Propagating recomputes `WorldTransform` from what was just written. The authored value has
    // not changed again, so a hash that covered the derived value would move here and one that does
    // not, does not.
    CY_REQUIRE(world.tree.propagate().has_value());
    CY_CHECK_EQ(world.hash(), after_authored);
}

CY_TEST_CASE("an interned name is hashed as its text, never as its index") {
    // The encoding, on its own, without a world around it. A `Name`'s index is its position in a
    // process-wide table filled in interning order, and `core-type-system` says it is not stable
    // across runs — so hashing the index would make the state hash a function of what else the
    // process happened to intern first.
    //
    // Asserted as an equality against the text rather than as an inequality against the index,
    // because "different from the index" would also be satisfied by hashing something else
    // entirely.
    struct Holder {
        cy::Name value;
    };
    const Holder holder{cy::Name::intern("a-name-nothing-else-in-this-suite-interns")};

    cy::determinism::StateField field;
    field.name = "value";
    field.id = 1;
    field.offset = 0;
    field.kind = cy::reflect::FieldKind::U32;
    field.classification = cy::determinism::SimulationClass::Authoritative;
    field.encoding = cy::determinism::StateEncoding::InternedName;

    cy::determinism::StateHashTree hashed_field(allocator());
    CY_REQUIRE(hashed_field.begin(cy::determinism::HashLevel::World, 1, "world").has_value());
    cy::determinism::hash_field(hashed_field, field, &holder);
    CY_REQUIRE(hashed_field.end().has_value());

    cy::determinism::StateHashTree hashed_text(allocator());
    CY_REQUIRE(hashed_text.begin(cy::determinism::HashLevel::World, 1, "world").has_value());
    // c_str(), not text().data(): the interned text is NUL-terminated at data()[size()]
    // precisely so it can be handed to an interface that takes a const char*, and
    // mix_text is one.
    hashed_text.mix_text(holder.value.c_str());
    CY_REQUIRE(hashed_text.end().has_value());

    CY_CHECK_EQ(hashed_field.root_hash(), hashed_text.root_hash());

    // And the index really is not what was folded: the same four bytes read directly give a
    // different answer.
    field.encoding = cy::determinism::StateEncoding::Direct;
    cy::determinism::StateHashTree hashed_index(allocator());
    CY_REQUIRE(hashed_index.begin(cy::determinism::HashLevel::World, 1, "world").has_value());
    cy::determinism::hash_field(hashed_index, field, &holder);
    CY_REQUIRE(hashed_index.end().has_value());
    CY_CHECK_NE(hashed_index.root_hash(), hashed_text.root_hash());
}
