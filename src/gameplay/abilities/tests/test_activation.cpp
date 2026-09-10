// M8.b tasks 4.1 and 4.3 — the activation pipeline in the specification's order, structured
// validation callable without activating, targeting, activation identity, prediction, and cues.
//
// The case that ties this module to M8.b section 2 is "a compiled ability program refuses at its
// own stage": the graph is authored here, compiled by `cy::graph::script::compile_ability`, and run
// by this pipeline through `AbilityScriptHost`. The spike found abilities and scripting are one
// language serving two consumers; this is what that looks like when it is wired up.

#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/abilities/activation.h>
#include <cy/test/test.h>

using namespace cy::gameplay;
using namespace cy::gameplay::abilities;
using cy::Name;
using cy::u32;
using cy::u64;
using cy::Vec3;
using cy::ecs::Entity;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

[[nodiscard]] Entity entity(u32 index) noexcept {
    return Entity::make(index, 1);
}

/// Positions for the targeting rules. The host answers these; this module has no world.
struct World {
    static bool position(Entity subject, Vec3& out, void* user) noexcept {
        (void)user;
        out = Vec3{static_cast<cy::f32>(subject.index()) * 5.0F, 0.0F, 0.0F};
        return true;
    }
    static bool always_visible(const Vec3&, const Vec3&, void*) noexcept { return true; }
    static bool never_visible(const Vec3&, const Vec3&, void*) noexcept { return false; }
};

struct Fixture {
    Fixture() noexcept
        : schema(allocator()),
          attributes(allocator(), schema),
          tags(allocator()),
          tag_store(allocator()),
          relationships(allocator()),
          effects(allocator(), attributes, tags),
          abilities(allocator()),
          costs(allocator(), attributes),
          buffer(allocator()),
          random(0x5EEDULL),
          pipeline(allocator(), abilities, effects, attributes, costs, tag_store, tags,
                   relationships, random) {}

    [[nodiscard]] bool build() noexcept {
        AttributeDeclaration mana_declaration;
        mana_declaration.name = Name::intern("mana");
        mana_declaration.stable_id = 1;
        mana_declaration.base = 100.0F;
        auto declared = schema.declare(mana_declaration);
        if (!declared) {
            return false;
        }
        mana = *declared;
        AttributeDeclaration health_declaration;
        health_declaration.name = Name::intern("health");
        health_declaration.stable_id = 2;
        health_declaration.base = 200.0F;
        auto second = schema.declare(health_declaration);
        if (!second) {
            return false;
        }
        health = *second;
        if (!attributes.add_entity(caster).has_value() ||
            !attributes.add_entity(victim).has_value()) {
            return false;
        }
        target_context.position = &World::position;
        target_context.line_of_sight = &World::always_visible;
        pipeline.set_target_context(target_context);
        pipeline.set_target_buffer(&buffer);
        return pipeline.reserve(64, 64).has_value();
    }

    AttributeSchema schema;
    AttributeStore attributes;
    TagRegistry tags;
    EntityTagStore tag_store;
    RelationshipService relationships;
    EffectSystem effects;
    AbilityRegistry abilities;
    CostLedger costs;
    TargetBuffer buffer;
    GameplayRandom random;
    TargetContext target_context;
    ActivationPipeline pipeline;
    AttributeId mana = kInvalidAttribute;
    AttributeId health = kInvalidAttribute;
    Entity caster = entity(1);
    Entity victim = entity(2);
};

/// A `name` literal, the way a graph's authored property carries one. The graph module has no
/// public helper for this and its own suite writes the same three lines.
[[nodiscard]] cy::graph::Literal text(const char* value) noexcept {
    cy::graph::Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(value);
    return literal;
}

[[nodiscard]] cy::determinism::SimulationPoint at(u64 tick) noexcept {
    cy::determinism::SimulationPoint point;
    point.tick = tick;
    return point;
}

}  // namespace

