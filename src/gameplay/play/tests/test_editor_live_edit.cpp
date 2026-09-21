// A field edited while a world is playing. M11.b task 3.2, and the
// `live-edit-applies-without-a-restart` criterion.
//
// ================================================================================================
// WHAT THE CRITERION ASKS FOR, AND HOW THIS SUITE AVOIDS ANSWERING IT VACUOUSLY
// ================================================================================================
//
// m11b.toml: *"a field edited while a world is playing takes effect under its declared policy, and
// a field whose policy says restart restarts. The claim is that the policy is DECLARED per field
// rather than inferred, so the case edits one field of each policy class."*
//
// Three things would make that pass while proving nothing, and each has a case written against it:
//
//   1. **A policy that is always derived.** Every field of every component a play session creates
//   is
//      classified `Authoring`, so the derived default for all of them is `Immediate` — and
//      `Immediate` is WRONG for all of them, because the physics bridge consumes each at body
//      creation and writes `LocalTransform` back every step. The cases check
//      `LiveEditDecision::declared` field by field, and check that the derived answer for the same
//      field would have been a different one.
//
//   2. **"Applied" as a boolean the code that applied it set.** `LiveEditOutcome` carries the
//      session's tick before and after, and the cases read those instead: every policy but
//      `RestartWorld` leaves them equal, and `RestartWorld` leaves the world at tick zero. The
//      session produces those numbers; this file only compares them.
//
//   3. **Runtime state "preserved" because nothing touched it.** The health case rebuilds the
//      component through a host that resets health to full, so preservation is the only way the
//      value can still be 53 afterwards. Break the preservation and the case reads 100.

#include <cy/ecs/world.h>
#include <cy/gameplay/live/compiler.h>
#include <cy/gameplay/live/policy.h>
#include <cy/gameplay/play/session.h>
#include <cy/test/test.h>

#include "editor_play_fixture.h"

#include <string>

using cy::f32;
using cy::u32;
using cy::u64;
using namespace cy::gameplay;
using namespace cy::gameplay::live;
using namespace cy::test_editor;

namespace {

/// The fixture's own component: an authoring maximum and the runtime value the simulation owns.
/// See `editor_play_fixture.h` for why it is the fixture's and not the engine's.
struct Character {
    f32 maximum_health = 100.0F;
    f32 health = 100.0F;
};

constexpr std::string_view kCharacter = "Character";
constexpr std::string_view kMaximumHealth = "maximum_health";
constexpr std::string_view kHealth = "health";

/// A host that knows how to rebuild the fixture's own component.
///
/// It resets `health` to the maximum, which is what a genuine "torn down and rebuilt from new data"
/// does — and which is exactly what makes the preservation check discriminating: if the compiler
/// did not carry the runtime value across, the case would read the maximum instead of 53.
class CharacterHost : public LiveEditHost {
public:
    CharacterHost(cy::ecs::ComponentTypeId component, f32 rebuilt_maximum) noexcept
        : component_(component), rebuilt_maximum_(rebuilt_maximum) {}

    [[nodiscard]] cy::Status reinitialize(PlaySession& session, u64 identity,
                                          std::string_view type) noexcept override {
        if (type != kCharacter) {
            return LiveEditHost::reinitialize(session, identity, type);
        }
        cy::ecs::World* world = session.world();
        const cy::ecs::Entity entity = session.entity_for(identity);
        if (world == nullptr || !entity.valid()) {
            return cy::fail(cy::ErrorCode::NotFound, "no entity for that identity");
        }
        if (cy::Status removed = world->remove(entity, component_); !removed) {
            return removed;
        }
        Character fresh;
        fresh.maximum_health = rebuilt_maximum_;
        fresh.health = rebuilt_maximum_;  // rebuilt from new data: full health
        ++rebuilds;
        return world->add(entity, component_, &fresh);
    }

    u32 rebuilds = 0;

private:
    cy::ecs::ComponentTypeId component_;
    f32 rebuilt_maximum_;
};

/// A session entered, with the compiler bound against it and the engine's policies declared.
struct Playing {
    explicit Playing(cy::physics::PhysicsServer* server) noexcept
        : policies(allocator()),
          compiler(allocator(), policies),
          session(allocator(), state.world) {
        if (!state.started) {
            return;
        }
        configuration = configuration_over(server, state.schema);
        ready = declare_engine_policies(policies).has_value() &&
                session.enter(configuration).has_value() &&
                compiler.bind_play_session(session).has_value();
    }

