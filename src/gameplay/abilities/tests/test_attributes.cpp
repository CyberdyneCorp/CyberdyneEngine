// M8.b task 4.2 — attributes and the engine-specified modifier order.
//
// The case that matters most is "insertion order does not decide": the same four modifiers are
// applied in two different orders on two stores, and the two answers are equal. Without it,
// `gameplay-abilities-and-effects`' "Order SHALL NOT depend on insertion order, container
// iteration order, or the order effects happened to be applied" is a comment.

#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/abilities/attributes.h>
#include <cy/test/test.h>

using namespace cy::gameplay;
using namespace cy::gameplay::abilities;
using cy::Name;
using cy::u32;
using cy::ecs::Entity;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

[[nodiscard]] Entity entity(u32 index) noexcept {
    return Entity::make(index, 1);
}

[[nodiscard]] AttributeDeclaration health() noexcept {
    AttributeDeclaration declaration;
    declaration.name = Name::intern("health");
    declaration.stable_id = 1;
    declaration.base = 100.0F;
    declaration.minimum = 0.0F;
    declaration.has_minimum = true;
    declaration.replicated = true;
    declaration.persistence = PersistenceClass::SaveGame;
    return declaration;
}

cy::f32 halve(cy::f32 current, cy::f32 magnitude) noexcept {
    return current * magnitude;
}

}  // namespace

CY_TEST_CASE(
    "ability_attributes: an attribute is data with a declaration, not a dictionary entry") {
    AttributeSchema schema(allocator());
    auto id = schema.declare(health());
    CY_REQUIRE(id.has_value());
    AttributeDeclaration armour;
    armour.name = Name::intern("armour");
    armour.stable_id = 2;
    armour.base = 10.0F;
    armour.persistence = PersistenceClass::Derived;
    armour.replicated = true;
    auto second = schema.declare(armour);
    CY_REQUIRE(second.has_value());

    // ONE DECLARATION, SEVERAL BEHAVIOURS. Replication and saving are derived from it, exactly as
    // a session fragment's are.
    CY_CHECK(schema.replicated(*id));
    CY_CHECK(schema.saved(*id));
    CY_CHECK_FALSE(schema.replicated(*second));
    CY_CHECK_FALSE(schema.saved(*second));
    // A stable identity cannot be reused, and zero is not one.
    CY_CHECK_FALSE(schema.declare(health()).has_value());

    AttributeStore store(allocator(), schema);
    CY_REQUIRE(store.add_entity(entity(1)).has_value());
    // The hot path is an identifier. `find_by_name` exists for the cook and is spelled so that its
    // appearance in a profile is obvious.
    CY_CHECK_EQ(schema.find_by_name(Name::intern("health")), *id);
    CY_CHECK_EQ(store.current(entity(1), *id), 100.0F);
}

CY_TEST_CASE("ability_attributes: two modifiers of different kinds follow the specified order") {
    AttributeSchema schema(allocator());
    auto id = schema.declare(health());
    CY_REQUIRE(id.has_value());
    AttributeStore store(allocator(), schema);
    CY_REQUIRE(store.add_entity(entity(1)).has_value());

    Modifier add;
    add.attribute = *id;
    add.op = ModifierOp::Add;
    add.magnitude = 50.0F;
    add.source_ordinal = 1;
    Modifier multiply;
    multiply.attribute = *id;
    multiply.op = ModifierOp::Multiply;
    multiply.magnitude = 2.0F;
    multiply.source_ordinal = 2;
    CY_REQUIRE(store.add_modifier(entity(1), add).has_value());
    CY_REQUIRE(store.add_modifier(entity(1), multiply).has_value());
    // ADDITIVE THEN MULTIPLICATIVE: (100 + 50) * 2, never 100 + (50 * 2).
    CY_CHECK_EQ(store.current(entity(1), *id), 300.0F);

    Modifier override_it;
    override_it.attribute = *id;
    override_it.op = ModifierOp::Override;
    override_it.magnitude = 1.0F;
    override_it.priority = 5;
    override_it.source_ordinal = 3;
    Modifier weaker_override = override_it;
    weaker_override.magnitude = 7.0F;
    weaker_override.priority = 1;
    weaker_override.source_ordinal = 4;
    CY_REQUIRE(store.add_modifier(entity(1), weaker_override).has_value());
    CY_REQUIRE(store.add_modifier(entity(1), override_it).has_value());
    // THE HIGHEST-PRIORITY OVERRIDE WINS, and it replaces what came before it.
    CY_CHECK_EQ(store.current(entity(1), *id), 1.0F);

    Modifier floor_it;
    floor_it.attribute = *id;
    floor_it.op = ModifierOp::ClampMin;
    floor_it.magnitude = 20.0F;
    floor_it.source_ordinal = 5;
    CY_REQUIRE(store.add_modifier(entity(1), floor_it).has_value());
    // CLAMPING IS LAST.
    CY_CHECK_EQ(store.current(entity(1), *id), 20.0F);
}

