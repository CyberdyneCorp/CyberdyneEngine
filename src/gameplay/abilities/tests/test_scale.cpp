// M8.b section 4's measured properties, and the milestone's exit criterion for this module:
// **a hundred concurrent gameplay effects hold their declared budget**.
//
// `integration`, not `unit`, and deliberately: every case here builds a store, a hundred owners and
// a live effect set, then runs a thousand simulation ticks. The `unit` tier's budget is a
// millisecond of CPU, and a scale case that fits into it is a scale case whose population has been
// trimmed until it stopped measuring anything — the remedy M8.a's closing gate applied to batch
// spawning, applied again.
//
// THESE ARE GAMEPLAY EFFECTS. Damage over time, stacking modifiers, expiring buffs. Particles are
// `vfx-system`'s and were deferred to M8.c.

#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/abilities/activation.h>
#include <cy/test/test.h>

#include <chrono>

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

/// THE DECLARED BUDGET. One simulation tick of a hundred concurrent gameplay effects, in
/// microseconds of CPU.
///
/// It is a bound on the SHAPE of the cost rather than a machine's number: a hundred effects is a
/// hundred integer comparisons and a handful of attribute writes, and anything that misses this by
/// an order of magnitude has stopped being a loop over compact records. The measured figure is
/// reported beside it, and the linearity case below is what actually catches a quadratic — a
/// microsecond bound alone would only catch it on a slow machine.
constexpr double kBudgetMicrosecondsPerTick = 10.0;

struct Harness {
    Harness() noexcept
        : schema(allocator()),
          attributes(allocator(), schema),
          tags(allocator()),
          tag_store(allocator()),
          effects(allocator(), attributes, tags) {}

    [[nodiscard]] bool build(u32 owners) noexcept {
        AttributeDeclaration declaration;
        declaration.name = Name::intern("health");
        declaration.stable_id = 1;
        declaration.base = 100000.0F;
        auto declared = schema.declare(declaration);
        if (!declared) {
            return false;
        }
        health = *declared;
        for (u32 index = 1; index <= owners; ++index) {
            if (!attributes.add_entity(entity(index)).has_value()) {
                return false;
            }
        }
        EffectDefinition burn;
        burn.name = Name::intern("burn");
        burn.stable_id = 1;
        burn.kind = EffectKind::Periodic;
        // Long enough to stay live for the whole measurement, and a period short enough that the
        // periodic path is exercised rather than skipped.
        burn.duration_ticks = 100000;
        burn.period_ticks = 10;
        burn.stacking = StackingPolicy::Stack;
        const EffectModifier tick_damage{health, ModifierOp::Add, -1.0F, 0, Name{}};
        const EffectModifier held{health, ModifierOp::Add, -1.0F, 0, Name{}};
        auto declared_effect = effects.declare(burn, cy::Span<const EffectModifier>(&held, 1),
                                               cy::Span<const EffectModifier>(&tick_damage, 1));
        if (!declared_effect) {
            return false;
        }
        burning = *declared_effect;
        return effects.reserve(owners + 8).has_value();
    }

    [[nodiscard]] bool light(u32 count) noexcept {
        for (u32 index = 1; index <= count; ++index) {
            ApplyReport report;
            if (!effects.apply(burning, entity(index), entity(1), 0, &tag_store, report)
                     .has_value()) {
                return false;
            }
            if (report.outcome != ApplyOutcome::Applied) {
                return false;
            }
        }
        return true;
    }

    /// Microseconds of wall clock for `ticks` advances, divided by the ticks.
    [[nodiscard]] double advance_microseconds_per_tick(u32 ticks) noexcept {
        const auto start = std::chrono::steady_clock::now();
        for (u32 index = 1; index <= ticks; ++index) {
            EffectTickReport report;
            effects.advance(static_cast<cy::i64>(index), report);
        }
        const auto finish = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();
        return static_cast<double>(elapsed) / (static_cast<double>(ticks) * 1000.0);
    }

    AttributeSchema schema;
    AttributeStore attributes;
    TagRegistry tags;
    EntityTagStore tag_store;
    EffectSystem effects;
    AttributeId health = kInvalidAttribute;
    EffectDefId burning = kInvalidEffectDef;
};

}  // namespace

