// SPDX-License-Identifier: MIT
// integration.game_backend_animation — `AnimationAdapter` over a real `AnimationSystem` installed
// in a `runtime::Simulation`, through the ABI table. Issue #76 stage 4.
//
// What the adapter promises a script: an attached animator plays at once; a requested crossfade is
// the same pose and the same root motion as the engine's own `AnimationSystem::play`, bit for bit;
// floats, bools and triggers reach the program, a trigger for exactly one tick; every event is
// delivered exactly once and in time order however many ticks a frame ran; root motion is taken,
// reported, or fed to a character controller; a joint's world pose is the published pose; and every
// refusal is the one `cy_abi.h` names.

#include <cy/game_backend/animation_backend.h>
#include <cy/game_backend/character_backend.h>

#include <cy/runtime/simulation.h>
#include <cy/scene/components.h>
#include <cy/scene/tree.h>

#include "locomotion_fixture.h"
#include "physics_fixture.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using animation::testing::LocomotionRig;

constexpr f32 kTick = 1.0F / 60.0F;

[[nodiscard]] runtime::SimulationConfig config_for(u32 ticks_per_frame) {
    runtime::SimulationConfig config;
    config.world_name = "animation-abi";
    config.session_seed = 11;
    config.clock.mode = determinism::TickMode::FixedStep;
    config.clock.fixed_ticks_per_frame = ticks_per_frame;
    return config;
}

[[nodiscard]] u64 hash(const char* text) noexcept {
    return abi::game::name_hash(text);
}

/// A simulation with the animation system installed, an adapter over it bound on a host, and the
/// locomotion rig registered as "locomotion".
struct Game {
    explicit Game(u32 ticks_per_frame = 1)
        : rig(animation::testing::allocator()),
          simulation(animation::testing::allocator(), config_for(ticks_per_frame)),
          host(system_allocator(MemoryDomain::Scripting)) {
        CY_REQUIRE(rig.build().has_value());
        CY_REQUIRE(simulation.initialize().has_value());
        Expected<ecs::ComponentTypeId, Error> registered =
            animation::register_animator(simulation.world());
        CY_REQUIRE(registered.has_value());
        animator = *registered;
        system = std::make_unique<animation::AnimationSystem>(
            animation::testing::allocator(), simulation.world(), animator, &tree());
        Expected<animation::RigId, Error> added = system->add_rig(rig.rig);
        CY_REQUIRE(added.has_value());
        CY_REQUIRE(system->install(simulation.schedule(), simulation.clock()).has_value());
        CY_REQUIRE(simulation.finalize_registration().has_value());
        adapter = std::make_unique<game_backend::AnimationAdapter>(
            animation::testing::allocator(), simulation.world(), *system, animator, &tree());
        CY_REQUIRE(adapter->add_rig("locomotion", *added).has_value());
        game_backend::bind_animation(host, adapter.get());
    }

    ~Game() { game_backend::bind_animation(host, nullptr); }

    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    [[nodiscard]] CyEngine engine() noexcept { return &host; }

    /// The tree every simulation here is configured with; a fixture without one stops the run.
    [[nodiscard]] scene::SceneTree& tree() {
        scene::SceneTree* created = simulation.tree();
        if (created == nullptr) {
            std::abort();
        }
        return *created;
    }

    /// A scene node at `x` metres, facing down -z, for an animator to sit on.
    [[nodiscard]] CyEntity node(f32 x = 0.0F) {
        Expected<scene::Node, Error> created =
            tree().create_node(Name::intern("character"), tree().root());
        CY_REQUIRE(created.has_value());
        auto* local = simulation.world().get_mut<scene::LocalTransform>(
            created->entity(), tree().components().local_transform);
        CY_REQUIRE(local != nullptr);
        local->value.translation = Vec3{x, 0.0F, 0.0F};
        CY_REQUIRE(scene::mark_transform_changed(tree(), created->entity()).has_value());
        return abi::to_abi(created->entity());
    }

    /// Attach the locomotion rig, in no phase.
    void attach(CyEntity entity, CyRootMotionMode mode = CY_ROOT_MOTION_IGNORE) {
        CyAnimatorDesc desc{};
        desc.struct_size = sizeof(desc);
        desc.rig = "locomotion";
        desc.root_motion = static_cast<u32>(mode);
        CY_REQUIRE_EQ(table().animation_attach(engine(), entity, &desc), CY_RESULT_OK);
    }