CY_TEST_CASE("ability_activation: the interface explains itself, without activating") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    EffectDefinition burn;
    burn.name = Name::intern("burn");
    burn.stable_id = 1;
    burn.kind = EffectKind::Duration;
    burn.duration_ticks = 60;
    const EffectModifier hurt{fixture.health, ModifierOp::Add, -20.0F, 0, Name{}};
    auto effect = fixture.effects.declare(burn, cy::Span<const EffectModifier>(&hurt, 1));
    CY_REQUIRE(effect.has_value());

    AbilityDefinition fireball;
    fireball.name = Name::intern("fireball");
    fireball.stable_id = 1;
    fireball.cooldown_ticks = 90;
    const Cost cost{CostKind::Attribute, fixture.mana, 60.0F, Name{}};
    auto ability = fixture.abilities.declare(fireball, cy::Span<const Cost>(&cost, 1),
                                             cy::Span<const EffectDefId>(&effect.value(), 1));
    CY_REQUIRE(ability.has_value());
    CY_REQUIRE(fixture.abilities.grant_ability(fixture.caster, *ability, Name::intern("kit"), 0)
                   .has_value());

    ActivationRequest request;
    request.owner = fixture.caster;
    request.ability = *ability;
    request.target.kind = TargetKind::Entity;
    request.target.entity = fixture.victim;

    // VALIDATION WITHOUT ACTIVATING. Called for every button, every frame — so it commits nothing,
    // reserves nothing and emits nothing.
    CY_CHECK(fixture.pipeline.validate(request, 0).permitted());
    CY_CHECK_EQ(fixture.costs.outstanding(), 0U);
    CY_CHECK_EQ(fixture.effects.active_count(), 0U);
    CY_CHECK_EQ(fixture.attributes.current(fixture.caster, fixture.mana), 100.0F);
    CY_CHECK_EQ(fixture.pipeline.timeline().size(), 0U);

    // Not enough mana, and the answer says so WITH THE NUMBERS.
    CY_REQUIRE(fixture.attributes.set_base(fixture.caster, fixture.mana, 20.0F).has_value());
    const ValidationResult refused = fixture.pipeline.validate(request, 0);
    CY_CHECK_FALSE(refused.permitted());
    CY_CHECK_EQ(refused.first().tag, ReasonTag::InsufficientResource);
    CY_CHECK_EQ(refused.first().required, 60.0F);
    CY_CHECK_EQ(refused.first().available, 20.0F);
}

