// TASK 4.4.4 — command validation returns reasons.
//
// `gameplay-framework`: "`true` or `false` SHALL NOT be the validation interface", and "One
// validation implementation SHALL serve: the interface explaining why an action is unavailable,
// artificial intelligence deciding what to attempt, the authority rejecting an illegal command, and
// tests asserting behaviour — so that the four cannot disagree."
//
// The last case in this file is the one that makes that claim testable: the interface's answer and
// the authority's answer come from the same call, so they are the same object, and there is no
// arrangement in which they could differ.

#include "fixture.h"

using namespace cy::gameplay_test;
using cy::f32;
using cy::u32;

namespace {

constexpr u32 kBuildCapable = 1U << 0U;

struct RuleState {
    f32 ore = 60.0F;
    u32 calls = 0;
};

/// A game rule: "you need 100 ore". Registered against a command type and run **after** the
/// structural checks.
void ore_rule(const GameplayContext& /*context*/, const Command& /*command*/,
              ValidationResult& result, void* user) noexcept {
    auto* state = static_cast<RuleState*>(user);
    ++state->calls;
    if (state->ore < 100.0F) {
        result.reject(ValidationReason{
            ReasonTag::InsufficientResource, cy::Name::intern("ore"), 100.0F, state->ore, {}});
    }
}

struct ValidationFixture : Fixture {
    [[nodiscard]] bool build() noexcept {
        auto added =
            session.add_participant(ParticipantKind::LocalHuman, cy::Name::intern("player"));
        if (!added) {
            return false;
        }
        participant = *added;
        auto created =
            control.create_source(ControlSourceKind::Human, participant, cy::Name::intern("input"));
        if (!created) {
            return false;
        }
        source = *created;
        if (!control.bind_entity(source, channels::command(), entity(1)).has_value()) {
            return false;
        }
        CommandDeclaration declaration;
        declaration.name = cy::Name::intern("Build");
        declaration.stable_id = 31;
        declaration.channel = channels::command();
        declaration.required_capability = kBuildCapable;
        auto declared = commands.declare(declaration);
        if (!declared) {
            return false;
        }
        build_type = *declared;
        return commands.set_capabilities(entity(1), kBuildCapable).has_value();
    }

    [[nodiscard]] Command build_command() const noexcept {
        Command command;
        command.type = build_type;
        command.participant = participant;
        command.source = source;
        command.target = entity(1);
        return command;
    }

    ParticipantId participant;
    ControlSourceId source;
    CommandTypeId build_type = kInvalidCommandType;
};

}  // namespace

CY_TEST_CASE("gameplay: a permitted command carries no reasons") {
    ValidationFixture fixture;
    CY_REQUIRE(fixture.build());
    const ValidationResult result =
        fixture.commands.validate(fixture.context(), fixture.build_command());
    CY_CHECK(result.permitted());
    CY_CHECK_EQ(result.reason_count(), 0);
    CY_CHECK_EQ(result.first().tag, ReasonTag::None);
}

CY_TEST_CASE("gameplay: structural checks run before game logic, and in order") {
    ValidationFixture fixture;
    CY_REQUIRE(fixture.build());
    RuleState state;
    CY_REQUIRE(fixture.commands.add_rule(fixture.build_type, &ore_rule, &state).has_value());

    // A participant that does not exist. The rule must **not** have been asked "may this
    // participant build" about a participant that is not there — whatever it answered would be
    // whatever its author happened to write for that case.
    Command ghost = fixture.build_command();
    ghost.participant = ParticipantId::from_slot(9, 9);
    const ValidationResult refused = fixture.commands.validate(fixture.context(), ghost);
    CY_CHECK_FALSE(refused.permitted());
    CY_CHECK_EQ(refused.first().tag, ReasonTag::NoSuchParticipant);
    CY_CHECK_EQ(state.calls, 0);

    // An entity this source does not control. Again the rule is not consulted.
    Command wrong_target = fixture.build_command();
    wrong_target.target = entity(2);
    const ValidationResult uncontrolled =
        fixture.commands.validate(fixture.context(), wrong_target);
    CY_CHECK_EQ(uncontrolled.first().tag, ReasonTag::NotControlled);
    CY_CHECK_EQ(state.calls, 0);

    // A target that does not accept the capability.
    CY_REQUIRE(
        fixture.control.bind_entity(fixture.source, channels::command(), entity(3)).has_value());
    Command soldier = fixture.build_command();
    soldier.target = entity(3);
    const ValidationResult incapable = fixture.commands.validate(fixture.context(), soldier);
    CY_CHECK_EQ(incapable.first().tag, ReasonTag::CapabilityMissing);
    CY_CHECK_EQ(state.calls, 0);

    // Everything structural in place: only now does the game rule run.
    const ValidationResult reached =
        fixture.commands.validate(fixture.context(), fixture.build_command());
    CY_CHECK_EQ(state.calls, 1);
    CY_CHECK_FALSE(reached.permitted());
}