    /// One frame: the ticks, the frame stages, and the adapter's frame boundary.
    void frame() {
        const determinism::FrameTicks ticks = simulation.begin_frame(0);
        for (u32 tick = 0; tick < ticks.ticks; ++tick) {
            CY_REQUIRE(simulation.step(nullptr).has_value());
        }
        CY_REQUIRE(simulation.frame(ticks.alpha, nullptr).has_value());
        CY_REQUIRE(system->last_error().has_value());
        CY_REQUIRE(adapter->begin_frame().has_value());
    }

    [[nodiscard]] CyAnimatorState state(CyEntity entity) {
        CyAnimatorState out{};
        out.struct_size = sizeof(out);
        CY_REQUIRE_EQ(table().animation_state(engine(), entity, &out), CY_RESULT_OK);
        return out;
    }

    [[nodiscard]] std::vector<CyAnimationEvent> events() {
        const abi::game::PhaseScope frame_phase(host.game.clock, CY_PHASE_FRAME_UPDATE);
        u32 count = 0;
        CY_REQUIRE_EQ(table().animation_events(engine(), nullptr, 0, &count), CY_RESULT_OK);
        std::vector<CyAnimationEvent> out(count);
        if (count != 0U) {
            CY_REQUIRE_EQ(table().animation_events(engine(), out.data(), count, &count),
                          CY_RESULT_OK);
        }
        return out;
    }

    LocomotionRig rig;
    runtime::Simulation simulation;
    abi::Host host;
    ecs::ComponentTypeId animator = ecs::kInvalidComponent;
    std::unique_ptr<animation::AnimationSystem> system;
    std::unique_ptr<game_backend::AnimationAdapter> adapter;
};

}  // namespace