    Playing(const Playing&) = delete;
    Playing& operator=(const Playing&) = delete;
    Playing(Playing&&) = delete;
    Playing& operator=(Playing&&) = delete;
    ~Playing() { (void)session.stop(); }

    /// Register the fixture's component in the session's world and put it on one node's entity.
    [[nodiscard]] cy::ecs::ComponentTypeId attach_character(u64 identity, f32 maximum, f32 health) {
        cy::ecs::World* world = session.world();
        if (world == nullptr) {
            return cy::ecs::kInvalidComponent;
        }
        const cy::Expected<cy::ecs::ComponentTypeId, cy::Error> registered =
            world->components().register_builtin("Character", sizeof(Character),
                                                 alignof(Character));
        if (!registered) {
            return cy::ecs::kInvalidComponent;
        }
        Character value;
        value.maximum_health = maximum;
        value.health = health;
        const cy::ecs::Entity entity = session.entity_for(identity);
        if (!entity.valid() || !world->add(entity, *registered, &value)) {
            return cy::ecs::kInvalidComponent;
        }
        return *registered;
    }

    [[nodiscard]] const Character* character(u64 identity, cy::ecs::ComponentTypeId component) {
        return session.world() == nullptr
                   ? nullptr
                   : session.world()->get<Character>(session.entity_for(identity), component);
    }

    Authored state;
    PlayConfiguration configuration;
    LiveEditPolicyTable policies;
    LiveEditCompiler compiler;
    PlaySession session;
    bool ready = false;
};

/// The authored identity of the sphere — the node with the rigid body and the character on it.
[[nodiscard]] u64 sphere_of(const Authored& authored) noexcept {
    return authored.world.nodes()[1].identity;
}

}  // namespace

CY_TEST_CASE("a field edited while playing is announced before it is applied") {
    Reference physics;
    CY_REQUIRE(physics.ready());
    Playing playing(physics.server());
    CY_REQUIRE(playing.ready);

    const u64 sphere = sphere_of(playing.state);
    AuthoringChange change;
    change.node_identity = sphere;

    // DECLARED, NOT DERIVED, AND THE DIFFERENCE IS CHECKED RATHER THAN ASSUMED. Each of these three
    // fields is classified `Authoring`, so the derived answer for each is `Immediate` — and each
    // engine declaration says something stronger, because the bridge consumes the value at body
    // creation.
    change.type = "RigidBody";
    change.field = "mass";
    LiveEditDecision decision = playing.compiler.announce(change);
    CY_CHECK(decision.policy == LiveEditPolicy::ReinitializeComponent);
    CY_CHECK(decision.declared);

    change.type = "Transform";
    change.field = "translation";
    decision = playing.compiler.announce(change);
    CY_CHECK(decision.policy == LiveEditPolicy::RecreateEntity);
    CY_CHECK(decision.declared);

    change.scope = ChangeScope::World;
    change.type = "World";
    change.field = "gravity";
    decision = playing.compiler.announce(change);
    CY_CHECK(decision.policy == LiveEditPolicy::RestartWorld);
    CY_CHECK(decision.declared);

    // The derived default those three declarations override. If the table's declarations were
    // dropped, every one of the above would read `Immediate` — which is the mistake the
    // declarations exist to prevent, and the reason `declared` is checked beside the policy.
    cy::gameplay::live::FieldNature authoring;
    CY_CHECK(derived_policy_for(authoring).policy == LiveEditPolicy::Immediate);
    CY_CHECK_FALSE(derived_policy_for(authoring).declared);

    // A field nothing has bound is announced too, rather than being absent from the inspector: it
    // answers `Unsupported` with a reason naming why.
    AuthoringChange unbound;
    unbound.node_identity = sphere;
    unbound.type = "RigidBody";
    unbound.field = "linear_damping";
    const LiveEditDecision unknown = playing.compiler.announce(unbound);
    CY_CHECK(unknown.policy == LiveEditPolicy::Unsupported);
    CY_CHECK(std::string(unknown.reason).find("no live binding") != std::string::npos);
}

