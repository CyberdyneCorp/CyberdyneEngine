// M8.b tasks 3.1, 3.2 and 3.3 — composable rules, phases as tags, time domains and their timers,
// typed events with declared delivery, and gameplay features.

#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/command.h>
#include <cy/gameplay/events.h>
#include <cy/gameplay/features.h>
#include <cy/gameplay/rules.h>
#include <cy/gameplay/tags.h>
#include <cy/gameplay/time.h>
#include <cy/test/test.h>

#include <type_traits>

using namespace cy::gameplay;
using cy::Name;
using cy::u32;
using cy::u64;
using cy::ecs::Entity;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

}  // namespace

CY_TEST_CASE("gameplay_rules: a skirmish mode is a composition, not a subclass") {
    RulesAsset skirmish(allocator(), Name::intern("skirmish"));
    auto teams = skirmish.add_piece(RuleKind::TeamFormation, Name::intern("balanced"));
    CY_REQUIRE(teams.has_value());
    CY_REQUIRE(skirmish.set_parameter(*teams, Name::intern("team_count"), 2.0).has_value());
    auto victory = skirmish.add_piece(RuleKind::Victory, Name::intern("elimination"));
    CY_REQUIRE(victory.has_value());
    CY_REQUIRE(skirmish.set_parameter(*victory, Name::intern("score_limit"), 50.0).has_value());
    auto economy = skirmish.add_piece(RuleKind::Economy, Name::intern("resources"));
    CY_REQUIRE(economy.has_value());
    CY_REQUIRE(skirmish.set_parameter(*economy, Name::intern("starting_ore"), 500.0).has_value());
    auto spawn = skirmish.add_piece(RuleKind::SpawnSelection, Name::intern("nearest_safe"),
                                    Name::intern("cy.rules.nearest_safe"));
    CY_REQUIRE(spawn.has_value());

    CY_CHECK_EQ(skirmish.piece_count(), 4U);
    CY_CHECK_EQ(skirmish.find(RuleKind::Victory), *victory);
    CY_CHECK_EQ(skirmish.number(*economy, Name::intern("starting_ore")), 500.0);
    CY_CHECK_EQ(skirmish.piece_at(*spawn).implementation, Name::intern("cy.rules.nearest_safe"));

    // A DESIGNER VARIES PARAMETERS. The second mode is the same composition with two numbers
    // changed, and its digest differs — which is what makes "a rules asset" a thing a cook key can
    // name.
    const u64 first = skirmish.digest();
    RulesAsset rich(allocator(), Name::intern("skirmish"));
    auto rich_teams = rich.add_piece(RuleKind::TeamFormation, Name::intern("balanced"));
    CY_REQUIRE(rich_teams.has_value());
    CY_REQUIRE(rich.set_parameter(*rich_teams, Name::intern("team_count"), 2.0).has_value());
    auto rich_victory = rich.add_piece(RuleKind::Victory, Name::intern("elimination"));
    CY_REQUIRE(rich_victory.has_value());
    CY_REQUIRE(rich.set_parameter(*rich_victory, Name::intern("score_limit"), 50.0).has_value());
    auto rich_economy = rich.add_piece(RuleKind::Economy, Name::intern("resources"));
    CY_REQUIRE(rich_economy.has_value());
    CY_REQUIRE(rich.set_parameter(*rich_economy, Name::intern("starting_ore"), 2000.0).has_value());
    auto rich_spawn = rich.add_piece(RuleKind::SpawnSelection, Name::intern("nearest_safe"),
                                     Name::intern("cy.rules.nearest_safe"));
    CY_REQUIRE(rich_spawn.has_value());
    CY_CHECK_NE(rich.digest(), first);
}