CY_TEST_CASE("ability_activation: the pipeline runs in order, and a rejection is diagnosable") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    EffectDefinition burn;
    burn.name = Name::intern("burn");
    burn.stable_id = 1;
    burn.kind = EffectKind::Duration;
    burn.duration_ticks = 60;
    const EffectModifier hurt{fixture.health, ModifierOp::Add, -20.0F, 0, Name{}};
    auto effect = fixture.effects.declare(burn, cy::Span<const EffectModifier>(&hurt, 1));
    CY_REQUIRE(effect.has_value());

    AbilityDefinition fireball;
    fireball.name = Name::intern("fireball");
    fireball.stable_id = 1;
    fireball.cooldown_ticks = 90;
    const Cost cost{CostKind::Attribute, fixture.mana, 60.0F, Name{}};
    auto ability = fixture.abilities.declare(fireball, cy::Span<const Cost>(&cost, 1),
                                             cy::Span<const EffectDefId>(&effect.value(), 1));
    CY_REQUIRE(ability.has_value());
    CY_REQUIRE(fixture.abilities.grant_ability(fixture.caster, *ability, Name::intern("kit"), 0)
                   .has_value());
    auto cue = fixture.tags.declare("Cue.Fireball.Cast");
    CY_REQUIRE(cue.has_value());

    ActivationRequest request;
    request.owner = fixture.caster;
    request.ability = *ability;
    request.target.kind = TargetKind::Entity;
    request.target.entity = fixture.victim;
    request.cue = *cue;

    // Enough mana for two casts, so the SECOND one is refused by the cooldown rather than by the
    // cost — the pipeline checks cost before cooldown, and this case is about the cooldown.
    CY_REQUIRE(fixture.attributes.set_base(fixture.caster, fixture.mana, 200.0F).has_value());

    ActivationReport report;
    auto id = fixture.pipeline.activate(request, at(10), report);
    CY_REQUIRE(id.has_value());
    CY_CHECK(report.committed);
    CY_CHECK_EQ(report.reached, AbilityStage::EmitCues);
    CY_CHECK_EQ(report.effects_applied, 1U);
    CY_CHECK_EQ(report.cues_emitted, 1U);
    // COST COMMITTED, COOLDOWN STARTED AS A TICK, EFFECT APPLIED.
    CY_CHECK_EQ(fixture.attributes.current(fixture.caster, fixture.mana), 140.0F);
    CY_CHECK_EQ(fixture.abilities.ready_tick(fixture.caster, *ability), 100);
    CY_CHECK_EQ(fixture.effects.active_count(), 1U);
    CY_CHECK_EQ(fixture.attributes.current(fixture.victim, fixture.health), 180.0F);
    CY_CHECK_EQ(fixture.costs.outstanding(), 0U);

    // The second activation in the same tick is refused at the cooldown stage, and the record says
    // which stage and with which numbers.
    ActivationReport second;
    auto refused = fixture.pipeline.activate(request, at(10), second);
    CY_REQUIRE(refused.has_value());
    CY_CHECK_FALSE(second.committed);
    CY_CHECK_EQ(second.reached, AbilityStage::CheckCooldown);
    CY_CHECK_EQ(second.validation.first().tag, ReasonTag::Cooldown);
    CY_CHECK_EQ(second.validation.first().required, 100.0F);

    // THE ACTIVATION TIMELINE holds both, by tick, with their results.
    CY_REQUIRE_EQ(fixture.pipeline.timeline().size(), 2U);
    CY_CHECK(fixture.pipeline.timeline()[0].committed);
    CY_CHECK_FALSE(fixture.pipeline.timeline()[1].committed);
    CY_CHECK_EQ(fixture.pipeline.timeline()[1].reason.tag, ReasonTag::Cooldown);
    // Two activations of one ability in one tick are DISTINGUISHABLE.
    CY_CHECK_NE(id->bits, refused->bits);
}

CY_TEST_CASE("ability_activation: an agent and a player face the same validation") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    AbilityDefinition strike;
    strike.name = Name::intern("strike");
    strike.stable_id = 1;
    strike.targeting.max_range = 12.0F;
    strike.targeting.require_line_of_sight = true;
    strike.planning.range = 12.0F;
    strike.planning.expected_magnitude = 20.0F;
    auto ability =
        fixture.abilities.declare(strike, cy::Span<const Cost>(), cy::Span<const EffectDefId>());
    CY_REQUIRE(ability.has_value());
    CY_REQUIRE(fixture.abilities.grant_ability(fixture.caster, *ability, Name::intern("kit"), 0)
                   .has_value());

    // ACQUISITION AND VALIDATION ARE SEPARATE. The agent picks by proximity; the target it picks
    // then faces exactly what a player's cursor target would.
    const Entity candidates[3] = {entity(2), entity(3), entity(9)};
    AcquisitionRequest acquisition;
    acquisition.how = Acquisition::Proximity;
    acquisition.instigator = fixture.caster;
    acquisition.radius = 12.0F;
    acquisition.candidates = cy::Span<const Entity>(candidates, 3);
    auto acquired = acquire_target(acquisition, fixture.target_context, fixture.buffer);
    CY_REQUIRE(acquired.has_value());
    CY_CHECK_EQ(acquired->kind, TargetKind::EntitySet);
    // Entities 2 and 3 are five and ten units away; entity 9 is forty and is not selected.
    CY_CHECK_EQ(acquired->member_count, 2U);

    ActivationRequest agent_request;
    agent_request.owner = fixture.caster;
    agent_request.ability = *ability;
    agent_request.target = *acquired;
    CY_CHECK(fixture.pipeline.validate(agent_request, 0).permitted());

    ActivationRequest player_request = agent_request;
    player_request.target = TargetData{};
    player_request.target.kind = TargetKind::Entity;
    player_request.target.entity = entity(9);
    player_request.target.acquisition = Acquisition::Cursor;
    const ValidationResult refused = fixture.pipeline.validate(player_request, 0);
    CY_CHECK_FALSE(refused.permitted());
    CY_CHECK_EQ(refused.first().tag, ReasonTag::OutOfRange);
    CY_CHECK_EQ(refused.first().required, 12.0F);

    // Line of sight is part of the same rules, and the same call answers it.
    fixture.target_context.line_of_sight = &World::never_visible;
    fixture.pipeline.set_target_context(fixture.target_context);
    const ValidationResult blocked = fixture.pipeline.validate(agent_request, 0);
    CY_CHECK_FALSE(blocked.permitted());
    CY_CHECK_EQ(blocked.first().detail, Name::intern("line_of_sight"));
}