CY_TEST_CASE("a field edited while playing takes effect under its declared policy") {
    Reference physics;
    CY_REQUIRE(physics.ready());
    Playing playing(physics.server());
    CY_REQUIRE(playing.ready);
    const u64 sphere = sphere_of(playing.state);

    for (u32 tick = 0; tick < 12; ++tick) {
        CY_REQUIRE(playing.session.tick().has_value());
    }
    CY_REQUIRE(playing.session.state() == PlayState::Playing);

    LiveEditHost host;

    // --- ReinitializeComponent, and it does NOT restart --------------------------------------
    AuthoringChange mass;
    mass.node_identity = sphere;
    mass.type = "RigidBody";
    mass.field = "mass";
    mass.value = float_value(7.5F);
    const cy::Expected<LiveEditOutcome, cy::Error> reinitialised =
        playing.compiler.apply(playing.session, playing.state.world, host, mass);
    CY_REQUIRE(reinitialised.has_value());
    CY_CHECK(reinitialised->policy == LiveEditPolicy::ReinitializeComponent);
    CY_CHECK(reinitialised->declared);
    CY_CHECK(reinitialised->applied);
    CY_CHECK_EQ(reinitialised->components_reinitialized, 1U);
    // THE MEASUREMENT, not the claim: the simulation is where it was, so the world did not restart.
    CY_CHECK_EQ(reinitialised->tick_after, reinitialised->tick_before);
    CY_CHECK_GT(reinitialised->tick_before, 0U);
    CY_CHECK(playing.session.state() == PlayState::Playing);
    // And the authoring half landed: the document carries the new value.
    CY_CHECK_NEAR(authored_float(playing.state.world, sphere, "RigidBody", "mass"), 7.5F, 0.0001);

    // --- RecreateEntity: a new entity, the same identity ---------------------------------------
    const cy::ecs::Entity before_recreate = playing.session.entity_for(sphere);
    CY_REQUIRE(before_recreate.valid());
    AuthoringChange moved;
    moved.node_identity = sphere;
    moved.type = "Transform";
    moved.field = "translation";
    moved.value = vec3_value(1.0F, 6.0F, -2.0F);
    const cy::Expected<LiveEditOutcome, cy::Error> recreated =
        playing.compiler.apply(playing.session, playing.state.world, host, moved);
    CY_REQUIRE(recreated.has_value());
    CY_CHECK(recreated->policy == LiveEditPolicy::RecreateEntity);
    CY_CHECK(recreated->declared);
    CY_CHECK_EQ(recreated->entities_recreated, 1U);
    CY_CHECK_EQ(recreated->tick_after, recreated->tick_before);
    const cy::ecs::Entity after_recreate = playing.session.entity_for(sphere);
    CY_REQUIRE(after_recreate.valid());
    // The identity survived and the entity did not, which is what "recreated, preserving identity"
    // means and is the only way to tell a recreate from a reinitialise from the outside.
    CY_CHECK(after_recreate != before_recreate);

    // --- RestartWorld: and the tick says so ----------------------------------------------------
    const u64 before_restart = playing.session.clock().tick();
    CY_CHECK_GT(before_restart, 0U);
    AuthoringChange gravity;
    gravity.scope = ChangeScope::World;
    gravity.type = "World";
    gravity.field = "gravity";
    gravity.value = vec3_value(0.0F, -3.0F, 0.0F);
    const cy::Expected<LiveEditOutcome, cy::Error> restarted =
        playing.compiler.apply(playing.session, playing.state.world, host, gravity);
    CY_REQUIRE(restarted.has_value());
    CY_CHECK(restarted->policy == LiveEditPolicy::RestartWorld);
    CY_CHECK_EQ(restarted->worlds_restarted, 1U);
    // THE ONE POLICY WHOSE TICKS DIFFER. A restarted world begins again at zero.
    CY_CHECK_EQ(restarted->tick_before, before_restart);
    CY_CHECK_LT(restarted->tick_after, restarted->tick_before);
    CY_CHECK_EQ(restarted->tick_after, 0U);
    CY_CHECK(playing.session.state() == PlayState::Playing);

    // AND THE EDIT SURVIVED THE RESTART. `stop()` restores the document to the session's restore
    // target, and the compiler wrote the mass edit into that target as well as into the live
    // document — so a designer's edit made during play is not silently discarded when play ends.
    CY_CHECK_NEAR(authored_float(playing.state.world, sphere, "RigidBody", "mass"), 7.5F, 0.0001);
    CY_REQUIRE(playing.session.stop().has_value());
    CY_CHECK_NEAR(authored_float(playing.state.world, sphere, "RigidBody", "mass"), 7.5F, 0.0001);
}