CY_TEST_CASE("gameplay: a reason carries the data behind it") {
    ValidationFixture fixture;
    CY_REQUIRE(fixture.build());
    RuleState state;
    CY_REQUIRE(fixture.commands.add_rule(fixture.build_type, &ore_rule, &state).has_value());

    const ValidationResult result =
        fixture.commands.validate(fixture.context(), fixture.build_command());
    CY_REQUIRE_FALSE(result.permitted());
    const ValidationReason& reason = result.first();
    // "You cannot build that" is unactionable. This renders as a sentence, drives an AI's next
    // decision, and asserts in a test — from one object.
    CY_CHECK_EQ(reason.tag, ReasonTag::InsufficientResource);
    CY_CHECK(reason.detail == cy::Name::intern("ore"));
    CY_CHECK_NEAR(reason.required, 100.0F, 1e-5F);
    CY_CHECK_NEAR(reason.available, 60.0F, 1e-5F);

    // With enough ore the same call permits it. Nothing else changed.
    state.ore = 150.0F;
    CY_CHECK(fixture.commands.validate(fixture.context(), fixture.build_command()).permitted());
}

CY_TEST_CASE("gameplay: validation runs without executing the command") {
    ValidationFixture fixture;
    CY_REQUIRE(fixture.build());
    RuleState state;
    CY_REQUIRE(fixture.commands.add_rule(fixture.build_type, &ore_rule, &state).has_value());

    // The interface greys out a button by calling exactly this, before the player acts. Nothing was
    // recorded, nothing was committed, and the log is empty.
    for (u32 index = 0; index < 5; ++index) {
        (void)fixture.commands.validate(fixture.context(), fixture.build_command());
    }
    CY_CHECK_EQ(fixture.commands.committed_count(), 0);
    CY_CHECK_EQ(fixture.commands.log().size(), 0);
    CY_CHECK_EQ(state.calls, 5);
}

CY_TEST_CASE("gameplay: several reasons are kept, and overflow is reported rather than hidden") {
    ValidationResult result;
    result.reject(ReasonTag::OutOfRange);
    result.reject(ReasonTag::Cooldown);
    result.reject(ReasonTag::WrongPhase);
    result.reject(ReasonTag::ProjectDefined, cy::Name::intern("forbidden"), 0.0F, 0.0F);
    CY_CHECK_EQ(result.reason_count(), ValidationResult::kMaxReasons);
    CY_CHECK_FALSE(result.overflowed());

    result.reject(ReasonTag::TargetInvalid);
    // The fifth is dropped and the fact is recorded. Replacing an earlier one would lose the first
    // reason — the one an interface shows — and succeeding quietly would make a fifth rejection
    // look like a permission.
    CY_CHECK_EQ(result.reason_count(), ValidationResult::kMaxReasons);
    CY_CHECK(result.overflowed());
    CY_CHECK_EQ(result.first().tag, ReasonTag::OutOfRange);
    CY_CHECK_FALSE(result.has(ReasonTag::TargetInvalid));
    CY_CHECK_FALSE(result.permitted());
}

CY_TEST_CASE("gameplay: the interface and the authority get the same answer, from the same call") {
    ValidationFixture fixture;
    CY_REQUIRE(fixture.build());
    RuleState state;
    CY_REQUIRE(fixture.commands.add_rule(fixture.build_type, &ore_rule, &state).has_value());
    auto producer = fixture.commands.open_producer(cy::Name::intern("input"));
    CY_REQUIRE(producer.has_value());

    // What the interface asks before drawing the button.
    const ValidationResult shown =
        fixture.commands.validate(fixture.context(), fixture.build_command());

    // What the authority decides when the command actually arrives.
    CY_REQUIRE(fixture.commands.producer(*producer).record(fixture.build_command()).has_value());
    fixture.commands.commit(fixture.context(), 1);
    CY_REQUIRE_EQ(fixture.commands.rejection_count(), 1);
    const ValidationResult& enforced = fixture.commands.rejection(0).result;

    // The same tag and the same data, because it is the same implementation. Four consumers cannot
    // disagree when there is only one of them.
    CY_CHECK_EQ(shown.permitted(), enforced.permitted());
    CY_CHECK_EQ(shown.first().tag, enforced.first().tag);
    CY_CHECK_NEAR(shown.first().available, enforced.first().available, 1e-5F);
    CY_CHECK(shown.first().detail == enforced.first().detail);
}

CY_TEST_CASE("gameplay: an undeclared command type is rejected rather than executed") {
    ValidationFixture fixture;
    CY_REQUIRE(fixture.build());
    Command unknown = fixture.build_command();
    unknown.type = kInvalidCommandType;
    const ValidationResult result = fixture.commands.validate(fixture.context(), unknown);
    CY_CHECK_EQ(result.first().tag, ReasonTag::UnknownCommand);
}
