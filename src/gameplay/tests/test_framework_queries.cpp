// M8.b tasks 3.2 and 3.3 — the interaction framework, the derived indexes, and gameplay
// diagnostics.

#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/diagnostics.h>
#include <cy/gameplay/indexes.h>
#include <cy/gameplay/interaction.h>
#include <cy/test/test.h>

using namespace cy::gameplay;
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

}  // namespace

CY_TEST_CASE("gameplay_interaction: the interface and an agent read the same options") {
    TagRegistry tags(allocator());
    EntityTagStore tag_store(allocator());
    auto mine = tags.declare("Interact.Mine");
    auto repair = tags.declare("Interact.Repair");
    auto inspect = tags.declare("Interact.Inspect");
    auto builder = tags.declare("Role.Builder");
    CY_REQUIRE(mine.has_value());
    CY_REQUIRE(repair.has_value());
    CY_REQUIRE(inspect.has_value());
    CY_REQUIRE(builder.has_value());

    InteractionRegistry interactions(allocator());
    const Entity node = entity(50);
    CY_REQUIRE(interactions.add_interactable(node, Vec3{0.0F, 0.0F, 0.0F}).has_value());
    InteractionOption option;
    option.action = *mine;
    option.display_text = Name::intern("Mine");
    option.range = 3.0F;
    option.command = 1;
    CY_REQUIRE(interactions.add_option(node, option).has_value());
    option.action = *repair;
    option.display_text = Name::intern("Repair");
    option.required_tag = *builder;
    CY_REQUIRE(interactions.add_option(node, option).has_value());
    option.action = *inspect;
    option.display_text = Name::intern("Inspect");
    option.required_tag = kInvalidTag;
    CY_REQUIRE(interactions.add_option(node, option).has_value());

    const Entity human = entity(1);
    const Entity agent = entity(2);
    CY_REQUIRE(tag_store.add(human, *builder).has_value());

    cy::Array<InteractionCandidate> results(allocator());
    InteractionQuery queries[2] = {};
    queries[0].interactor = human;
    queries[0].origin = Vec3{1.0F, 0.0F, 0.0F};
    queries[0].radius = 4.0F;
    queries[1].interactor = agent;
    queries[1].origin = Vec3{1.0F, 0.0F, 0.0F};
    queries[1].radius = 4.0F;
    InteractionQueryReport report;
    CY_REQUIRE(
        interactions
            .query_batch(cy::Span<InteractionQuery>(queries, 2), tag_store, tags, results, report)
            .has_value());

    // The human, who is a builder, sees three; the agent, who is not, sees two — and both read the
    // SAME options through the same call.
    CY_CHECK_EQ(queries[0].result_count, 3U);
    CY_CHECK_EQ(queries[1].result_count, 2U);
    CY_CHECK_EQ(report.queries, 2U);

    // SELECTING ONE PRODUCES A COMMAND. It does not perform anything: intent reaches the
    // simulation only through the command stream.
    const auto participant = ParticipantId::from_slot(0, 1);
    const auto source = ControlSourceId::from_slot(0, 1);
    auto command = interactions.select(results[queries[0].first_result], participant, source);
    CY_REQUIRE(command.has_value());
    CY_CHECK_EQ(command->type, 1U);
    CY_CHECK_EQ(command->target, node);
    CY_CHECK_EQ(command->participant, participant);
}

CY_TEST_CASE("gameplay_indexes: an index discarded and rebuilt is identical") {
    OwnershipRegistry ownership(allocator());
    RelationshipService relationships(allocator());
    TagRegistry tags(allocator());
    EntityTagStore tag_store(allocator());
    auto team = relationships.add_team(Name::intern("red"));
    auto robot = tags.declare("Unit.Robot");
    auto harvester = tags.declare("Unit.Robot.Harvester");
    CY_REQUIRE(team.has_value());
    CY_REQUIRE(robot.has_value());
    CY_REQUIRE(harvester.has_value());
    const auto owner = ParticipantId::from_slot(0, 1);

    GameplayIndexes indexes(allocator());
    for (u32 index = 1; index <= 32; ++index) {
        const Entity subject = entity(index);
        CY_REQUIRE(ownership.set_owner(subject, Owner::of_participant(owner)).has_value());
        CY_REQUIRE(relationships.set_team(subject, *team).has_value());
        CY_REQUIRE(relationships.add_affiliation(subject, Name::intern("squad"), 2).has_value());
        const TagId tag = (index % 2) == 0 ? *harvester : *robot;
        CY_REQUIRE(tag_store.add(subject, tag).has_value());
        CY_REQUIRE(indexes.on_owner_changed(subject, owner).has_value());
        CY_REQUIRE(indexes.on_team_changed(subject, *team).has_value());
        CY_REQUIRE(indexes.on_affiliation_changed(subject, Name::intern("squad"), 2).has_value());
        CY_REQUIRE(indexes.on_tag_added(subject, tag).has_value());
    }

    const u64 incremental = indexes.digest();
    CY_CHECK_EQ(indexes.owned_by(owner, nullptr, 0), 32U);
    CY_CHECK_EQ(indexes.on_team(*team, nullptr, 0), 32U);
    CY_CHECK_EQ(indexes.affiliated(Name::intern("squad"), 2, nullptr, 0), 32U);
    CY_CHECK_EQ(indexes.tagged(*harvester, nullptr, 0), 16U);
    // Hierarchical: everything tagged Unit.Robot.Harvester answers a Unit.Robot query.
    CY_CHECK_EQ(indexes.tagged_matching(tags, *robot, nullptr, 0), 32U);

    CY_REQUIRE(indexes.rebuild(ownership, relationships, tag_store).has_value());
    // THE CACHE CLAIM, EXECUTED. Discarding it and deriving it again produces the same value.
    CY_CHECK_EQ(indexes.digest(), incremental);
    CY_CHECK_EQ(indexes.owned_by(owner, nullptr, 0), 32U);
    CY_CHECK_EQ(indexes.tagged_matching(tags, *robot, nullptr, 0), 32U);
}

