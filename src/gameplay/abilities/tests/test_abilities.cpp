// M8.b tasks 4.1 and 4.2 — ability sets, grants, per-owner state, transactional costs and
// tick-exact cooldowns.

#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/abilities/abilities.h>
#include <cy/test/test.h>

#include <type_traits>

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

[[nodiscard]] AbilityDefinition fireball(u32 stable_id) noexcept {
    AbilityDefinition definition;
    definition.name = Name::intern("fireball");
    definition.stable_id = stable_id;
    definition.cooldown_ticks = 90;
    return definition;
}

}  // namespace

CY_TEST_CASE("ability_sets: a thousand units of one type reference one set") {
    AbilityRegistry registry(allocator());
    auto first =
        registry.declare(fireball(1), cy::Span<const Cost>(), cy::Span<const EffectDefId>());
    AbilityDefinition dash;
    dash.name = Name::intern("dash");
    dash.stable_id = 2;
    dash.cooldown_ticks = 30;
    auto second = registry.declare(dash, cy::Span<const Cost>(), cy::Span<const EffectDefId>());
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());

    auto set = registry.create_set(Name::intern("mage"));
    CY_REQUIRE(set.has_value());
    CY_REQUIRE(registry.add_to_set(*set, *first).has_value());
    CY_REQUIRE(registry.add_to_set(*set, *second).has_value());

    for (u32 index = 1; index <= 64; ++index) {
        CY_REQUIRE(
            registry.grant_set(entity(index), *set, Name::intern("template"), 0).has_value());
    }
    // ONE SET, SIXTY-FOUR OWNERS. Each owner carries the compact state and nothing else.
    CY_CHECK_EQ(registry.set_count(), 1U);
    CY_CHECK_EQ(registry.set_members(*set).size(), 2U);
    CY_CHECK_EQ(registry.owner_count(), 64U);
    CY_CHECK(registry.has_ability(entity(7), *first));
    // AND THE STATE IS A RECORD: an identity, a ready tick, a charge count.
    CY_CHECK(std::is_trivially_copyable_v<AbilityState>);
    AbilityState states[4] = {};
    CY_CHECK_EQ(registry.abilities_of(entity(7), states, 4), 2U);
}

CY_TEST_CASE("ability_grants: removing the equipment that granted it removes exactly that") {
    AbilityRegistry registry(allocator());
    auto blast =
        registry.declare(fireball(1), cy::Span<const Cost>(), cy::Span<const EffectDefId>());
    AbilityDefinition parry;
    parry.name = Name::intern("parry");
    parry.stable_id = 2;
    auto guard = registry.declare(parry, cy::Span<const Cost>(), cy::Span<const EffectDefId>());
    CY_REQUIRE(blast.has_value());
    CY_REQUIRE(guard.has_value());

    auto from_sword = registry.grant_ability(entity(1), *blast, Name::intern("sword"), 0);
    auto from_ring = registry.grant_ability(entity(1), *blast, Name::intern("ring"), 0);
    auto from_shield = registry.grant_ability(entity(1), *guard, Name::intern("shield"), 0);
    CY_REQUIRE(from_sword.has_value());
    CY_REQUIRE(from_ring.has_value());
    CY_REQUIRE(from_shield.has_value());

    // The sword comes off. The ring still grants the same ability, so it stays; the shield's
    // ability is untouched. That is "removal is exact", and it is why grants are records.
    CY_CHECK_EQ(registry.revoke(*from_sword), 0U);
    CY_CHECK(registry.has_ability(entity(1), *blast));
    CY_CHECK_EQ(registry.revoke(*from_ring), 1U);
    CY_CHECK_FALSE(registry.has_ability(entity(1), *blast));
    CY_CHECK(registry.has_ability(entity(1), *guard));
}

