// M8.b task 4.2 — effects, stacking policies, and periods that are exact because they are ticks.

#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/abilities/effects.h>
#include <cy/test/test.h>

#include <type_traits>

using namespace cy::gameplay;
using namespace cy::gameplay::abilities;
using cy::Name;
using cy::u32;
using cy::u64;
using cy::ecs::Entity;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

[[nodiscard]] Entity entity(u32 index) noexcept {
    return Entity::make(index, 1);
}

struct Fixture {
    Fixture() noexcept
        : schema(allocator()),
          store(allocator(), schema),
          tags(allocator()),
          tag_store(allocator()),
          effects(allocator(), store, tags) {}

    [[nodiscard]] bool build() noexcept {
        AttributeDeclaration declaration;
        declaration.name = Name::intern("health");
        declaration.stable_id = 1;
        declaration.base = 100.0F;
        auto declared = schema.declare(declaration);
        if (!declared) {
            return false;
        }
        health = *declared;
        return store.add_entity(entity(1)).has_value() && store.add_entity(entity(2)).has_value();
    }

    AttributeSchema schema;
    AttributeStore store;
    TagRegistry tags;
    EntityTagStore tag_store;
    EffectSystem effects;
    AttributeId health = kInvalidAttribute;
};

}  // namespace

CY_TEST_CASE("ability_effects: a burn ticks exactly ten times at exact ticks") {
    // `gameplay-abilities-and-effects`' own scenario: "WHEN a periodic effect applies every thirty
    // ticks for three hundred THEN it SHALL apply exactly ten times, at exact ticks".
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    EffectDefinition burn;
    burn.name = Name::intern("burn");
    burn.stable_id = 1;
    burn.kind = EffectKind::Periodic;
    burn.duration_ticks = 300;
    burn.period_ticks = 30;
    burn.source_ordinal = 1;
    const EffectModifier tick_damage{fixture.health, ModifierOp::Add, -5.0F, 0, Name{}};
    auto id = fixture.effects.declare(burn, cy::Span<const EffectModifier>(),
                                      cy::Span<const EffectModifier>(&tick_damage, 1));
    CY_REQUIRE(id.has_value());

    ApplyReport applied;
    CY_REQUIRE(fixture.effects.apply(*id, entity(1), entity(2), 0, &fixture.tag_store, applied)
                   .has_value());
    CY_CHECK_EQ(applied.outcome, ApplyOutcome::Applied);

    u32 periods = 0;
    for (cy::i64 tick = 1; tick <= 400; ++tick) {
        EffectTickReport report;
        fixture.effects.advance(tick, report);
        periods += report.periods_applied;
        if (tick == 29) {
            CY_CHECK_EQ(periods, 0U);
        }
        if (tick == 30) {
            // AT THE EXACT TICK. Not at 29, not at 31.
            CY_CHECK_EQ(periods, 1U);
        }
    }
    CY_CHECK_EQ(periods, 10U);
    CY_CHECK_EQ(fixture.store.base(entity(1), fixture.health), 50.0F);
    CY_CHECK_EQ(fixture.effects.active_count(), 0U);
}

CY_TEST_CASE("ability_effects: poison stacks to its limit and refreshes, with no per-effect code") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    EffectDefinition poison;
    poison.name = Name::intern("poison");
    poison.stable_id = 2;
    poison.kind = EffectKind::Duration;
    poison.duration_ticks = 100;
    poison.stacking = StackingPolicy::LimitedStacks;
    poison.max_stacks = 5;
    poison.source_ordinal = 2;
    const EffectModifier slow{fixture.health, ModifierOp::Add, -2.0F, 0, Name{}};
    auto id = fixture.effects.declare(poison, cy::Span<const EffectModifier>(&slow, 1));
    CY_REQUIRE(id.has_value());

    ApplyReport applied;
    for (u32 round = 0; round < 5; ++round) {
        CY_REQUIRE(fixture.effects
                       .apply(*id, entity(1), entity(2), static_cast<cy::i64>(round),
                              &fixture.tag_store, applied)
                       .has_value());
    }
    CY_CHECK_EQ(applied.outcome, ApplyOutcome::Stacked);
    CY_CHECK_EQ(applied.stacks, 5U);
    CY_CHECK_EQ(fixture.effects.active_count(), 1U);
    // Five stacks of "-2" is -10 once, not five modifiers of -2 that a walk has to add up.
    CY_CHECK_EQ(fixture.store.current(entity(1), fixture.health), 90.0F);

    // The sixth reapplication is CAPPED and REFRESHES, and says which.
    CY_REQUIRE(fixture.effects.apply(*id, entity(1), entity(2), 50, &fixture.tag_store, applied)
                   .has_value());
    CY_CHECK_EQ(applied.outcome, ApplyOutcome::StacksCapped);
    CY_CHECK_EQ(applied.stacks, 5U);
    const EffectInstance* live = fixture.effects.instance(applied.instance);
    CY_REQUIRE_NE(live, nullptr);
    CY_CHECK_EQ(live->end_tick, 150);
}