CY_TEST_CASE("ability_activation: a rejected prediction is reverted and reportable") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    EffectDefinition slow;
    slow.name = Name::intern("slow");
    slow.stable_id = 1;
    slow.kind = EffectKind::Duration;
    slow.duration_ticks = 100;
    const EffectModifier hurt{fixture.health, ModifierOp::Add, -10.0F, 0, Name{}};
    auto effect = fixture.effects.declare(slow, cy::Span<const EffectModifier>(&hurt, 1));
    CY_REQUIRE(effect.has_value());

    AbilityDefinition bolt;
    bolt.name = Name::intern("bolt");
    bolt.stable_id = 1;
    bolt.prediction = PredictionPolicy::ClientPredicted;
    auto ability = fixture.abilities.declare(bolt, cy::Span<const Cost>(),
                                             cy::Span<const EffectDefId>(&effect.value(), 1));
    CY_REQUIRE(ability.has_value());
    AbilityDefinition server_only;
    server_only.name = Name::intern("summon");
    server_only.stable_id = 2;
    server_only.prediction = PredictionPolicy::AuthorityOnly;
    auto guarded = fixture.abilities.declare(server_only, cy::Span<const Cost>(),
                                             cy::Span<const EffectDefId>());
    CY_REQUIRE(guarded.has_value());
    CY_REQUIRE(fixture.abilities.grant_ability(fixture.caster, *ability, Name::intern("kit"), 0)
                   .has_value());
    CY_REQUIRE(fixture.abilities.grant_ability(fixture.caster, *guarded, Name::intern("kit"), 0)
                   .has_value());
    auto cue = fixture.tags.declare("Cue.Bolt");
    CY_REQUIRE(cue.has_value());

    // PREDICTION DOES NOT BYPASS VALIDATION: predicting an authority-only ability is refused at the
    // policy stage, before anything is committed.
    ActivationRequest illegal;
    illegal.owner = fixture.caster;
    illegal.ability = *guarded;
    illegal.predicted = true;
    ActivationReport report;
    auto refused = fixture.pipeline.activate(illegal, at(5), report);
    CY_REQUIRE(refused.has_value());
    CY_CHECK_FALSE(report.committed);
    CY_CHECK_EQ(report.reached, AbilityStage::PredictionPolicy);
    CY_CHECK_EQ(report.validation.first().detail, Name::intern("not_predictable"));

    ActivationRequest predicted;
    predicted.owner = fixture.caster;
    predicted.ability = *ability;
    predicted.target.kind = TargetKind::Entity;
    predicted.target.entity = fixture.victim;
    predicted.predicted = true;
    predicted.cue = *cue;
    ActivationReport predicted_report;
    auto id = fixture.pipeline.activate(predicted, at(20), predicted_report);
    CY_REQUIRE(id.has_value());
    CY_CHECK(predicted_report.committed);
    CY_CHECK_EQ(fixture.effects.active_count(), 1U);
    CY_REQUIRE_EQ(fixture.pipeline.cues().size(), 1U);
    // A predicted cue is SPECULATIVE and says so, so a consumer can decline to realise it.
    CY_CHECK(fixture.pipeline.cues()[0].speculative);

    CY_REQUIRE(fixture.pipeline.reconcile(*id, false).has_value());
    // THE PREDICTED EFFECT IS UNDONE and the cause is recorded.
    CY_CHECK_EQ(fixture.effects.active_count(), 0U);
    const ActivationRecord* record = fixture.pipeline.record(*id);
    CY_REQUIRE_NE(record, nullptr);
    CY_CHECK(record->reverted);
    CY_CHECK_EQ(record->cancellation, CancellationCause::AuthorityRejected);
}