CY_TEST_CASE("gameplay_phases: a game adds overtime without modifying an engine type") {
    TagRegistry tags(allocator());
    auto warmup = tags.declare("Match.Warmup");
    auto play = tags.declare("Match.Play");
    auto overtime = tags.declare("Match.Play.Overtime");
    auto results = tags.declare("Match.Results");
    CY_REQUIRE(warmup.has_value());
    CY_REQUIRE(play.has_value());
    CY_REQUIRE(overtime.has_value());
    CY_REQUIRE(results.has_value());

    PhaseController phases(allocator());
    CY_REQUIRE(phases.allow(kInvalidTag, *warmup).has_value());
    CY_REQUIRE(phases.allow(*warmup, *play).has_value());
    CY_REQUIRE(phases.allow(*play, *overtime).has_value());
    CY_REQUIRE(phases.allow(*overtime, *results).has_value());
    auto aborted = tags.declare("Match.Aborted");
    CY_REQUIRE(aborted.has_value());
    CY_REQUIRE(phases.allow_from_any(*aborted).has_value());

    CY_CHECK(phases.enter(*warmup, 100).permitted());
    CY_CHECK(phases.enter(*play, 240).permitted());
    // Warmup is reachable only from the initial state — `kInvalidTag` as a source is where a
    // session starts, NOT a wildcard — so the refusal says why rather than returning false.
    const ValidationResult refused = phases.enter(*warmup, 300);
    CY_CHECK_FALSE(refused.permitted());
    CY_CHECK_EQ(refused.first().tag, ReasonTag::WrongPhase);
    CY_CHECK_EQ(phases.current(), *play);

    // A SYSTEM DECLARED FOR Match.Play IS ACTIVE IN OVERTIME. It does not know overtime exists,
    // which is the whole reason a phase is a hierarchical tag rather than an enumerator.
    CY_CHECK(phases.enter(*overtime, 900).permitted());
    CY_CHECK(phases.active_in(tags, *play));
    CY_CHECK(phases.active_in(tags, *overtime));
    CY_CHECK_FALSE(phases.active_in(tags, *results));
    CY_CHECK(phases.active_in(tags, kInvalidTag));

    // TRANSITIONS ARE RECORDED WITH THEIR TICK, so replay and peers agree on when they happened.
    // An abort is reachable from anywhere, and that is said explicitly rather than falling out of
    // the initial-state rule.
    CY_CHECK(phases.check(*aborted).permitted());

    CY_REQUIRE_EQ(phases.event_count(), 5U);
    CY_CHECK_EQ(phases.event_at(0).kind, PhaseEventKind::Entering);
    CY_CHECK_EQ(phases.event_at(0).tick, 100U);
    CY_CHECK_EQ(phases.event_at(1).kind, PhaseEventKind::Leaving);
    CY_CHECK_EQ(phases.event_at(1).phase, *warmup);
    CY_CHECK_EQ(phases.event_at(4).tick, 900U);
}

CY_TEST_CASE("gameplay_time: menus animate while gameplay is paused, and slow motion is scoped") {
    TimeDomains domains(allocator());
    const DomainId gameplay = domains.builtin(TimeDomainKind::Gameplay);
    const DomainId interface_domain = domains.builtin(TimeDomainKind::Interface);
    const DomainId real = domains.builtin(TimeDomainKind::Real);
    CY_REQUIRE_NE(gameplay, kInvalidDomain);

    CY_REQUIRE(domains.set_paused(gameplay, true).has_value());
    domains.advance(1.0F / 60.0F);
    CY_CHECK_EQ(domains.delta(gameplay), 0.0F);
    CY_CHECK_GT(domains.delta(interface_domain), 0.0F);
    // Real time is neither scaled nor paused, and says so by refusing rather than ignoring.
    CY_CHECK_GT(domains.delta(real), 0.0F);
    CY_CHECK_FALSE(domains.set_paused(real, true).has_value());
    CY_CHECK_FALSE(domains.set_scale(real, 0.5F).has_value());

    CY_REQUIRE(domains.set_paused(gameplay, false).has_value());
    CY_REQUIRE(domains.set_scale(gameplay, 0.5F).has_value());
    domains.advance(1.0F / 60.0F);
    CY_CHECK_NEAR(domains.delta(gameplay), domains.delta(interface_domain) * 0.5F, 1e-6F);

    // A session may forbid pausing entirely, and the refusal is reported.
    domains.set_pause_allowed(false);
    CY_CHECK_FALSE(domains.set_paused(gameplay, true).has_value());
    CY_CHECK_FALSE(domains.paused(gameplay));

    // A project's own domain, declared without changing an engine enumeration.
    auto audio = domains.declare(Name::intern("audio"), true);
    CY_REQUIRE(audio.has_value());
    CY_CHECK_EQ(domains.find(Name::intern("audio")), *audio);
}