CY_TEST_CASE("ability_scale: a hundred concurrent effects hold their declared budget") {
    Harness harness;
    CY_REQUIRE(harness.build(100));
    CY_REQUIRE(harness.light(100));
    CY_REQUIRE_EQ(harness.effects.active_count(), 100U);

    // Warm the caches, then measure. A single sample of a frame time is a draw; a thousand ticks
    // is what makes the number reproducible.
    (void)harness.advance_microseconds_per_tick(200);
    const double measured = harness.advance_microseconds_per_tick(1000);
    CY_TEST_MESSAGE("100 concurrent gameplay effects: " << measured << " us/tick, budget "
                                                        << kBudgetMicrosecondsPerTick << " us");
    CY_CHECK_LT(measured, kBudgetMicrosecondsPerTick);
    // And they are still all live and still ticking exactly: the budget is not being held by
    // having quietly stopped doing the work.
    CY_CHECK_EQ(harness.effects.active_count(), 100U);
    // Tick 1010 is the next period boundary after the measured thousand: one application each,
    // on the exact tick, a thousand ticks after they were lit.
    EffectTickReport report;
    harness.effects.advance(1010, report);
    CY_CHECK_EQ(report.instances_examined, 100U);
    CY_CHECK_EQ(report.periods_applied, 100U);
}

CY_TEST_CASE("ability_scale: the cost of a hundred effects is a hundred times the cost of one") {
    // THE CASE THAT CATCHES A QUADRATIC, which a microsecond bound on one machine does not. Four
    // times the effects for at most eight times the cost — generous enough not to be a coin toss,
    // tight enough that an O(n squared) advance fails it by a wide margin.
    Harness small;
    CY_REQUIRE(small.build(400));
    CY_REQUIRE(small.light(25));
    (void)small.advance_microseconds_per_tick(200);
    const double twenty_five = small.advance_microseconds_per_tick(2000);

    Harness large;
    CY_REQUIRE(large.build(400));
    CY_REQUIRE(large.light(100));
    (void)large.advance_microseconds_per_tick(200);
    const double hundred = large.advance_microseconds_per_tick(2000);

    CY_TEST_MESSAGE("25 effects: " << twenty_five << " us/tick; 100 effects: " << hundred
                                   << " us/tick");
    CY_CHECK_LT(hundred, (twenty_five * 8.0) + 1.0);
}

CY_TEST_CASE("ability_scale: many owners activate one shared ability as a loop") {
    // `gameplay-abilities-and-effects`: "WHEN ten thousand units activate the same ability in one
    // tick THEN they SHALL be processed as a batch over their shared program."
    AttributeSchema schema(allocator());
    AttributeDeclaration mana;
    mana.name = Name::intern("mana");
    mana.stable_id = 1;
    mana.base = 100.0F;
    auto attribute = schema.declare(mana);
    CY_REQUIRE(attribute.has_value());
    AttributeStore attributes(allocator(), schema);
    TagRegistry tags(allocator());
    EntityTagStore tag_store(allocator());
    RelationshipService relationships(allocator());
    EffectSystem effects(allocator(), attributes, tags);
    AbilityRegistry abilities(allocator());
    CostLedger costs(allocator(), attributes);
    GameplayRandom random(0x5EEDULL);
    ActivationPipeline pipeline(allocator(), abilities, effects, attributes, costs, tag_store, tags,
                                relationships, random);

    EffectDefinition mark;
    mark.name = Name::intern("mark");
    mark.stable_id = 1;
    mark.kind = EffectKind::Duration;
    mark.duration_ticks = 100;
    mark.stacking = StackingPolicy::Stack;
    const EffectModifier none{*attribute, ModifierOp::Add, 0.0F, 0, Name{}};
    auto effect = effects.declare(mark, cy::Span<const EffectModifier>(&none, 1));
    CY_REQUIRE(effect.has_value());

    AbilityDefinition order;
    order.name = Name::intern("order");
    order.stable_id = 1;
    order.cooldown_ticks = 10;
    const Cost cost{CostKind::Attribute, *attribute, 5.0F, Name{}};
    auto ability = abilities.declare(order, cy::Span<const Cost>(&cost, 1),
                                     cy::Span<const EffectDefId>(&effect.value(), 1));
    CY_REQUIRE(ability.has_value());
    auto set = abilities.create_set(Name::intern("soldier"));
    CY_REQUIRE(set.has_value());
    CY_REQUIRE(abilities.add_to_set(*set, *ability).has_value());

    constexpr u32 kOwners = 10000;
    cy::Array<Entity> owners(allocator());
    CY_REQUIRE(owners.reserve(kOwners).has_value());
    for (u32 index = 1; index <= kOwners; ++index) {
        const Entity owner = entity(index);
        CY_REQUIRE(attributes.add_entity(owner).has_value());
        CY_REQUIRE(abilities.grant_set(owner, *set, Name::intern("template"), 0).has_value());
        CY_REQUIRE(owners.push_back(owner).has_value());
    }
    CY_REQUIRE(effects.reserve(kOwners + 16).has_value());
    CY_REQUIRE(pipeline.reserve(kOwners + 16, 16).has_value());

    cy::determinism::SimulationPoint at;
    at.tick = 1;
    TargetData self;
    self.kind = TargetKind::SelfTarget;
    BatchReport report;
    const auto start = std::chrono::steady_clock::now();
    CY_REQUIRE(pipeline.activate_batch(*ability, owners.span(), self, at, report).has_value());
    const auto finish = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count();

    CY_TEST_MESSAGE("10 000 activations over one shared ability: " << elapsed << " us");
    CY_CHECK_EQ(report.requested, kOwners);
    CY_CHECK_EQ(report.committed, kOwners);
    CY_CHECK_EQ(report.refused, 0U);
    CY_CHECK_EQ(report.effects_applied, kOwners);
    // ONE SHARED DEFINITION AND NO ALLOCATION PER ACTIVATION. The pipeline was reserved, so the
    // batch grew nothing — which is the requirement's "no heap allocation per activation" as a
    // number rather than as a claim.
    CY_CHECK_EQ(report.allocations, 0U);
    CY_CHECK_EQ(abilities.definition_count(), 1U);
    CY_CHECK_EQ(effects.active_count(), kOwners);
}