CY_TEST_CASE("ability_effects: the declared policy decides, and the engine resolves it") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    const EffectModifier none{fixture.health, ModifierOp::Add, 0.0F, 0, Name{}};

    EffectDefinition unique;
    unique.name = Name::intern("mark");
    unique.stable_id = 3;
    unique.kind = EffectKind::Duration;
    unique.duration_ticks = 50;
    unique.stacking = StackingPolicy::UniqueBySource;
    auto marked = fixture.effects.declare(unique, cy::Span<const EffectModifier>(&none, 1));
    CY_REQUIRE(marked.has_value());

    ApplyReport applied;
    CY_REQUIRE(fixture.effects.apply(*marked, entity(1), entity(2), 0, &fixture.tag_store, applied)
                   .has_value());
    CY_CHECK_EQ(applied.outcome, ApplyOutcome::Applied);
    // A second source marks independently; the same source refreshes.
    CY_REQUIRE(fixture.effects
                   .apply(*marked, entity(1), Entity::make(3, 1), 0, &fixture.tag_store, applied)
                   .has_value());
    CY_CHECK_EQ(applied.outcome, ApplyOutcome::Applied);
    CY_REQUIRE(fixture.effects.apply(*marked, entity(1), entity(2), 10, &fixture.tag_store, applied)
                   .has_value());
    CY_CHECK_EQ(applied.outcome, ApplyOutcome::Refreshed);
    CY_CHECK_EQ(fixture.effects.active_count(), 2U);

    EffectDefinition longest;
    longest.name = Name::intern("shield");
    longest.stable_id = 4;
    longest.kind = EffectKind::Duration;
    longest.duration_ticks = 20;
    longest.stacking = StackingPolicy::KeepHighest;
    auto shield = fixture.effects.declare(longest, cy::Span<const EffectModifier>(&none, 1));
    CY_REQUIRE(shield.has_value());
    CY_REQUIRE(
        fixture.effects.apply(*shield, entity(2), entity(1), 100, &fixture.tag_store, applied)
            .has_value());
    // Reapplied EARLIER than the live one ends: kept, and reported as kept.
    CY_REQUIRE(
        fixture.effects.apply(*shield, entity(2), entity(1), 105, &fixture.tag_store, applied)
            .has_value());
    CY_CHECK_EQ(applied.outcome, ApplyOutcome::Replaced);
    CY_REQUIRE(
        fixture.effects.apply(*shield, entity(2), entity(1), 100, &fixture.tag_store, applied)
            .has_value());
    CY_CHECK_EQ(applied.outcome, ApplyOutcome::Kept);
}