CY_TEST_CASE("gameplay_indexes: an ownership query touches one bucket") {
    GameplayIndexes indexes(allocator());
    const auto first = ParticipantId::from_slot(0, 1);
    const auto second = ParticipantId::from_slot(1, 1);
    for (u32 index = 1; index <= 400; ++index) {
        const auto owner = (index % 4) == 0 ? first : second;
        CY_REQUIRE(indexes.on_owner_changed(entity(index), owner).has_value());
    }
    indexes.reset_counters();
    Entity found[128] = {};
    const u32 count = indexes.owned_by(first, found, 128);
    CY_CHECK_EQ(count, 100U);
    // NOT A SCAN. It touched a hundred entities to answer about a hundred, not four hundred to
    // find them.
    CY_CHECK_EQ(indexes.entities_scanned(), 100U);
    CY_CHECK_EQ(indexes.tracked_entities(), 400U);
}

CY_TEST_CASE("gameplay_diagnostics: who is driving this, and why was that rejected") {
    OwnershipRegistry ownership(allocator());
    RelationshipService relationships(allocator());
    TagRegistry tags(allocator());
    EntityTagStore tag_store(allocator());
    ControlRegistry control(allocator());
    CommandStream commands(allocator(), control);
    GameSession session(allocator(), 0x5EEDULL);

    auto team = relationships.add_team(Name::intern("red"));
    auto stunned = tags.declare("State.Stunned");
    CY_REQUIRE(team.has_value());
    CY_REQUIRE(stunned.has_value());
    auto participant = session.add_participant(ParticipantKind::LocalHuman, Name::intern("player"));
    CY_REQUIRE(participant.has_value());
    auto human = control.create_source(ControlSourceKind::Human, *participant, Name::intern("pad"));
    auto assist = control.create_source(ControlSourceKind::ArtificialIntelligence, *participant,
                                        Name::intern("assist"));
    CY_REQUIRE(human.has_value());
    CY_REQUIRE(assist.has_value());

    const Entity tank = entity(11);
    CY_REQUIRE(control.bind_entity(*human, channels::movement(), tank).has_value());
    CY_REQUIRE(control.bind_entity(*assist, channels::turret(), tank).has_value());
    CY_REQUIRE(ownership.set_owner(tank, Owner::of_participant(*participant)).has_value());
    CY_REQUIRE(ownership.set_authority(tank, NetworkAuthority::Server).has_value());
    CY_REQUIRE(relationships.set_team(tank, *team).has_value());
    CY_REQUIRE(relationships.add_affiliation(tank, Name::intern("squad"), 4).has_value());
    CY_REQUIRE(tag_store.add(tank, *stunned).has_value());
    CY_REQUIRE(commands.set_capabilities(tank, 0x3U).has_value());

    const EntityReport report =
        inspect_entity(tank, ownership, control, relationships, tag_store, commands);
    // Owner, controllers and their channels, team, affiliations, capabilities and tags — reported
    // DISTINCTLY, because conflating any two of them is on the forbidden-patterns list.
    CY_CHECK_EQ(report.owner.kind, OwnerKind::Participant);
    CY_CHECK_EQ(report.authority, NetworkAuthority::Server);
    CY_CHECK_EQ(report.team, *team);
    CY_CHECK_EQ(report.capability_mask, 0x3U);
    CY_REQUIRE_EQ(report.controller_count, 2U);
    CY_CHECK_EQ(report.affiliation_count, 1U);
    CY_REQUIRE_EQ(report.tag_count, 1U);
    CY_CHECK_EQ(report.tags[0], *stunned);
    const bool has_movement = report.controllers[0].channel == channels::movement() ||
                              report.controllers[1].channel == channels::movement();
    const bool has_turret = report.controllers[0].channel == channels::turret() ||
                            report.controllers[1].channel == channels::turret();
    CY_CHECK(has_movement);
    CY_CHECK(has_turret);

    // THE RULE DEBUGGER. A command for an entity this participant does not control on the
    // required channel is rejected structurally, and the reason carries the data behind it.
    CommandDeclaration build;
    build.name = Name::intern("Build");
    build.stable_id = 77;
    build.channel = channels::command();
    auto type = commands.declare(build);
    CY_REQUIRE(type.has_value());
    auto producer = commands.open_producer(Name::intern("input"));
    CY_REQUIRE(producer.has_value());
    Command command;
    command.type = *type;
    command.participant = *participant;
    command.source = *human;
    command.target = tank;
    CY_REQUIRE(commands.producer(*producer).record(command).has_value());

    GameplayContext context;
    context.session = &session;
    context.services = &session.services();
    context.commands = &commands;
    context.at.tick = 12;
    commands.commit(context, 12);
    CY_REQUIRE_EQ(commands.rejection_count(), 1U);

    RejectionReport rejections[2] = {};
    CY_REQUIRE_EQ(rejection_reports(commands, rejections, 2), 1U);
    CY_CHECK_EQ(rejections[0].reason.tag, ReasonTag::NotControlled);
    CY_CHECK_EQ(rejections[0].type_name, Name::intern("Build"));
    CY_CHECK_EQ(rejections[0].tick, 12U);

    // And the timeline shows what WAS committed, by tick and by source. A rejected command is not
    // in it: the log is the record of what the simulation consumed.
    TimelineEntry timeline[4] = {};
    CY_CHECK_EQ(command_timeline(commands, 0, 100, ControlSourceId{}, timeline, 4), 0U);
}