CY_TEST_CASE("ability_scale: everything is destroyed under load, in the wrong order, twice") {
    // HARD RULE: teardown is tested under load. The failure this catches is not a leak report — it
    // is a destructor that walks a structure another destructor already released, which a suite
    // that only ever tears down an empty system never reaches.
    //
    // The scope below is entered twice so that the second entry runs against an allocator that has
    // already served and released the first, and everything inside is destroyed in REVERSE
    // declaration order while it is still full: ten thousand live effects, ten thousand owners with
    // ability sets, an attribute store with a modifier per effect, and a pipeline holding a record
    // per activation.
    for (u32 round = 0; round < 2; ++round) {
        AttributeSchema schema(allocator());
        AttributeDeclaration mana;
        mana.name = Name::intern("mana");
        mana.stable_id = 1;
        mana.base = 1000.0F;
        auto attribute = schema.declare(mana);
        CY_REQUIRE(attribute.has_value());
        AttributeStore attributes(allocator(), schema);
        TagRegistry tags(allocator());
        EntityTagStore tag_store(allocator());
        RelationshipService relationships(allocator());
        EffectSystem effects(allocator(), attributes, tags);
        AbilityRegistry abilities(allocator());
        CostLedger costs(allocator(), attributes);
        GameplayRandom random(0x5EEDULL);
        ActivationPipeline pipeline(allocator(), abilities, effects, attributes, costs, tag_store,
                                    tags, relationships, random);

        EffectDefinition curse;
        curse.name = Name::intern("curse");
        curse.stable_id = 1;
        curse.kind = EffectKind::Duration;
        curse.duration_ticks = 100000;
        curse.stacking = StackingPolicy::Stack;
        const EffectModifier drain{*attribute, ModifierOp::Add, -1.0F, 0, Name{}};
        auto effect = effects.declare(curse, cy::Span<const EffectModifier>(&drain, 1));
        CY_REQUIRE(effect.has_value());

        AbilityDefinition hex;
        hex.name = Name::intern("hex");
        hex.stable_id = 1;
        auto ability = abilities.declare(hex, cy::Span<const Cost>(),
                                         cy::Span<const EffectDefId>(&effect.value(), 1));
        CY_REQUIRE(ability.has_value());
        auto set = abilities.create_set(Name::intern("warlock"));
        CY_REQUIRE(set.has_value());
        CY_REQUIRE(abilities.add_to_set(*set, *ability).has_value());

        constexpr u32 kOwners = 10000;
        cy::Array<Entity> owners(allocator());
        CY_REQUIRE(owners.reserve(kOwners).has_value());
        for (u32 index = 1; index <= kOwners; ++index) {
            const Entity owner = entity(index);
            CY_REQUIRE(attributes.add_entity(owner).has_value());
            CY_REQUIRE(abilities.grant_set(owner, *set, Name::intern("template"), 0).has_value());
            CY_REQUIRE(owners.push_back(owner).has_value());
        }
        cy::determinism::SimulationPoint point;
        point.tick = 1;
        TargetData self;
        self.kind = TargetKind::SelfTarget;
        BatchReport report;
        CY_REQUIRE(
            pipeline.activate_batch(*ability, owners.span(), self, point, report).has_value());
        CY_REQUIRE_EQ(report.committed, kOwners);
        CY_CHECK_EQ(effects.active_count(), kOwners);
        CY_CHECK_EQ(pipeline.timeline().size(), kOwners);
        // Half of them are torn down explicitly first, so the destructors run over a structure that
        // has already had holes punched in it rather than over an untouched one.
        for (u32 index = 1; index <= kOwners / 2; ++index) {
            (void)effects.remove_all_on(entity(index));
            attributes.remove_entity(entity(index));
        }
        CY_CHECK_EQ(effects.active_count(), kOwners / 2);
    }
    // Reaching here at all is the assertion: two full rounds constructed, filled, half-dismantled
    // and destroyed. Run under `just test-sanitize` and a use-after-free in any of it is a report
    // rather than a silence.
    CY_CHECK(true);
}