CY_TEST_CASE("ability_effects: immunity and requirement checks come first") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    auto immune = fixture.tags.declare("State.FireImmune");
    auto flammable = fixture.tags.declare("State.Flammable");
    CY_REQUIRE(immune.has_value());
    CY_REQUIRE(flammable.has_value());

    EffectDefinition burn;
    burn.name = Name::intern("burn");
    burn.stable_id = 5;
    burn.kind = EffectKind::Duration;
    burn.duration_ticks = 10;
    burn.immunity_tag = *immune;
    burn.required_tag = *flammable;
    const EffectModifier hurt{fixture.health, ModifierOp::Add, -1.0F, 0, Name{}};
    auto id = fixture.effects.declare(burn, cy::Span<const EffectModifier>(&hurt, 1));
    CY_REQUIRE(id.has_value());

    ApplyReport applied;
    CY_REQUIRE(fixture.effects.apply(*id, entity(1), entity(2), 0, &fixture.tag_store, applied)
                   .has_value());
    CY_CHECK_EQ(applied.outcome, ApplyOutcome::RefusedRequirement);
    CY_CHECK_EQ(fixture.effects.active_count(), 0U);

    CY_REQUIRE(fixture.tag_store.add(entity(1), *flammable).has_value());
    CY_REQUIRE(fixture.tag_store.add(entity(1), *immune).has_value());
    CY_REQUIRE(fixture.effects.apply(*id, entity(1), entity(2), 0, &fixture.tag_store, applied)
                   .has_value());
    // IMMUNITY IS CHECKED FIRST, so an immune-and-flammable target is immune.
    CY_CHECK_EQ(applied.outcome, ApplyOutcome::RefusedImmune);

    CY_CHECK(fixture.tag_store.remove(entity(1), *immune));
    CY_REQUIRE(fixture.effects.apply(*id, entity(1), entity(2), 0, &fixture.tag_store, applied)
                   .has_value());
    CY_CHECK_EQ(applied.outcome, ApplyOutcome::Applied);
    CY_CHECK_EQ(fixture.store.current(entity(1), fixture.health), 99.0F);
}

CY_TEST_CASE("ability_effects: an effect is a record, and a rollback is an assignment") {
    // "Effects are not objects": the live set is one array of trivially copyable records, so a
    // snapshot is a copy and a rollback is a restore. Nothing here allocates per effect.
    CY_CHECK(std::is_trivially_copyable_v<EffectInstance>);

    Fixture fixture;
    CY_REQUIRE(fixture.build());
    EffectDefinition burn;
    burn.name = Name::intern("burn");
    burn.stable_id = 6;
    burn.kind = EffectKind::Periodic;
    burn.duration_ticks = 300;
    burn.period_ticks = 30;
    const EffectModifier tick_damage{fixture.health, ModifierOp::Add, -5.0F, 0, Name{}};
    auto id = fixture.effects.declare(burn, cy::Span<const EffectModifier>(),
                                      cy::Span<const EffectModifier>(&tick_damage, 1));
    CY_REQUIRE(id.has_value());
    CY_REQUIRE(fixture.effects.reserve(64).has_value());

    ApplyReport applied;
    CY_REQUIRE(fixture.effects.apply(*id, entity(1), entity(2), 0, &fixture.tag_store, applied)
                   .has_value());
    for (cy::i64 tick = 1; tick <= 90; ++tick) {
        EffectTickReport report;
        fixture.effects.advance(tick, report);
    }
    const cy::f32 health_at_90 = fixture.store.base(entity(1), fixture.health);
    const u64 digest_at_90 = fixture.effects.digest();
    cy::Array<EffectInstance> snapshot(allocator());
    CY_REQUIRE(snapshot.append(fixture.effects.snapshot()).has_value());

    for (cy::i64 tick = 91; tick <= 150; ++tick) {
        EffectTickReport report;
        fixture.effects.advance(tick, report);
    }
    CY_CHECK_NE(fixture.effects.digest(), digest_at_90);

    // ROLL BACK: restore the records and the base value, then replay the same ticks. The periods
    // land on the same ticks because they are ticks.
    CY_REQUIRE(fixture.effects.restore(snapshot.span()).has_value());
    CY_REQUIRE(fixture.store.set_base(entity(1), fixture.health, health_at_90).has_value());
    CY_CHECK_EQ(fixture.effects.digest(), digest_at_90);
    for (cy::i64 tick = 91; tick <= 150; ++tick) {
        EffectTickReport report;
        fixture.effects.advance(tick, report);
    }
    // Three periods before the snapshot and two after it, both times: 100 - 5 * 5.
    CY_CHECK_EQ(fixture.store.base(entity(1), fixture.health), 75.0F);
}