CY_TEST_CASE("ability_activation: a re-simulated activation does not play its cue twice") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    AbilityDefinition shout;
    shout.name = Name::intern("shout");
    shout.stable_id = 1;
    auto ability =
        fixture.abilities.declare(shout, cy::Span<const Cost>(), cy::Span<const EffectDefId>());
    CY_REQUIRE(ability.has_value());
    CY_REQUIRE(fixture.abilities.grant_ability(fixture.caster, *ability, Name::intern("kit"), 0)
                   .has_value());
    auto cue = fixture.tags.declare("Cue.Shout");
    CY_REQUIRE(cue.has_value());

    // The same activation, derived the same way, at the same simulation point. A rollback that
    // replays tick 30 produces it again; the ledger suppresses the repeat.
    const ActivationId id = ActivationPipeline::derive_id(fixture.caster, 1, at(30), 0);
    CY_CHECK(id.valid());
    CY_CHECK_EQ(id.bits, ActivationPipeline::derive_id(fixture.caster, 1, at(30), 0).bits);
    CY_CHECK_NE(id.bits, ActivationPipeline::derive_id(fixture.caster, 1, at(31), 0).bits);

    ActivationRequest request;
    request.owner = fixture.caster;
    request.ability = *ability;
    request.cue = *cue;
    ActivationReport first;
    CY_REQUIRE(fixture.pipeline.activate(request, at(30), first).has_value());
    CY_CHECK_EQ(first.cues_emitted, 1U);

    // RE-SIMULATION. A rollback replays the recorded command WITH ITS RECORDED IDENTITY, and the
    // ledger recognises the (activation, cue, simulation point) triple it already emitted.
    ActivationRequest replayed = request;
    replayed.identity = first.id;
    ActivationReport second;
    auto again = fixture.pipeline.activate(replayed, at(30), second);
    CY_REQUIRE(again.has_value());
    CY_CHECK(second.committed);
    CY_CHECK_EQ(again->bits, first.id.bits);
    CY_CHECK_EQ(second.cues_emitted, 0U);
    CY_CHECK_EQ(second.cues_suppressed, 1U);
    // ONE CUE, NOT TWO. The cast does not play twice.
    CY_CHECK_EQ(fixture.pipeline.cues().size(), 1U);
    CY_CHECK_EQ(fixture.pipeline.cues_suppressed(), 1U);

    // A genuinely different activation at a different tick is not suppressed.
    ActivationReport third;
    CY_REQUIRE(fixture.pipeline.activate(request, at(31), third).has_value());
    CY_CHECK_EQ(third.cues_emitted, 1U);
    CY_CHECK_EQ(fixture.pipeline.cues().size(), 2U);
}

CY_TEST_CASE("ability_activation: randomness comes from the activation, and replays") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    AbilityDefinition strike;
    strike.name = Name::intern("strike");
    strike.stable_id = 3;
    auto ability =
        fixture.abilities.declare(strike, cy::Span<const Cost>(), cy::Span<const EffectDefId>());
    CY_REQUIRE(ability.has_value());

    const ActivationId first = ActivationPipeline::derive_id(fixture.caster, 3, at(42), 0);
    const ActivationId second = ActivationPipeline::derive_id(fixture.caster, 3, at(42), 1);
    const cy::f32 a = fixture.pipeline.stream_for(first, *ability).unit_float(at(42), 0, 0);
    const cy::f32 b = fixture.pipeline.stream_for(second, *ability).unit_float(at(42), 0, 0);
    // Two activations of one ability in one tick draw INDEPENDENTLY, and each is reproducible.
    CY_CHECK_NE(a, b);
    CY_CHECK_EQ(a, fixture.pipeline.stream_for(first, *ability).unit_float(at(42), 0, 0));

    // A second session with the same seed reproduces both. That is "a critical hit replays".
    GameplayRandom replay(0x5EEDULL);
    ActivationPipeline replayed(allocator(), fixture.abilities, fixture.effects, fixture.attributes,
                                fixture.costs, fixture.tag_store, fixture.tags,
                                fixture.relationships, replay);
    CY_CHECK_EQ(replayed.stream_for(first, *ability).unit_float(at(42), 0, 0), a);
}