CY_TEST_CASE("a field edited while playing keeps the runtime state the simulation owns") {
    // `live-editing`: *"WHEN a designer raises a character's maximum health while it is at 53 of
    // 100 THEN the maximum SHALL update and the current value SHALL remain 53."*
    Reference physics;
    CY_REQUIRE(physics.ready());
    Playing playing(physics.server());
    CY_REQUIRE(playing.ready);
    const u64 sphere = sphere_of(playing.state);

    const cy::ecs::ComponentTypeId character = playing.attach_character(sphere, 100.0F, 53.0F);
    CY_REQUIRE(character != cy::ecs::kInvalidComponent);

    // The two fields, bound with their real classifications: the maximum is authoring data and the
    // current value is the simulation's.
    LiveFieldBinding maximum;
    maximum.type = kCharacter;
    maximum.field = kMaximumHealth;
    maximum.component = character;
    maximum.offset = static_cast<u32>(offsetof(Character, maximum_health));
    maximum.kind = LiveFieldKind::F32;
    maximum.nature.persistence = cy::reflect::PersistenceKind::Authoring;
    CY_REQUIRE(playing.compiler.bind(maximum).has_value());

    LiveFieldBinding current;
    current.type = kCharacter;
    current.field = kHealth;
    current.component = character;
    current.offset = static_cast<u32>(offsetof(Character, health));
    current.kind = LiveFieldKind::F32;
    current.nature.persistence = cy::reflect::PersistenceKind::RuntimeState;
    CY_REQUIRE(playing.compiler.bind(current).has_value());

    // The component is rebuilt from new data, so the policy is a reinitialise. Declared, because
    // nothing about `maximum_health`'s classification implies it.
    CY_REQUIRE(
        playing.policies.declare(kCharacter, kMaximumHealth, LiveEditPolicy::ReinitializeComponent)
            .has_value());

    for (u32 tick = 0; tick < 5; ++tick) {
        CY_REQUIRE(playing.session.tick().has_value());
    }

    CharacterHost host(character, 200.0F);
    // Named `edit` rather than `raise` because doctest's CY_CHECK / CY_REQUIRE expansion may pull
    // in an unqualified `raise(int)` (POSIX signal helper) on some libstdc++ / doctest header
    // combinations, and a local named `raise` then shadows it — GCC on arm64 refuses the macro
    // with "no match for call to (AuthoringChange) (int)". Same-scope shadowing of a stdlib name
    // is the antipattern; renaming avoids it without teaching every downstream header a workaround.
    AuthoringChange edit;
    edit.node_identity = sphere;
    edit.type = kCharacter;
    edit.field = kMaximumHealth;
    edit.value = float_value(200.0F);
    const cy::Expected<LiveEditOutcome, cy::Error> applied =
        playing.compiler.apply(playing.session, playing.state.world, host, edit);
    CY_REQUIRE(applied.has_value());
    CY_CHECK(applied->policy == LiveEditPolicy::ReinitializeComponent);
    CY_CHECK_EQ(host.rebuilds, 1U);
    // One field carried, and it is reported rather than assumed.
    CY_CHECK_EQ(applied->runtime_state_preserved, 1U);
    CY_CHECK_EQ(applied->runtime_state_lost, 0U);

    const Character* after = playing.character(sphere, character);
    CY_REQUIRE(after != nullptr);
    // THE SCENARIO, VERBATIM. The host rebuilt the component with health at the full 200, so a
    // compiler that did not carry the runtime value across would read 200 here.
    CY_CHECK_NEAR(after->maximum_health, 200.0F, 0.0001);
    CY_CHECK_NEAR(after->health, 53.0F, 0.0001);
    CY_CHECK_EQ(applied->tick_after, applied->tick_before);
}

CY_TEST_CASE("a field edited while playing reports runtime state it could not carry") {
    // The other half of the same requirement: *"Where a change cannot preserve runtime state … the
    // loss SHALL be reported and … rather than occurring silently."*
    Reference physics;
    CY_REQUIRE(physics.ready());
    Playing playing(physics.server());
    CY_REQUIRE(playing.ready);
    const u64 sphere = sphere_of(playing.state);

    const cy::ecs::ComponentTypeId character = playing.attach_character(sphere, 100.0F, 53.0F);
    CY_REQUIRE(character != cy::ecs::kInvalidComponent);

    LiveFieldBinding current;
    current.type = kCharacter;
    current.field = kHealth;
    current.component = character;
    current.offset = static_cast<u32>(offsetof(Character, health));
    current.kind = LiveFieldKind::F32;
    current.nature.persistence = cy::reflect::PersistenceKind::RuntimeState;
    CY_REQUIRE(playing.compiler.bind(current).has_value());

    // A recreate destroys the entity and respawns it from the authored node — and the authored node
    // has no `Character` for the session to rebuild, because the component was added to the running
    // world rather than authored. So the runtime state has nowhere to go, and saying so is the
    // requirement.
    LiveEditHost host;
    AuthoringChange moved;
    moved.node_identity = sphere;
    moved.type = "Transform";
    moved.field = "translation";
    moved.value = vec3_value(0.0F, 5.0F, 0.0F);
    const cy::Expected<LiveEditOutcome, cy::Error> recreated =
        playing.compiler.apply(playing.session, playing.state.world, host, moved);
    CY_REQUIRE(recreated.has_value());
    CY_CHECK(recreated->policy == LiveEditPolicy::RecreateEntity);
    CY_CHECK_EQ(recreated->runtime_state_preserved, 0U);
    CY_CHECK_EQ(recreated->runtime_state_lost, 1U);
}