CY_TEST_CASE("gameplay_time: a delayed effect fires at an exact tick, and only then") {
    TimerWheel wheel(allocator());
    auto first = wheel.schedule(30, 111);
    auto second = wheel.schedule(30, 222);
    auto later = wheel.schedule(90, 333);
    auto cancelled = wheel.schedule(30, 444);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(later.has_value());
    CY_REQUIRE(cancelled.has_value());
    CY_CHECK(wheel.cancel(*cancelled));

    TimerExpiry fired[8] = {};
    CY_CHECK_EQ(wheel.advance_to(29, fired, 8), 0U);
    CY_CHECK_EQ(wheel.last_examined(), 0U);

    CY_REQUIRE_EQ(wheel.advance_to(30, fired, 8), 2U);
    // Within a tick, insertion order — declared, because a replay has to reproduce it.
    CY_CHECK_EQ(fired[0].user, 111U);
    CY_CHECK_EQ(fired[1].user, 222U);
    CY_CHECK_EQ(fired[0].due_tick, 30U);
    CY_CHECK_EQ(wheel.advance_to(89, fired, 8), 0U);
    CY_REQUIRE_EQ(wheel.advance_to(90, fired, 8), 1U);
    CY_CHECK_EQ(fired[0].user, 333U);
    CY_CHECK_EQ(wheel.pending(), 0U);
}

CY_TEST_CASE("gameplay_time: timers are per domain") {
    TimeDomains domains(allocator());
    TimerService timers(allocator(), domains);
    const DomainId gameplay = domains.builtin(TimeDomainKind::Gameplay);
    const DomainId interface_domain = domains.builtin(TimeDomainKind::Interface);
    CY_REQUIRE(timers.add_domain(gameplay).has_value());
    CY_REQUIRE(timers.add_domain(interface_domain).has_value());
    CY_REQUIRE_NE(timers.wheel(gameplay), nullptr);
    CY_CHECK_NE(timers.wheel(gameplay), timers.wheel(interface_domain));
    CY_CHECK_EQ(timers.wheel(kInvalidDomain), nullptr);

    CY_REQUIRE(timers.wheel(gameplay)->schedule(10, 1).has_value());
    TimerExpiry fired[2] = {};
    // Advancing one domain's wheel does not advance another's: a paused gameplay domain's timers
    // do not fire because the interface animated.
    CY_CHECK_EQ(timers.wheel(interface_domain)->advance_to(100, fired, 2), 0U);
    CY_CHECK_EQ(timers.wheel(gameplay)->advance_to(100, fired, 2), 1U);
}

CY_TEST_CASE("gameplay_events: damage does not cascade through synchronous calls") {
    EventBus bus(allocator());
    TagRegistry tags(allocator());
    auto combat = tags.declare("Message.Combat");
    CY_REQUIRE(combat.has_value());

    EventDeclaration damaged;
    damaged.name = Name::intern("Damaged");
    damaged.stable_id = 1;
    damaged.channel = *combat;
    damaged.replay_relevant = false;
    damaged.network_relevant = true;
    auto type = bus.declare(damaged);
    CY_REQUIRE(type.has_value());
    auto producer = bus.open_producer(Name::intern("combat"));
    CY_REQUIRE(producer.has_value());

    GameplayEvent event;
    event.type = *type;
    event.target = EventTarget::Entity;
    event.entity = Entity::make(4, 1);
    event.tick = 42;
    CY_REQUIRE(bus.publish(*producer, event).has_value());

    // NOTHING WAS DELIVERED. The default is phase-buffered, so publishing is not a call into
    // whatever happens to listen.
    CY_CHECK_EQ(bus.delivered_count(), 0U);
    CY_CHECK_EQ(bus.pending_count(), 1U);
    CY_CHECK_EQ(bus.deliver(Delivery::NextTick), 0U);
    CY_CHECK_EQ(bus.deliver(Delivery::EndOfPhase), 1U);
    CY_CHECK_EQ(bus.delivered_count(), 1U);
    CY_CHECK_EQ(bus.pending_count(), 0U);

    GameplayEvent seen[4] = {};
    CY_CHECK_EQ(bus.addressed_to_entity(Entity::make(4, 1), seen, 4), 1U);
    CY_CHECK_EQ(bus.addressed_to_entity(Entity::make(5, 1), seen, 4), 0U);
    CY_CHECK(bus.network_relevant(*type));
    CY_CHECK_FALSE(bus.replay_relevant(*type));
}

CY_TEST_CASE("gameplay_events: the interface observes a tagged channel, not a system") {
    EventBus bus(allocator());
    TagRegistry tags(allocator());
    auto objective = tags.declare("Message.Objective.Complete");
    auto message = tags.declare("Message");
    CY_REQUIRE(objective.has_value());
    CY_REQUIRE(message.has_value());

    EventDeclaration completed;
    completed.name = Name::intern("ObjectiveComplete");
    completed.stable_id = 7;
    completed.channel = *objective;
    completed.delivery = Delivery::ImmediateLocal;
    auto type = bus.declare(completed);
    CY_REQUIRE(type.has_value());
    auto producer = bus.open_producer(Name::intern("objectives"));
    CY_REQUIRE(producer.has_value());

    GameplayEvent event;
    event.type = *type;
    event.tick = 9;
    CY_REQUIRE(bus.publish(*producer, event).has_value());
    // Immediate-local really is immediate; it is the exception the declaration names, and the
    // default is still the buffered one.
    CY_CHECK_EQ(bus.delivered_count(), 1U);

    GameplayEvent seen[2] = {};
    // The interface subscribes to `Message` and receives what was published on
    // `Message.Objective.Complete`, without naming the system that produced it.
    CY_CHECK_EQ(bus.observe(tags, *message, seen, 2), 1U);
    CY_CHECK_EQ(bus.observe(tags, tags.find("Message.Combat"), seen, 2), 0U);
}