CY_TEST_CASE("ability_activation: a compiled ability program refuses at its own stage") {
    // THE SEAM TO M8.b SECTION 2. The graph is CyberGraph's, the compiler is
    // `cy::graph::script::compile_ability`, and this pipeline runs its CHECK stages through
    // `AbilityScriptHost` — one language, two consumers, exactly as the spike found.
    using namespace cy::graph;
    NodeRegistry registry(allocator());
    CY_REQUIRE(script::register_script_nodes(registry).has_value());
    CY_REQUIRE(script::register_ability_nodes(registry).has_value());

    Graph graph(allocator(), Name::intern("frostbolt"));
    CY_REQUIRE(graph.add_node(1, Name::intern("ability.stage")).has_value());
    CY_REQUIRE(graph.set_property(1, Name::intern("stage"), text("check_state")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("ability.refuse")).has_value());
    CY_REQUIRE(graph.set_property(2, Name::intern("reason"), text("silenced")).has_value());
    CY_REQUIRE(graph.connect(1, Name::intern("then"), 2, Name::intern("in")).has_value());
    CY_REQUIRE(graph.add_node(3, Name::intern("ability.stage")).has_value());
    CY_REQUIRE(graph.set_property(3, Name::intern("stage"), text("commit")).has_value());
    CY_REQUIRE(graph.add_node(4, Name::intern("script.emit_command")).has_value());
    CY_REQUIRE(graph.set_property(4, Name::intern("command"), text("cast")).has_value());
    CY_REQUIRE(graph.connect(3, Name::intern("then"), 4, Name::intern("in")).has_value());
    graph.resolve(registry);

    DiagnosticSink sink(allocator());
    auto program = script::compile_ability(graph, registry, sink);
    CY_REQUIRE(program.has_value());

    Fixture fixture;
    CY_REQUIRE(fixture.build());
    AbilityDefinition frostbolt;
    frostbolt.name = Name::intern("frostbolt");
    frostbolt.stable_id = 1;
    frostbolt.program = &program.value();
    auto ability =
        fixture.abilities.declare(frostbolt, cy::Span<const Cost>(), cy::Span<const EffectDefId>());
    CY_REQUIRE(ability.has_value());
    CY_REQUIRE(fixture.abilities.grant_ability(fixture.caster, *ability, Name::intern("kit"), 0)
                   .has_value());

    ActivationRequest request;
    request.owner = fixture.caster;
    request.ability = *ability;

    // The graph's own check stage refuses, and the reason it named travels out through the same
    // structured result everything else uses.
    const ValidationResult refused = fixture.pipeline.validate(request, 0);
    CY_CHECK_FALSE(refused.permitted());
    CY_CHECK_EQ(refused.first().detail, Name::intern("silenced"));

    // NOTHING WAS ACTIVATED: the commit stage emits a command and no command was emitted.
    AbilityScriptHost host(fixture.attributes, fixture.tag_store, fixture.tags, fixture.abilities,
                           fixture.caster, *ability, 0);
    script::ScriptState state(allocator(), program.value().program());
    auto verdict = script::validate_activation(program.value(), state, host);
    CY_REQUIRE(verdict.has_value());
    CY_CHECK_FALSE(verdict.value().allowed);
    CY_CHECK_EQ(host.commands(), 0U);
    CY_CHECK_EQ(host.events(), 0U);

    // And the pipeline refuses the activation too, with nothing committed.
    ActivationReport report;
    CY_REQUIRE(fixture.pipeline.activate(request, at(0), report).has_value());
    CY_CHECK_FALSE(report.committed);
    CY_CHECK_EQ(fixture.effects.active_count(), 0U);
}