CY_TEST_CASE("a field edited while playing whose policy is unsupported is refused, not applied") {
    Reference physics;
    CY_REQUIRE(physics.ready());
    Playing playing(physics.server());
    CY_REQUIRE(playing.ready);
    const u64 sphere = sphere_of(playing.state);

    const cy::ecs::ComponentTypeId character = playing.attach_character(sphere, 100.0F, 53.0F);
    CY_REQUIRE(character != cy::ecs::kInvalidComponent);

    LiveFieldBinding current;
    current.type = kCharacter;
    current.field = kHealth;
    current.component = character;
    current.offset = static_cast<u32>(offsetof(Character, health));
    current.kind = LiveFieldKind::F32;
    current.nature.persistence = cy::reflect::PersistenceKind::RuntimeState;
    CY_REQUIRE(playing.compiler.bind(current).has_value());

    AuthoringChange change;
    change.node_identity = sphere;
    change.type = kCharacter;
    change.field = kHealth;
    change.value = float_value(1.0F);

    // Derived from the classification: the simulation owns this value, so an authoring edit of it
    // applies on the next run.
    const LiveEditDecision decision = playing.compiler.announce(change);
    CY_CHECK(decision.policy == LiveEditPolicy::Unsupported);
    CY_CHECK_FALSE(decision.declared);

    LiveEditHost host;
    const cy::Expected<LiveEditOutcome, cy::Error> refused =
        playing.compiler.apply(playing.session, playing.state.world, host, change);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::Unsupported);

    // NOTHING WAS APPLIED. A refusal that had written the value first would be the "partially
    // applied" the requirement forbids by name.
    const Character* after = playing.character(sphere, character);
    CY_REQUIRE(after != nullptr);
    CY_CHECK_NEAR(after->health, 53.0F, 0.0001);
}

CY_TEST_CASE("a field edited while playing with no in-place form refuses an immediate policy") {
    // `Collider.shape` is cooked into a shape handle when the body is created, so there is no
    // in-place write for it. Declaring one is a mistake, and the compiler reports it as one rather
    // than writing bytes that mean something else.
    Reference physics;
    CY_REQUIRE(physics.ready());
    Playing playing(physics.server());
    CY_REQUIRE(playing.ready);
    const u64 sphere = sphere_of(playing.state);

    CY_REQUIRE(
        playing.policies.declare("Collider", "radius", LiveEditPolicy::Immediate).has_value());

    AuthoringChange change;
    change.node_identity = sphere;
    change.type = "Collider";
    change.field = "radius";
    change.value = float_value(2.0F);
    CY_CHECK(playing.compiler.announce(change).policy == LiveEditPolicy::Immediate);

    LiveEditHost host;
    const cy::Expected<LiveEditOutcome, cy::Error> refused =
        playing.compiler.apply(playing.session, playing.state.world, host, change);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::Unsupported);
    CY_CHECK(std::string(refused.error().message).find("in-place") != std::string::npos);
}