CY_TEST_CASE("ability_cooldowns: readiness is a tick value, exact and shared by a group") {
    AbilityRegistry registry(allocator());
    TagRegistry tags(allocator());
    auto group = tags.declare("Cooldown.Movement");
    CY_REQUIRE(group.has_value());

    AbilityDefinition dash;
    dash.name = Name::intern("dash");
    dash.stable_id = 1;
    dash.cooldown_ticks = 90;
    dash.cooldown_group = *group;
    AbilityDefinition blink;
    blink.name = Name::intern("blink");
    blink.stable_id = 2;
    blink.cooldown_ticks = 30;
    blink.cooldown_group = *group;
    auto first = registry.declare(dash, cy::Span<const Cost>(), cy::Span<const EffectDefId>());
    auto second = registry.declare(blink, cy::Span<const Cost>(), cy::Span<const EffectDefId>());
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(registry.grant_ability(entity(1), *first, Name::intern("kit"), 0).has_value());
    CY_REQUIRE(registry.grant_ability(entity(1), *second, Name::intern("kit"), 0).has_value());

    CY_CHECK(registry.ready(entity(1), *first, 0));
    CY_REQUIRE(registry.start_cooldown(entity(1), *first, 10).has_value());
    // A READY TICK, not a countdown: 10 + 90 = 100, exactly.
    CY_CHECK_EQ(registry.ready_tick(entity(1), *first), 100);
    CY_CHECK_FALSE(registry.ready(entity(1), *first, 99));
    CY_CHECK(registry.ready(entity(1), *first, 100));
    // THE GROUP IS SHARED: blink's own cooldown is 30, but the group says 100.
    CY_CHECK_EQ(registry.ready_tick(entity(1), *second), 100);
    CY_CHECK_FALSE(registry.ready(entity(1), *second, 99));
}

CY_TEST_CASE("ability_charges: charges recharge independently and are part of the state") {
    AbilityRegistry registry(allocator());
    AbilityDefinition volley;
    volley.name = Name::intern("volley");
    volley.stable_id = 1;
    volley.cooldown_ticks = 5;
    volley.max_charges = 3;
    volley.recharge_ticks = 40;
    auto id = registry.declare(volley, cy::Span<const Cost>(), cy::Span<const EffectDefId>());
    CY_REQUIRE(id.has_value());
    CY_REQUIRE(registry.grant_ability(entity(1), *id, Name::intern("kit"), 0).has_value());

    const AbilityState* state = registry.state_of(entity(1), *id);
    CY_REQUIRE_NE(state, nullptr);
    CY_CHECK_EQ(state->charges, 3U);
    CY_REQUIRE(registry.start_cooldown(entity(1), *id, 0).has_value());
    CY_REQUIRE(registry.start_cooldown(entity(1), *id, 10).has_value());
    CY_REQUIRE(registry.start_cooldown(entity(1), *id, 20).has_value());
    CY_CHECK_EQ(registry.state_of(entity(1), *id)->charges, 0U);
    // Out of charges is not ready, even when the cooldown has passed.
    CY_CHECK_FALSE(registry.ready(entity(1), *id, 1000));
    CY_CHECK_EQ(registry.advance_charges(59), 0U);
    CY_CHECK_EQ(registry.advance_charges(60), 1U);
    CY_CHECK(registry.ready(entity(1), *id, 1000));
    CY_CHECK_EQ(registry.advance_charges(140), 2U);
    CY_CHECK_EQ(registry.state_of(entity(1), *id)->charges, 3U);
    // And it stops at the maximum.
    CY_CHECK_EQ(registry.advance_charges(10000), 0U);
}

CY_TEST_CASE("ability_costs: two activations in one tick cannot both spend the last of it") {
    AttributeSchema schema(allocator());
    AttributeDeclaration mana;
    mana.name = Name::intern("mana");
    mana.stable_id = 1;
    mana.base = 100.0F;
    auto attribute = schema.declare(mana);
    CY_REQUIRE(attribute.has_value());
    AttributeStore store(allocator(), schema);
    CY_REQUIRE(store.add_entity(entity(1)).has_value());
    CostLedger ledger(allocator(), store);

    // VALIDATE, RESERVE, COMMIT. The first reserves sixty; the second sees forty available and is
    // refused — rather than both reading a hundred and both spending it.
    CY_CHECK_EQ(ledger.available(entity(1), *attribute), 100.0F);
    CY_REQUIRE(ledger.reserve(entity(1), *attribute, 60.0F, 0xA1).has_value());
    CY_CHECK_EQ(ledger.available(entity(1), *attribute), 40.0F);
    CY_CHECK_FALSE(ledger.reserve(entity(1), *attribute, 60.0F, 0xA2).has_value());
    CY_CHECK_EQ(ledger.outstanding(), 1U);

    CY_REQUIRE(ledger.commit(0xA1).has_value());
    CY_CHECK_EQ(store.current(entity(1), *attribute), 40.0F);
    CY_CHECK_EQ(ledger.outstanding(), 0U);

    // A refused activation RELEASES rather than spending, and the resource is available again.
    CY_REQUIRE(ledger.reserve(entity(1), *attribute, 40.0F, 0xA3).has_value());
    ledger.release(0xA3);
    CY_CHECK_EQ(ledger.available(entity(1), *attribute), 40.0F);
    CY_CHECK_EQ(store.current(entity(1), *attribute), 40.0F);
}