CY_TEST_CASE("animation abi: an attached animator plays and crossfades a state at once") {
    Game game;
    const CyEntity hero = game.node();
    game.attach(hero);
    CY_CHECK_EQ(game.state(hero).state, hash("idle"));
    CY_CHECK_EQ(game.state(hero).flags, 0U);

    {
        const abi::game::PhaseScope fixed(game.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_REQUIRE_EQ(table().animation_play(game.engine(), hero, "walk", 0.25F), CY_RESULT_OK);
    }
    const CyAnimatorState blending = game.state(hero);
    CY_CHECK_EQ(blending.flags, CY_ANIMATOR_BLENDING | CY_ANIMATOR_REQUESTED);
    CY_CHECK_EQ(blending.state, hash("idle"));
    CY_CHECK_EQ(blending.target, hash("walk"));
    CY_CHECK_EQ(blending.blend_weight, 0.0F);
    // Half the crossfade: the weight is the time it has had.
    for (u32 tick = 0; tick < 7; ++tick) {
        game.frame();
    }
    CY_CHECK_GT(game.state(hero).blend_weight, 0.4F);
    CY_CHECK_LT(game.state(hero).blend_weight, 0.6F);
    for (u32 tick = 0; tick < 10; ++tick) {
        game.frame();
    }
    const CyAnimatorState walking = game.state(hero);
    CY_CHECK_EQ(walking.state, hash("walk"));
    CY_CHECK_EQ(walking.flags, 0U);

    // STOP is the program's entry state, and a zero blend is a cut.
    {
        const abi::game::PhaseScope fixed(game.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_REQUIRE_EQ(table().animation_stop(game.engine(), hero, 0.0F), CY_RESULT_OK);
    }
    CY_CHECK_EQ(game.state(hero).state, hash("idle"));
    CY_CHECK_EQ(table().animation_detach(game.engine(), hero), CY_RESULT_OK);
    CyAnimatorState gone{};
    gone.struct_size = sizeof(gone);
    CY_CHECK_EQ(table().animation_state(game.engine(), hero, &gone), CY_RESULT_NOT_FOUND);
    abi::clear_last_error();
}

CY_TEST_CASE("animation abi: walk then run through the table is the C++ path, bit for bit") {
    // The same requests at the same ticks, one game through the ABI and one through
    // `AnimationSystem::play`: the published matrices and the root motion must be the same bits.
    Game through_abi;
    Game direct;
    const CyEntity a = through_abi.node();
    const CyEntity b = direct.node();
    through_abi.attach(a, CY_ROOT_MOTION_EXTRACT);
    direct.attach(b, CY_ROOT_MOTION_EXTRACT);
    for (u32 frame = 0; frame < 90; ++frame) {
        if (frame == 5 || frame == 40) {
            const char* state = frame == 5 ? "walk" : "run";
            const abi::game::PhaseScope fixed(through_abi.host.game.clock, CY_PHASE_FIXED_UPDATE);
            CY_REQUIRE_EQ(table().animation_play(through_abi.engine(), a, state, 0.2F),
                          CY_RESULT_OK);
            CY_REQUIRE(
                direct.system->play(abi::from_abi(b), Name::intern(state), 0.2F).has_value());
        }
        through_abi.frame();
        direct.frame();
        const Span<const Mat4> left =
            through_abi.system->poses().current(through_abi.system->pose_of(abi::from_abi(a)));
        const Span<const Mat4> right =
            direct.system->poses().current(direct.system->pose_of(abi::from_abi(b)));
        CY_REQUIRE_EQ(left.size(), right.size());
        CY_REQUIRE(std::memcmp(left.data(), right.data(), left.size_bytes()) == 0);
        CyRootMotion motion{};
        motion.struct_size = sizeof(motion);
        CY_REQUIRE_EQ(table().animation_root_motion(through_abi.engine(), a, &motion),
                      CY_RESULT_OK);
        const animation::RootDelta expected = direct.system->root_motion(abi::from_abi(b));
        // The same bits either way: the ABI copies the engine's answer, it does not recompute it.
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
        CY_REQUIRE(std::memcmp(motion.translation, &expected.translation, sizeof(f32) * 3) == 0);
    }
    CY_CHECK_EQ(through_abi.state(a).state, hash("run"));
    // The walk's root moves a metre a second: by now the character has travelled.
    CyRootMotion total{};
    total.struct_size = sizeof(total);
    CY_REQUIRE_EQ(table().animation_root_motion(through_abi.engine(), a, &total), CY_RESULT_OK);
    CY_CHECK_LT(total.travelled[2], -0.5F);
}

CY_TEST_CASE("animation abi: floats and bools reach the program, and a trigger lives one tick") {
    Game game;
    const CyEntity hero = game.node();
    game.attach(hero);
    const abi::game::PhaseScope fixed(game.host.game.clock, CY_PHASE_FIXED_UPDATE);
    CY_REQUIRE_EQ(table().animation_set_float(game.engine(), hero, "clock_run", 0.25F),
                  CY_RESULT_OK);
    f32 value = 0.0F;
    CY_REQUIRE_EQ(table().animation_get_float(game.engine(), hero, "clock_run", &value),
                  CY_RESULT_OK);
    CY_CHECK_EQ(value, 0.25F);
    CY_REQUIRE_EQ(table().animation_set_bool(game.engine(), hero, "request_die", true),
                  CY_RESULT_OK);
    CY_REQUIRE_EQ(table().animation_get_float(game.engine(), hero, "request_die", &value),
                  CY_RESULT_OK);
    CY_CHECK_EQ(value, 1.0F);
    CY_REQUIRE_EQ(table().animation_set_bool(game.engine(), hero, "request_die", false),
                  CY_RESULT_OK);

    // THE TRIGGER: the walk request reads 1 until the next tick has advanced on it, then 0 — and
    // the machine took the edge it opened.
    CY_REQUIRE_EQ(table().animation_fire_trigger(game.engine(), hero, "request_walk"),
                  CY_RESULT_OK);
    CY_REQUIRE_EQ(table().animation_get_float(game.engine(), hero, "request_walk", &value),
                  CY_RESULT_OK);
    CY_CHECK_EQ(value, 1.0F);
    game.frame();
    CY_REQUIRE_EQ(table().animation_get_float(game.engine(), hero, "request_walk", &value),
                  CY_RESULT_OK);
    CY_CHECK_EQ(value, 0.0F);
    CY_CHECK_EQ(game.state(hero).target, hash("walk"));
    CY_CHECK_EQ(game.state(hero).flags, CY_ANIMATOR_BLENDING);

    CY_CHECK_EQ(table().animation_set_float(game.engine(), hero, "no_such_parameter", 1.0F),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().animation_fire_trigger(game.engine(), hero, "no_such_parameter"),
                CY_RESULT_NOT_FOUND);
    abi::clear_last_error();
}

CY_TEST_CASE("animation abi: every event is delivered once, in time order, across a large step") {
    // Five ticks a frame: a footstep at half of each one-second walk cycle crosses inside frames,
    // never on their boundaries, and each must arrive in exactly one frame's list.
    Game game(5);
    const CyEntity hero = game.node();
    game.attach(hero);
    {
        const abi::game::PhaseScope fixed(game.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_REQUIRE_EQ(table().animation_play(game.engine(), hero, "walk", 0.0F), CY_RESULT_OK);
    }
    u32 footsteps = 0;
    for (u32 frame = 0; frame < 48; ++frame) {  // 240 ticks: four seconds
        game.frame();
        for (const CyAnimationEvent& event : game.events()) {
            CY_CHECK_EQ(event.entity, hero);
            CY_CHECK_EQ(event.name, hash("footstep"));
            CY_CHECK_EQ(event.normalised_time, 0.5F);
            ++footsteps;
        }
        // A frame boundary with no tick since the last one hands out nothing again.
        CY_REQUIRE(game.adapter->begin_frame().has_value());
        CY_CHECK(game.events().empty());
    }
    // Four cycles from a cut at t = 0: the footsteps at 0.5, 1.5, 2.5 and 3.5 s.
    CY_CHECK_EQ(footsteps, 4U);

    // The sizing pattern, and a suppressed animator emits nothing.
    CyAnimatorDesc quiet{};
    quiet.struct_size = sizeof(quiet);
    quiet.rig = "locomotion";
    quiet.flags = CY_ANIMATOR_SUPPRESS_EVENTS;
    const CyEntity other = game.node(2.0F);
    CY_REQUIRE_EQ(table().animation_attach(game.engine(), other, &quiet), CY_RESULT_OK);
    {
        const abi::game::PhaseScope fixed(game.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_REQUIRE_EQ(table().animation_play(game.engine(), other, "walk", 0.0F), CY_RESULT_OK);
    }
    u32 total = 0;
    for (u32 frame = 0; frame < 24; ++frame) {
        game.frame();
        const std::vector<CyAnimationEvent> events = game.events();
        for (const CyAnimationEvent& event : events) {
            CY_CHECK_EQ(event.entity, hero);
        }
        total += static_cast<u32>(events.size());
        if (!events.empty()) {
            const abi::game::PhaseScope frame_phase(game.host.game.clock, CY_PHASE_FRAME_UPDATE);
            CyAnimationEvent one{};
            u32 count = 0;
            CY_CHECK_EQ(table().animation_events(game.engine(), &one, 0, &count),
                        CY_RESULT_BUFFER_TOO_SMALL);
            CY_CHECK_EQ(count, static_cast<u32>(events.size()));
        }
    }
    CY_CHECK_EQ(total, 2U);
    abi::clear_last_error();
}

CY_TEST_CASE("animation abi: root motion is taken, extracted, or moves a character") {
    // ACCUMULATE: the deltas of every tick since the last take, composed; the walk's root goes down
    // -z at a metre a second.
    {
        Game game;
        const CyEntity hero = game.node();
        game.attach(hero, CY_ROOT_MOTION_ACCUMULATE);
        {
            const abi::game::PhaseScope fixed(game.host.game.clock, CY_PHASE_FIXED_UPDATE);
            CY_REQUIRE_EQ(table().animation_play(game.engine(), hero, "walk", 0.0F), CY_RESULT_OK);
        }
        for (u32 frame = 0; frame < 30; ++frame) {
            game.frame();
        }
        const abi::game::PhaseScope fixed(game.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CyRootMotion taken{};
        taken.struct_size = sizeof(taken);
        CY_REQUIRE_EQ(table().animation_take_root_motion(game.engine(), hero, &taken),
                      CY_RESULT_OK);
        CY_CHECK_NEAR(taken.translation[2], -0.5F, 0.02F);
        CY_REQUIRE_EQ(table().animation_take_root_motion(game.engine(), hero, &taken),
                      CY_RESULT_OK);
        CY_CHECK_EQ(taken.translation[2], 0.0F);
        // Not in F, and not from an animator that does not accumulate.
        CY_REQUIRE_EQ(
            table().animation_set_root_motion(game.engine(), hero, CY_ROOT_MOTION_EXTRACT),
            CY_RESULT_OK);
        CY_CHECK_EQ(table().animation_take_root_motion(game.engine(), hero, &taken),
                    CY_RESULT_NOT_FOUND);
        abi::clear_last_error();
    }

    // CHARACTER: the adapter takes the delta every tick and moves the character controller by it,
    // turned into the world by the node's placement.
    Scene physics;
    (void)physics.box(1, Vec3{0.0F, -20.0F, 0.0F}, 20.0F);
    game_backend::CharacterAdapter characters(allocator(), physics.server, physics.world);
    Game game;
    game_backend::bind_characters(game.host, &characters);
    game.adapter->bind_characters(&characters);
    const CyEntity hero = game.node();
    CyCharacterDesc capsule{};
    capsule.struct_size = sizeof(capsule);
    capsule.start.position[1] = 0.95F;
    CY_REQUIRE_EQ(table().character_create(game.engine(), hero, &capsule), CY_RESULT_OK);
    const CyEntity loose = game.node(3.0F);
    CyAnimatorDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.rig = "locomotion";
    desc.root_motion = CY_ROOT_MOTION_CHARACTER;
    // Refused for an entity with no character.
    CY_CHECK_EQ(table().animation_attach(game.engine(), loose, &desc), CY_RESULT_NOT_FOUND);
    CY_REQUIRE_EQ(table().animation_attach(game.engine(), hero, &desc), CY_RESULT_OK);
    CY_CHECK_EQ(game.adapter->character_driven().size(), 1U);
    {
        const abi::game::PhaseScope fixed(game.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_REQUIRE_EQ(table().animation_play(game.engine(), hero, "walk", 0.0F), CY_RESULT_OK);
    }
    for (u32 frame = 0; frame < 60; ++frame) {
        game.frame();
        CY_REQUIRE(game.adapter->update(kTick).has_value());
    }
    CyCharacterState moved{};
    moved.struct_size = sizeof(moved);
    CY_REQUIRE_EQ(table().character_state(game.engine(), hero, &moved), CY_RESULT_OK);
    // A second of walking: about a metre down -z, on the ground.
    CY_CHECK_LT(moved.position[2], -0.8F);
    CY_CHECK_GT(moved.position[2], -1.2F);
    CY_CHECK_EQ(moved.ground, CY_GROUND_GROUNDED);
    game.adapter->bind_characters(nullptr);
    game_backend::bind_characters(game.host, nullptr);
    abi::clear_last_error();
}

CY_TEST_CASE("animation abi: a driven character that loses its controller stops no other") {
    // REGRESSION: `update` returned at the first refused move, so destroying one unit's character
    // controller while its animator still fed it failed every later tick and left every character
    // after it in entity order standing still.
    Scene physics;
    (void)physics.box(1, Vec3{0.0F, -20.0F, 0.0F}, 20.0F);
    game_backend::CharacterAdapter characters(allocator(), physics.server, physics.world);
    Game game;
    game_backend::bind_characters(game.host, &characters);
    game.adapter->bind_characters(&characters);
    CyCharacterDesc capsule{};
    capsule.struct_size = sizeof(capsule);
    capsule.start.position[1] = 0.95F;
    CyAnimatorDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.rig = "locomotion";
    desc.root_motion = CY_ROOT_MOTION_CHARACTER;
    const CyEntity units[] = {game.node(0.0F), game.node(4.0F)};
    for (const CyEntity unit : units) {
        CY_REQUIRE_EQ(table().character_create(game.engine(), unit, &capsule), CY_RESULT_OK);
        CY_REQUIRE_EQ(table().animation_attach(game.engine(), unit, &desc), CY_RESULT_OK);
        const abi::game::PhaseScope fixed(game.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_REQUIRE_EQ(table().animation_play(game.engine(), unit, "walk", 0.0F), CY_RESULT_OK);
    }
    CY_REQUIRE_EQ(game.adapter->character_driven().size(), 2U);
    // The FIRST driven entity loses its controller; its animator stays attached.
    const CyEntity fallen = game.adapter->character_driven()[0];
    const CyEntity walker = game.adapter->character_driven()[1];
    CY_REQUIRE_EQ(table().character_destroy(game.engine(), fallen), CY_RESULT_OK);
    for (u32 frame = 0; frame < 30; ++frame) {
        game.frame();
        CY_REQUIRE(game.adapter->update(kTick).has_value());
    }
    CyCharacterState moved{};
    moved.struct_size = sizeof(moved);
    CY_REQUIRE_EQ(table().character_state(game.engine(), walker, &moved), CY_RESULT_OK);
    CY_CHECK_LT(moved.position[2], -0.4F);
    game.adapter->bind_characters(nullptr);
    game_backend::bind_characters(game.host, nullptr);
    abi::clear_last_error();
}

CY_TEST_CASE("animation abi: a joint's world pose is the published pose through the node") {
    Game game;
    const CyEntity hero = game.node(2.0F);
    game.attach(hero);
    game.frame();
    const abi::game::PhaseScope frame_phase(game.host.game.clock, CY_PHASE_FRAME_UPDATE);
    CyPose head{};
    CY_REQUIRE_EQ(table().animation_joint_pose(game.engine(), hero, "head", &head), CY_RESULT_OK);
    Expected<Mat4, Error> model =
        game.system->joint_model_matrix(abi::from_abi(hero), Name::intern("head"));
    CY_REQUIRE(model.has_value());
    // The node stands 2 m along x; the joint's world place is its model place moved there.
    CY_CHECK_NEAR(head.position[0], model->translation().x + 2.0F, 1e-5F);
    CY_CHECK_NEAR(head.position[1], model->translation().y, 1e-5F);
    CY_CHECK_EQ(table().animation_joint_pose(game.engine(), hero, "no_such_joint", &head),
                CY_RESULT_NOT_FOUND);
    abi::clear_last_error();
}

CY_TEST_CASE("animation abi: what it refuses, by name") {
    Game game;
    const CyEntity hero = game.node();
    CyAnimatorDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.rig = "no_such_rig";
    CY_CHECK_EQ(table().animation_attach(game.engine(), hero, &desc), CY_RESULT_NOT_FOUND);
    desc.rig = "locomotion";
    desc.root_motion = 9;
    CY_CHECK_EQ(table().animation_attach(game.engine(), hero, &desc), CY_RESULT_INVALID_ARGUMENT);
    desc.root_motion = 0;
    CY_REQUIRE_EQ(table().animation_attach(game.engine(), hero, &desc), CY_RESULT_OK);
    CY_CHECK_EQ(table().animation_attach(game.engine(), hero, &desc), CY_RESULT_ALREADY_EXISTS);

    // Phases: a request in the frame phase, an event read in the fixed step.
    {
        const abi::game::PhaseScope frame_phase(game.host.game.clock, CY_PHASE_FRAME_UPDATE);
        CY_CHECK_EQ(table().animation_play(game.engine(), hero, "walk", 0.2F),
                    CY_RESULT_PERMISSION_DENIED);
    }
    {
        const abi::game::PhaseScope fixed(game.host.game.clock, CY_PHASE_FIXED_UPDATE);
        u32 count = 0;
        CY_CHECK_EQ(table().animation_events(game.engine(), nullptr, 0, &count),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(table().animation_play(game.engine(), hero, "walk", -1.0F),
                    CY_RESULT_INVALID_ARGUMENT);
        CY_CHECK_EQ(table().animation_play(game.engine(), hero, "walk", NAN),
                    CY_RESULT_INVALID_ARGUMENT);
        CY_CHECK_EQ(table().animation_play(game.engine(), hero, "no_such_state", 0.2F),
                    CY_RESULT_NOT_FOUND);
        CY_CHECK_EQ(table().animation_play(game.engine(), CY_ENTITY_NULL, "walk", 0.2F),
                    CY_RESULT_INVALID_ARGUMENT);
        CY_CHECK_EQ(table().animation_set_root_motion(game.engine(), hero, 7),
                    CY_RESULT_INVALID_ARGUMENT);
        // CHARACTER with no character controller bound.
        CY_CHECK_EQ(
            table().animation_set_root_motion(game.engine(), hero, CY_ROOT_MOTION_CHARACTER),
            CY_RESULT_NOT_FOUND);
    }
    // An entity with no animator.
    const CyEntity stranger = game.node(5.0F);
    CyAnimatorState state{};
    state.struct_size = sizeof(state);
    CY_CHECK_EQ(table().animation_state(game.engine(), stranger, &state), CY_RESULT_NOT_FOUND);
    // No backend bound.
    game_backend::bind_animation(game.host, nullptr);
    CY_CHECK_EQ(table().animation_state(game.engine(), hero, &state), CY_RESULT_UNAVAILABLE);
    game_backend::bind_animation(game.host, game.adapter.get());
    abi::clear_last_error();
}