CY_TEST_CASE("the six live edit policies round-trip their own spelling") {
    // A word rather than a number on any wire, for the reason `PlayMode` is a word: a seventh
    // policy added on one side must be refused by name rather than read as the closest one.
    const LiveEditPolicy all[] = {
        LiveEditPolicy::Immediate,      LiveEditPolicy::ReinitializeComponent,
        LiveEditPolicy::RecreateEntity, LiveEditPolicy::ReloadAsset,
        LiveEditPolicy::RestartWorld,   LiveEditPolicy::Unsupported,
    };
    for (const LiveEditPolicy policy : all) {
        const cy::Expected<LiveEditPolicy, cy::Error> parsed =
            live_edit_policy_of(live_edit_policy_name(policy));
        CY_REQUIRE(parsed.has_value());
        CY_CHECK(*parsed == policy);
    }
    CY_CHECK_FALSE(live_edit_policy_of("restart").has_value());

    // A transaction touching several fields applies at the strongest policy any of them declares.
    CY_CHECK(live_edit_stronger(LiveEditPolicy::Immediate, LiveEditPolicy::RestartWorld) ==
             LiveEditPolicy::RestartWorld);
    CY_CHECK(
        live_edit_stronger(LiveEditPolicy::RecreateEntity, LiveEditPolicy::ReinitializeComponent) ==
        LiveEditPolicy::RecreateEntity);

    // And the four classifications derive the four answers the header states.
    cy::gameplay::live::FieldNature nature;
    nature.persistence = cy::reflect::PersistenceKind::Authoring;
    CY_CHECK(derived_policy_for(nature).policy == LiveEditPolicy::Immediate);
    nature.asset_reference = true;
    CY_CHECK(derived_policy_for(nature).policy == LiveEditPolicy::ReloadAsset);
    nature.asset_reference = false;
    nature.persistence = cy::reflect::PersistenceKind::Derived;
    CY_CHECK(derived_policy_for(nature).policy == LiveEditPolicy::ReinitializeComponent);
    nature.persistence = cy::reflect::PersistenceKind::RuntimeState;
    CY_CHECK(derived_policy_for(nature).policy == LiveEditPolicy::Unsupported);
    nature.persistence = cy::reflect::PersistenceKind::PersistentState;
    CY_CHECK(derived_policy_for(nature).policy == LiveEditPolicy::Unsupported);
}

CY_TEST_CASE("a field edited while playing that restarts the world drops stale bindings") {
    // A component identifier belongs to a WORLD, and a restart builds a new one. A binding kept
    // across it would point at whichever column that number happens to name in the new world — a
    // write into the wrong bytes rather than a diagnosable failure. The compiler drops every
    // binding and takes the engine's again, so the next change to a caller's field announces
    // `Unsupported` with a reason a caller can act on.
    Reference physics;
    CY_REQUIRE(physics.ready());
    Playing playing(physics.server());
    CY_REQUIRE(playing.ready);
    const u64 sphere = sphere_of(playing.state);

    const cy::ecs::ComponentTypeId character = playing.attach_character(sphere, 100.0F, 53.0F);
    CY_REQUIRE(character != cy::ecs::kInvalidComponent);
    LiveFieldBinding maximum;
    maximum.type = kCharacter;
    maximum.field = kMaximumHealth;
    maximum.component = character;
    maximum.offset = static_cast<u32>(offsetof(Character, maximum_health));
    maximum.kind = LiveFieldKind::F32;
    CY_REQUIRE(playing.compiler.bind(maximum).has_value());

    AuthoringChange edit;  // Not `raise` — see the twin comment on the earlier occurrence.
    edit.node_identity = sphere;
    edit.type = kCharacter;
    edit.field = kMaximumHealth;
    edit.value = float_value(150.0F);
    // Bound, so it announces the derived `Immediate` rather than "no live binding".
    CY_CHECK(playing.compiler.announce(edit).policy == LiveEditPolicy::Immediate);

    LiveEditHost host;
    AuthoringChange gravity;
    gravity.scope = ChangeScope::World;
    gravity.type = "World";
    gravity.field = "gravity";
    gravity.value = vec3_value(0.0F, -1.0F, 0.0F);
    const cy::Expected<LiveEditOutcome, cy::Error> restarted =
        playing.compiler.apply(playing.session, playing.state.world, host, gravity);
    CY_REQUIRE(restarted.has_value());
    CY_CHECK_EQ(restarted->worlds_restarted, 1U);

    // The caller's binding is gone, and it says so rather than writing somewhere.
    const LiveEditDecision after = playing.compiler.announce(edit);
    CY_CHECK(after.policy == LiveEditPolicy::Unsupported);
    CY_CHECK(std::string(after.reason).find("no live binding") != std::string::npos);
    // And the engine's own are back, against the NEW world's identifiers.
    AuthoringChange mass;
    mass.node_identity = sphere;
    mass.type = "RigidBody";
    mass.field = "mass";
    mass.value = float_value(3.0F);
    CY_CHECK(playing.compiler.announce(mass).policy == LiveEditPolicy::ReinitializeComponent);
    CY_REQUIRE(
        playing.compiler.apply(playing.session, playing.state.world, host, mass).has_value());
}