CY_TEST_CASE("gameplay_events: a command and an event are not the same type") {
    // `gameplay-framework` forbids conflating them, and the way they get conflated is a helper
    // somebody adds. These are the assertions that fail the moment one appears.
    CY_CHECK_FALSE((std::is_convertible_v<GameplayEvent, Command>));
    CY_CHECK_FALSE((std::is_convertible_v<Command, GameplayEvent>));
    CY_CHECK_FALSE((std::is_constructible_v<Command, GameplayEvent>));
    CY_CHECK_FALSE((std::is_constructible_v<GameplayEvent, Command>));
}

CY_TEST_CASE("gameplay_features: a mode is a set of features, activated in dependency order") {
    FeatureRegistry features(allocator());
    auto base = features.register_feature(Name::intern("base_strategy"));
    auto economy = features.register_feature(Name::intern("economy"));
    auto fog = features.register_feature(Name::intern("fog_of_war"));
    auto victory = features.register_feature(Name::intern("victory_domination"));
    CY_REQUIRE(base.has_value());
    CY_REQUIRE(economy.has_value());
    CY_REQUIRE(fog.has_value());
    CY_REQUIRE(victory.has_value());
    CY_REQUIRE(features.declare_dependency(*economy, Name::intern("base_strategy")).has_value());
    CY_REQUIRE(features.declare_dependency(*victory, Name::intern("economy")).has_value());
    CY_REQUIRE(features.declare_dependency(*victory, Name::intern("fog_of_war")).has_value());
    CY_REQUIRE(
        features.contribute(*economy, ContributionKind::System, Name::intern("HarvestSystem"))
            .has_value());
    CY_REQUIRE(features.contribute(*fog, ContributionKind::System, Name::intern("VisibilitySystem"))
                   .has_value());

    FeatureId order[8] = {};
    auto count = features.resolve_order(*victory, order, 8);
    CY_REQUIRE(count.has_value());
    CY_REQUIRE_EQ(*count, 4U);
    CY_CHECK_EQ(order[0], *base);
    CY_CHECK_EQ(order[1], *economy);
    CY_CHECK_EQ(order[3], *victory);

    CY_REQUIRE(features.activate(*victory).has_value());
    CY_CHECK_EQ(features.state(*base), FeatureState::Activated);
    CY_CHECK_EQ(features.state(*victory), FeatureState::Activated);

    Contribution systems[8] = {};
    CY_CHECK_EQ(features.active_contributions(ContributionKind::System, systems, 8), 2U);
    // A feature something active still depends on is not deactivated out from under it.
    CY_CHECK_FALSE(features.deactivate(*economy).has_value());
    CY_REQUIRE(features.deactivate(*victory).has_value());
    CY_CHECK(features.deactivate(*economy).has_value());
}

CY_TEST_CASE("gameplay_features: a missing dependency or a cycle activates nothing") {
    FeatureRegistry features(allocator());
    auto mode = features.register_feature(Name::intern("mode"));
    CY_REQUIRE(mode.has_value());
    CY_REQUIRE(features.declare_dependency(*mode, Name::intern("absent")).has_value());
    CY_CHECK_FALSE(features.activate(*mode).has_value());
    // NOTHING WAS ACTIVATED. A partial activation is a state nobody wrote code for.
    CY_CHECK_EQ(features.state(*mode), FeatureState::Installed);

    FeatureRegistry cyclic(allocator());
    auto first = cyclic.register_feature(Name::intern("first"));
    auto second = cyclic.register_feature(Name::intern("second"));
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(cyclic.declare_dependency(*first, Name::intern("second")).has_value());
    CY_REQUIRE(cyclic.declare_dependency(*second, Name::intern("first")).has_value());
    CY_CHECK_FALSE(cyclic.resolve_order(*first, nullptr, 0).has_value());
    CY_CHECK_FALSE(cyclic.activate(*first).has_value());
    CY_CHECK_EQ(cyclic.state(*first), FeatureState::Installed);
}