CY_TEST_CASE("ability_attributes: insertion order does not decide the answer") {
    AttributeSchema schema(allocator());
    auto id = schema.declare(health());
    CY_REQUIRE(id.has_value());

    Modifier modifiers[4];
    modifiers[0] = Modifier{*id, ModifierOp::Multiply, 1.5F, 0, 10, 0, Name{}};
    modifiers[1] = Modifier{*id, ModifierOp::Add, 40.0F, 0, 20, 0, Name{}};
    modifiers[2] = Modifier{*id, ModifierOp::ClampMax, 180.0F, 0, 30, 0, Name{}};
    modifiers[3] = Modifier{*id, ModifierOp::Add, 10.0F, 0, 5, 0, Name{}};

    AttributeStore forward(allocator(), schema);
    CY_REQUIRE(forward.add_entity(entity(1)).has_value());
    for (const Modifier& modifier : modifiers) {
        CY_REQUIRE(forward.add_modifier(entity(1), modifier).has_value());
    }

    AttributeStore backward(allocator(), schema);
    CY_REQUIRE(backward.add_entity(entity(1)).has_value());
    for (u32 index = 4; index-- > 0;) {
        CY_REQUIRE(backward.add_modifier(entity(1), modifiers[index]).has_value());
    }

    // THE SAME NUMBER ON TWO MACHINES. (100 + 10 + 40) * 1.5 = 225, clamped to 180.
    CY_CHECK_EQ(forward.current(entity(1), *id), 180.0F);
    CY_CHECK_EQ(backward.current(entity(1), *id), forward.current(entity(1), *id));
    CY_CHECK_EQ(backward.digest(), forward.digest());
}

CY_TEST_CASE("ability_attributes: the base and every contribution are inspectable") {
    AttributeSchema schema(allocator());
    auto id = schema.declare(health());
    CY_REQUIRE(id.has_value());
    AttributeStore store(allocator(), schema);
    CY_REQUIRE(store.add_entity(entity(1)).has_value());
    CY_REQUIRE(store.register_custom(Name::intern("halve"), &halve).has_value());

    CY_REQUIRE(store.add_modifier(entity(1), Modifier{*id, ModifierOp::Add, 20.0F, 0, 1, 0, Name{}})
                   .has_value());
    CY_REQUIRE(store
                   .add_modifier(entity(1), Modifier{*id, ModifierOp::Custom, 0.5F, 0, 2, 0,
                                                     Name::intern("halve")})
                   .has_value());
    CY_REQUIRE(
        store.add_modifier(entity(1), Modifier{*id, ModifierOp::Override, 5.0F, 9, 3, 0, Name{}})
            .has_value());
    CY_REQUIRE(
        store.add_modifier(entity(1), Modifier{*id, ModifierOp::Override, 6.0F, 1, 4, 0, Name{}})
            .has_value());

    Contribution contributions[8] = {};
    const u32 count = store.explain(entity(1), *id, contributions, 8);
    CY_REQUIRE_EQ(count, 4U);
    CY_CHECK_EQ(store.base(entity(1), *id), 100.0F);
    CY_CHECK_EQ(contributions[0].op, ModifierOp::Add);
    CY_CHECK_EQ(contributions[0].value_after, 120.0F);
    CY_CHECK_EQ(contributions[1].op, ModifierOp::Custom);
    CY_CHECK_EQ(contributions[1].value_after, 60.0F);
    CY_CHECK_EQ(contributions[2].op, ModifierOp::Override);
    CY_CHECK(contributions[2].applied);
    CY_CHECK_EQ(contributions[2].value_after, 5.0F);
    // The override that lost is REPORTED as not applied rather than being invisible.
    CY_CHECK_FALSE(contributions[3].applied);
    CY_CHECK_EQ(store.current(entity(1), *id), 5.0F);
}

CY_TEST_CASE("ability_attributes: an effect's modifiers are removed exactly") {
    AttributeSchema schema(allocator());
    auto id = schema.declare(health());
    CY_REQUIRE(id.has_value());
    AttributeStore store(allocator(), schema);
    CY_REQUIRE(store.add_entity(entity(1)).has_value());

    CY_REQUIRE(store.add_modifier(entity(1), Modifier{*id, ModifierOp::Add, 10.0F, 0, 1, 7, Name{}})
                   .has_value());
    CY_REQUIRE(store.add_modifier(entity(1), Modifier{*id, ModifierOp::Add, 20.0F, 0, 2, 7, Name{}})
                   .has_value());
    CY_REQUIRE(store.add_modifier(entity(1), Modifier{*id, ModifierOp::Add, 30.0F, 0, 3, 8, Name{}})
                   .has_value());
    CY_CHECK_EQ(store.current(entity(1), *id), 160.0F);
    CY_CHECK_EQ(store.remove_modifiers_of(entity(1), 7), 2U);
    // The other effect's modifier is untouched — which is what makes an expiry exact.
    CY_CHECK_EQ(store.current(entity(1), *id), 130.0F);
    CY_CHECK_EQ(store.modifier_count(entity(1)), 1U);
}
