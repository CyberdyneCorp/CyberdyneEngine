// SPDX-License-Identifier: MIT
// The per-frame animation system: `Animator` components on entities, advanced in the fixed step and
// evaluated in `Stage::Animation` by a real `runtime::Simulation`, with nothing per sample in the
// game. Issue #76 stage 1.
//
// INTEGRATION: every case runs a simulation over tens or hundreds of ticks, and two of them start a
// job system.

#include <cy/animation/animation_system.h>
#include <cy/core/jobs/job_system.h>
#include <cy/runtime/simulation.h>
#include <cy/scene/components.h>
#include <cy/scene/tree.h>
#include <cy/test/test.h>

#include "locomotion_fixture.h"

#include <cstdlib>
#include <cstring>
#include <memory>

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;
namespace pose = cy::graph::pose;

namespace {

constexpr f32 kTick = 1.0F / 60.0F;

[[nodiscard]] runtime::SimulationConfig config_for(u32 ticks_per_frame, bool realtime = false) {
    runtime::SimulationConfig config;
    config.world_name = "animation";
    config.session_seed = 7;
    config.clock.mode =
        realtime ? determinism::TickMode::Realtime : determinism::TickMode::FixedStep;
    config.clock.fixed_ticks_per_frame = ticks_per_frame;
    return config;
}

/// A simulation with the animation system installed over it.
struct Host {
    explicit Host(u32 ticks_per_frame = 1, bool realtime = false)
        : simulation(allocator(), config_for(ticks_per_frame, realtime)) {}

    [[nodiscard]] Status open(std::initializer_list<const AnimationRig*> rigs,
                              const AnimationSystemConfig& settings = {}) {
        if (Status initialized = simulation.initialize(); !initialized) {
            return initialized;
        }
        Expected<ecs::ComponentTypeId, Error> registered = register_animator(simulation.world());
        if (!registered) {
            return Status{make_unexpected(registered.error())};
        }
        animator = *registered;
        system = std::make_unique<AnimationSystem>(allocator(), simulation.world(), animator,
                                                   simulation.tree(), settings);
        for (const AnimationRig* rig : rigs) {
            Expected<RigId, Error> added = system->add_rig(*rig);
            if (!added) {
                return Status{make_unexpected(added.error())};
            }
        }
        Expected<ecs::SystemId, Error> installed =
            system->install(simulation.schedule(), simulation.clock());
        if (!installed) {
            return Status{make_unexpected(installed.error())};
        }
        return simulation.finalize_registration();
    }

    /// The scene tree every host here creates. A simulation configured without one would be a
    /// broken fixture, so it stops the run rather than handing back null.
    [[nodiscard]] scene::SceneTree& tree() {
        scene::SceneTree* created = simulation.tree();
        if (created == nullptr) {
            std::abort();
        }
        return *created;
    }

    /// An entity carrying `settings`; a scene node when `node`, so root motion has a transform.
    [[nodiscard]] ecs::Entity spawn(const Animator& settings, bool node = false) {
        ecs::Entity entity = ecs::kNoEntity;
        if (node) {
            Expected<scene::Node, Error> created =
                tree().create_node(Name::intern("character"), tree().root());
            CY_REQUIRE(created.has_value());
            entity = created->entity();
            CY_REQUIRE(simulation.world().add(entity, animator, &settings).has_value());
        } else {
            const ecs::ComponentTypeId components[] = {animator};
            Expected<ecs::Entity, Error> created =
                simulation.world().create(Span<const ecs::ComponentTypeId>(components, 1));
            CY_REQUIRE(created.has_value());
            entity = *created;
            CY_REQUIRE(simulation.world().set(entity, animator, settings).has_value());
        }
        return entity;
    }

    /// One frame: the ticks the clock grants, then the frame stages.
    [[nodiscard]] Status frame(jobs::JobSystem* jobs = nullptr, i64 elapsed_ns = 0) {
        const determinism::FrameTicks ticks = simulation.begin_frame(elapsed_ns);
        for (u32 tick = 0; tick < ticks.ticks; ++tick) {
            Expected<determinism::CommitRecord, Error> stepped = simulation.step(jobs);
            if (!stepped) {
                return Status{make_unexpected(stepped.error())};
            }
        }
        if (Status framed = simulation.frame(ticks.alpha, jobs); !framed) {
            return framed;
        }
        return system->last_error();
    }

    [[nodiscard]] Status request(ecs::Entity entity, pose::LocomotionState state) {
        return request_state(state, [this, entity](Name name, f32 value) noexcept {
            return system->set_parameter(entity, name, value);
        });
    }

    runtime::Simulation simulation;
    ecs::ComponentTypeId animator = ecs::kInvalidComponent;
    std::unique_ptr<AnimationSystem> system;
};

/// The hand-driven path samples/09b-animated-character took: `advance` per tick, `evaluate` and
/// `publish_pose` per frame, into a pose world of its own.
struct HandDriven {
    explicit HandDriven(Allocator& memory) noexcept
        : instance(memory),
          scratch(memory),
          world(memory),
          local(memory),
          model(memory),
          matrices(memory) {}

    [[nodiscard]] Status prepare(const AnimationRig& rig) {
        if (Status prepared = instance.prepare(rig); !prepared) {
            return prepared;
        }
        if (Status prepared = scratch.prepare(rig); !prepared) {
            return prepared;
        }
        const u16 joints = rig.skeleton().joint_count();
        Expected<PoseHandle, Error> added = world.add(joints);
        if (!added) {
            return Status{make_unexpected(added.error())};
        }
        handle = *added;
        if (Status sized = local.resize(joints); !sized) {
            return sized;
        }
        if (Status sized = model.resize(joints); !sized) {
            return sized;
        }
        return matrices.resize(joints);
    }

    [[nodiscard]] Status pose(const AnimationRig& rig) {
        rig.skeleton().reference_pose(local.span());
        EvaluationStats stats;
        if (Status evaluated = evaluate(rig, instance, 0, scratch, local.span(), stats);
            !evaluated) {
            return evaluated;
        }
        return publish_pose(rig.skeleton(), local.span(), 0, world, handle, model.span(),
                            matrices.span());
    }

    [[nodiscard]] Status request(const AnimationRig& rig, pose::LocomotionState state) {
        return request_state(state, [this, &rig](Name name, f32 value) noexcept {
            return instance.set_parameter(rig, name, value);
        });
    }

    AnimationInstance instance;
    PoseScratch scratch;
    PoseWorld world;
    PoseHandle handle;
    Array<Transform> local;
    Array<Transform> model;
    Array<Mat4> matrices;
};

[[nodiscard]] bool same_bits(Span<const Mat4> a, Span<const Mat4> b) noexcept {
    return a.size() == b.size() && a.size() != 0 &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(Mat4)) == 0;
}

[[nodiscard]] bool same_bits(Vec3 a, Vec3 b) noexcept {
    return std::memcmp(&a, &b, sizeof(Vec3)) == 0;
}

/// The request schedule samples/09b-animated-character plays, compressed: idle, walk, run, back
/// through the walk, and a death that holds.
[[nodiscard]] pose::LocomotionState requested_at(u32 tick) noexcept {
    if (tick >= 200) {
        return pose::LocomotionState::Die;
    }
    if (tick >= 150) {
        return pose::LocomotionState::Walk;
    }
    if (tick >= 90) {
        return pose::LocomotionState::Run;
    }
    if (tick >= 30) {
        return pose::LocomotionState::Walk;
    }
    return pose::LocomotionState::Idle;
}

[[nodiscard]] Animator animator_on(RigId rig, LodTier tier = LodTier::Full,
                                   RootMotionMode mode = RootMotionMode::Ignore) noexcept {
    Animator animator;
    animator.rig = rig;
    animator.tier = tier;
    animator.root_motion = mode;
    return animator;
}

}  // namespace

CY_TEST_CASE(
    "animation system: an entity's machine advances with simulation time, bit for bit the "
    "hand-driven path") {
    LocomotionRig locomotion(allocator());
    CY_REQUIRE(locomotion.build().has_value());
    Host host;
    CY_REQUIRE(host.open({&locomotion.rig}).has_value());
    const ecs::Entity character = host.spawn(animator_on(0));

    // The first frame runs one tick: the instance is created by the tick's sync, at the entry
    // state, and advanced once with no request raised.
    HandDriven hand(allocator());
    CY_REQUIRE(hand.prepare(locomotion.rig).has_value());
    CY_REQUIRE(host.frame().has_value());
    CY_REQUIRE(host.system->instance(character) != nullptr);
    CY_REQUIRE(advance(locomotion.rig, hand.instance, kTick, nullptr).has_value());

    // From then on both receive the same requests before the same tick. The comparison is of the
    // BITS: the system is that path moved, not a second implementation of it.
    for (u32 tick = 1; tick < 330; ++tick) {
        CY_REQUIRE(host.request(character, requested_at(tick)).has_value());
        CY_REQUIRE(hand.request(locomotion.rig, requested_at(tick)).has_value());
        CY_REQUIRE(host.frame().has_value());
        CY_REQUIRE(advance(locomotion.rig, hand.instance, kTick, nullptr).has_value());
        CY_REQUIRE(hand.pose(locomotion.rig).has_value());

        const PoseHandle handle = host.system->pose_of(character);
        CY_REQUIRE(
            same_bits(host.system->poses().current(handle), hand.world.current(hand.handle)));
        CY_REQUIRE(same_bits(host.system->travelled(character), hand.instance.travelled()));
    }
    // The machine visited every state and stopped in the death.
    CY_CHECK_EQ(host.system->instance(character)->machine().state,
                static_cast<u16>(pose::LocomotionState::Die));
    CY_CHECK_EQ(host.system->stats().ticks_total, 330U);
}

CY_TEST_CASE(
    "animation system: two ticks in one frame advance twice, and a frame with none advances "
    "nothing") {
    LocomotionRig locomotion(allocator());
    CY_REQUIRE(locomotion.build().has_value());

    // Two ticks a frame: after ten frames the walk's clock has run twenty ticks, not ten.
    Host twice(2);
    CY_REQUIRE(twice.open({&locomotion.rig}).has_value());
    const ecs::Entity walker = twice.spawn(animator_on(0));
    CY_REQUIRE(twice.frame().has_value());
    const f32 started =
        *twice.system->parameter(walker, pose::locomotion_clock(pose::LocomotionState::Walk));
    for (u32 frame = 0; frame < 10; ++frame) {
        CY_REQUIRE(twice.frame().has_value());
    }
    const f32 ran =
        *twice.system->parameter(walker, pose::locomotion_clock(pose::LocomotionState::Walk));
    CY_CHECK_NEAR(ran - started, 20.0F * kTick, 1e-5);

    // A real-time clock given no elapsed time grants no tick: the frame stages run, and the pose
    // and the clocks stay where they were.
    Host idle(1, true);
    CY_REQUIRE(idle.open({&locomotion.rig}).has_value());
    const ecs::Entity still = idle.spawn(animator_on(0));
    CY_REQUIRE(idle.frame(nullptr, 20'000'000).has_value());
    CY_REQUIRE(idle.system->instance(still) != nullptr);
    const u64 ticks = idle.system->stats().ticks_total;
    const PoseHandle handle = idle.system->pose_of(still);
    const u32 offset = idle.system->poses().matrix_offset(handle);
    CY_REQUIRE(idle.frame(nullptr, 0).has_value());
    CY_CHECK_EQ(idle.system->stats().ticks_total, ticks);
    CY_CHECK_EQ(idle.system->stats().evaluated, 0U);
    CY_CHECK_EQ(idle.system->poses().matrix_offset(handle), offset);
}

CY_TEST_CASE(
    "animation system: requests drive every transition, and an entered state's clip runs on "
    "through its blend") {
    LocomotionRig locomotion(allocator());
    CY_REQUIRE(locomotion.build().has_value());
    Host host;
    CY_REQUIRE(host.open({&locomotion.rig}).has_value());
    const ecs::Entity character = host.spawn(animator_on(0));
    CY_REQUIRE(host.frame().has_value());

    // Each state's clock, followed from the tick the state becomes a blend's target: it starts at
    // zero there and moves forward one tick at a time through the blend's completion — the
    // continuity samples/09b-animated-character kept by hand with `StateClocks`.
    bool visited[pose::kLocomotionStateCount] = {};
    u32 blends = 0;
    u16 following = 0xFFFFU;
    f32 previous = 0.0F;
    for (u32 tick = 1; tick < 330; ++tick) {
        CY_REQUIRE(host.request(character, requested_at(tick)).has_value());
        CY_REQUIRE(host.frame().has_value());
        const graph::pose::PoseInstance& machine = host.system->instance(character)->machine();
        visited[machine.state] = true;
        if (machine.target != 0xFFFFU && machine.target != following) {
            following = machine.target;
            ++blends;
            previous = *host.system->parameter(
                character, pose::locomotion_clock(static_cast<pose::LocomotionState>(following)));
            CY_CHECK_EQ(previous, 0.0F);
            continue;
        }
        if (following == 0xFFFFU || following == static_cast<u16>(pose::LocomotionState::Die)) {
            continue;
        }
        const f32 now = *host.system->parameter(
            character, pose::locomotion_clock(static_cast<pose::LocomotionState>(following)));
        // Forward by one tick, or wrapped by one loop of a one-second clip — never backwards.
        const f32 step = now >= previous ? now - previous : (now + 1.0F) - previous;
        CY_CHECK_NEAR(step, kTick, 1e-4);
        previous = now;
    }
    CY_CHECK_EQ(blends, 4U);
    for (const bool seen : visited) {
        CY_CHECK(seen);
    }
}

CY_TEST_CASE(
    "animation system: level of detail sets the rate, the bones and whether a pose is evaluated at "
    "all") {
    LocomotionRig locomotion(allocator());
    CY_REQUIRE(locomotion.build().has_value());
    Host host;
    CY_REQUIRE(host.open({&locomotion.rig}).has_value());
    const ecs::Entity full = host.spawn(animator_on(0, LodTier::Full));
    const ecs::Entity simplified = host.spawn(animator_on(0, LodTier::Simplified));
    const ecs::Entity baked = host.spawn(animator_on(0, LodTier::Baked));
    CY_REQUIRE(host.frame().has_value());
    for (const ecs::Entity entity : {full, simplified, baked}) {
        CY_REQUIRE(host.request(entity, pose::LocomotionState::Walk).has_value());
    }
    Array<Mat4> baked_reference(allocator());
    CY_REQUIRE(baked_reference.append(host.system->poses().current(host.system->pose_of(baked)))
                   .has_value());

    // Twelve ticks at 60 Hz: the full instance is posed on all twelve, the simplified one at its
    // 30 Hz on six, and the baked one on none.
    u32 publishes[3] = {0, 0, 0};
    u32 offsets[3] = {};
    const ecs::Entity entities[3] = {full, simplified, baked};
    for (u32 index = 0; index < 3; ++index) {
        offsets[index] = host.system->poses().matrix_offset(host.system->pose_of(entities[index]));
    }
    for (u32 tick = 0; tick < 12; ++tick) {
        CY_REQUIRE(host.frame().has_value());
        for (u32 index = 0; index < 3; ++index) {
            const u32 now =
                host.system->poses().matrix_offset(host.system->pose_of(entities[index]));
            publishes[index] += now != offsets[index] ? 1U : 0U;
            offsets[index] = now;
        }
    }
    CY_CHECK_EQ(publishes[0], 12U);
    CY_CHECK_EQ(publishes[1], 6U);
    CY_CHECK_EQ(publishes[2], 0U);

    // The simplified instance is evaluated at bone level 1, where the finger is dropped: its
    // skinning matrix is the identity the dropped joint is published as, while the full instance
    // moves the arm the finger hangs from.
    const Mat4 identity = Mat4::identity();
    const Span<const Mat4> simplified_pose =
        host.system->poses().current(host.system->pose_of(simplified));
    CY_CHECK(
        same_bits(Span<const Mat4>(&simplified_pose[kFinger], 1), Span<const Mat4>(&identity, 1)));
    CY_CHECK_FALSE(same_bits(
        Span<const Mat4>(&host.system->poses().current(host.system->pose_of(full))[kUpperArm], 1),
        Span<const Mat4>(&identity, 1)));

    // The baked instance kept the reference pose it was published with on its first frame, and
    // travelled exactly as far as the full one: root motion runs at every tier.
    const Span<const Mat4> baked_pose = host.system->poses().current(host.system->pose_of(baked));
    CY_CHECK(same_bits(baked_pose, baked_reference.span()));
    CY_CHECK(same_bits(host.system->travelled(baked), host.system->travelled(full)));
    CY_CHECK(host.system->travelled(full).z < -0.05F);

    // A tier changed on the component is the instance's tier from the next tick.
    ecs::World& world = host.simulation.world();
    world.get_mut<Animator>(baked, host.animator)->tier = LodTier::Full;
    const u32 before = host.system->poses().matrix_offset(host.system->pose_of(baked));
    CY_REQUIRE(host.frame().has_value());
    CY_CHECK_NE(host.system->poses().matrix_offset(host.system->pose_of(baked)), before);
}

CY_TEST_CASE(
    "animation system: a footstep fires once per loop crossing, and a suppressed instance counts "
    "it") {
    LocomotionRig locomotion(allocator());
    CY_REQUIRE(locomotion.build().has_value());
    Host host;
    CY_REQUIRE(host.open({&locomotion.rig}).has_value());
    const ecs::Entity loud = host.spawn(animator_on(0));
    Animator quiet_settings = animator_on(0);
    quiet_settings.events = EventPolicy::Suppress;
    const ecs::Entity quiet = host.spawn(quiet_settings);
    CY_REQUIRE(host.frame().has_value());
    CY_REQUIRE(host.request(loud, pose::LocomotionState::Walk).has_value());
    CY_REQUIRE(host.request(quiet, pose::LocomotionState::Walk).has_value());

    // The walk's clock restarts when it becomes the blend's target, and its footstep is at half a
    // second: 150 ticks of a one-second walk cross it at 0.5 s and 1.5 s and at no other time.
    u32 footsteps = 0;
    u32 suppressed = 0;
    for (u32 tick = 0; tick < 150; ++tick) {
        CY_REQUIRE(host.frame().has_value());
        for (const EmittedEvent& event : host.system->events().events()) {
            CY_CHECK_EQ(event.name, Name::intern("footstep"));
            CY_CHECK(host.system->entity_of(event) == loud);
            ++footsteps;
        }
        suppressed += host.system->events().suppressed();
    }
    CY_CHECK_EQ(footsteps, 2U);
    CY_CHECK_EQ(suppressed, 2U);
}

CY_TEST_CASE("animation system: root motion goes where each instance's mode says") {
    LocomotionRig locomotion(allocator());
    CY_REQUIRE(locomotion.build().has_value());
    // The walk carries root motion: a metre a second toward -z on the root joint.
    CY_REQUIRE(locomotion.walk.has_root_motion());
    Host host;
    CY_REQUIRE(host.open({&locomotion.rig}).has_value());
    const ecs::Entity applied =
        host.spawn(animator_on(0, LodTier::Full, RootMotionMode::ApplyToTransform), true);
    const ecs::Entity controlled =
        host.spawn(animator_on(0, LodTier::Full, RootMotionMode::Controller), true);
    const ecs::Entity extracted =
        host.spawn(animator_on(0, LodTier::Full, RootMotionMode::ExtractOnly), true);
    const ecs::Entity ignored =
        host.spawn(animator_on(0, LodTier::Full, RootMotionMode::Ignore), true);
    CY_REQUIRE(host.frame().has_value());
    for (const ecs::Entity entity : {applied, controlled, extracted, ignored}) {
        CY_REQUIRE(host.request(entity, pose::LocomotionState::Walk).has_value());
    }
    for (u32 tick = 0; tick < 60; ++tick) {
        CY_REQUIRE(host.frame().has_value());
    }

    const ecs::World& world = host.simulation.world();
    const ecs::ComponentTypeId local = host.tree().components().local_transform;
    const auto placed = [&](ecs::Entity entity) {
        return world.get<scene::LocalTransform>(entity, local)->value.translation;
    };
    const Vec3 travelled = host.system->travelled(applied);
    CY_REQUIRE(travelled.z < -0.5F);
    // Applied: the transform moved by exactly what was extracted, tick by tick.
    CY_CHECK_NEAR(placed(applied).z, travelled.z, 1e-4);
    // Controller: the transform did not move, and the controller takes the whole of it once.
    CY_CHECK_EQ(placed(controlled).z, 0.0F);
    const RootDelta taken = host.system->take_root_motion(controlled);
    CY_CHECK_NEAR(taken.translation.z, host.system->travelled(controlled).z, 1e-4);
    CY_CHECK_EQ(host.system->take_root_motion(controlled).translation.z, 0.0F);
    // Extract only and ignore: nothing moved; the last tick's delta is there to read.
    CY_CHECK_EQ(placed(extracted).z, 0.0F);
    CY_CHECK_EQ(placed(ignored).z, 0.0F);
    CY_CHECK_LT(host.system->root_motion(extracted).translation.z, 0.0F);
}

CY_TEST_CASE("animation system: removing an instance mid-run leaves every other handle valid") {
    LocomotionRig locomotion(allocator());
    CY_REQUIRE(locomotion.build().has_value());

    // Two hosts, one with a middle character the other never had. Removing it from the first must
    // leave the other two posing exactly as they do in the second.
    Host removing;
    Host reference;
    CY_REQUIRE(removing.open({&locomotion.rig}).has_value());
    CY_REQUIRE(reference.open({&locomotion.rig}).has_value());
    const ecs::Entity first = removing.spawn(animator_on(0));
    const ecs::Entity middle = removing.spawn(animator_on(0));
    const ecs::Entity last = removing.spawn(animator_on(0));
    const ecs::Entity first_ref = reference.spawn(animator_on(0));
    const ecs::Entity last_ref = reference.spawn(animator_on(0));
    CY_REQUIRE(removing.frame().has_value());
    CY_REQUIRE(reference.frame().has_value());

    const PoseHandle last_pose = removing.system->pose_of(last);
    for (u32 tick = 1; tick < 120; ++tick) {
        if (tick == 40) {
            CY_REQUIRE(removing.simulation.world().remove(middle, removing.animator).has_value());
        }
        CY_REQUIRE(removing.request(first, requested_at(tick)).has_value());
        CY_REQUIRE(removing.request(last, requested_at(tick + 20)).has_value());
        if (tick < 40) {
            CY_REQUIRE(removing.request(middle, requested_at(tick + 60)).has_value());
        }
        CY_REQUIRE(reference.request(first_ref, requested_at(tick)).has_value());
        CY_REQUIRE(reference.request(last_ref, requested_at(tick + 20)).has_value());
        CY_REQUIRE(removing.frame().has_value());
        CY_REQUIRE(reference.frame().has_value());

        CY_REQUIRE(
            same_bits(removing.system->poses().current(removing.system->pose_of(first)),
                      reference.system->poses().current(reference.system->pose_of(first_ref))));
        CY_REQUIRE(
            same_bits(removing.system->poses().current(removing.system->pose_of(last)),
                      reference.system->poses().current(reference.system->pose_of(last_ref))));
    }
    CY_CHECK(removing.system->instance(middle) == nullptr);
    CY_CHECK(removing.system->pose_of(last) == last_pose);
    CY_CHECK(removing.system->poses().live(last_pose));
    CY_CHECK_EQ(removing.system->stats().instances_removed, 1U);
    CY_CHECK_EQ(removing.system->stats().instances, 2U);
}

CY_TEST_CASE(
    "animation system: five hundred instances over three rigs are three batches spread over the "
    "workers") {
    LocomotionRig a(allocator());
    LocomotionRig b(allocator());
    LocomotionRig c(allocator());
    CY_REQUIRE(a.build().has_value());
    CY_REQUIRE(b.build().has_value());
    CY_REQUIRE(c.build().has_value());

    jobs::JobSystem workers;
    jobs::JobSystemConfig config;
    config.worker_count = 4;
    CY_REQUIRE(workers.start(config).has_value());

    AnimationSystemConfig slices;
    slices.slice = 8;
    Host parallel;
    Host serial;
    CY_REQUIRE(parallel.open({&a.rig, &b.rig, &c.rig}, slices).has_value());
    CY_REQUIRE(serial.open({&a.rig, &b.rig, &c.rig}, slices).has_value());
    ecs::Entity spawned[2][500] = {};
    for (u32 index = 0; index < 500; ++index) {
        spawned[0][index] = parallel.spawn(animator_on(index % 3));
        spawned[1][index] = serial.spawn(animator_on(index % 3));
    }
    CY_REQUIRE(parallel.frame(&workers).has_value());
    CY_REQUIRE(serial.frame().has_value());
    CY_CHECK_EQ(parallel.system->stats().batches, 3U);
    CY_CHECK_EQ(parallel.system->stats().instances, 500U);

    for (u32 tick = 1; tick < 40; ++tick) {
        for (u32 index = 0; index < 500; ++index) {
            CY_REQUIRE(
                parallel.request(spawned[0][index], requested_at(tick + index % 50)).has_value());
            CY_REQUIRE(
                serial.request(spawned[1][index], requested_at(tick + index % 50)).has_value());
        }
        CY_REQUIRE(parallel.frame(&workers).has_value());
        CY_REQUIRE(serial.frame().has_value());
    }
    CY_CHECK_EQ(parallel.system->stats().evaluated, 500U);
    CY_CHECK_GE(parallel.system->stats().slices, 63U);

    // MORE THAN ONE PARTICIPANT TOOK A SLICE, and which one did is invisible in the result: the
    // job-scheduled poses equal the single-threaded ones bit for bit.
    u32 participants = 0;
    for (u32 bit = 0; bit < 64; ++bit) {
        participants += ((parallel.system->stats().workers >> bit) & 1U) != 0 ? 1U : 0U;
    }
    CY_CHECK_GT(participants, 1U);
    for (u32 index = 0; index < 500; ++index) {
        CY_REQUIRE(
            same_bits(parallel.system->poses().current(parallel.system->pose_of(spawned[0][index])),
                      serial.system->poses().current(serial.system->pose_of(spawned[1][index]))));
    }
    parallel.system.reset();
    serial.system.reset();
    workers.shutdown();
}

CY_TEST_CASE(
    "animation system: two runs of the same ticks reproduce pose, root motion and events exactly") {
    LocomotionRig locomotion(allocator());
    CY_REQUIRE(locomotion.build().has_value());
    jobs::JobSystem workers;
    jobs::JobSystemConfig config;
    config.worker_count = 3;
    CY_REQUIRE(workers.start(config).has_value());

    // The second run takes the same ticks in different frames — three a frame against one — which
    // is exactly what a determinism rule written against simulation time has to survive.
    struct Run {
        Host host;
        ecs::Entity characters[8] = {};
        Array<EmittedEvent> events;
        explicit Run(u32 ticks_per_frame) : host(ticks_per_frame), events(allocator()) {}
    };
    Run one(1);
    Run three(3);
    for (Run* run : {&one, &three}) {
        CY_REQUIRE(run->host.open({&locomotion.rig}).has_value());
        for (u32 index = 0; index < 8; ++index) {
            run->characters[index] = run->host.spawn(
                animator_on(0, LodTier::Full, RootMotionMode::ApplyToTransform), true);
        }
    }
    // Requests change only on multiples of three ticks, so both runs see the same request before
    // the same tick.
    for (u32 tick = 0; tick < 240; tick += 3) {
        for (Run* run : {&one, &three}) {
            for (u32 index = 0; index < 8; ++index) {
                if (run->host.system->instance(run->characters[index]) != nullptr) {
                    CY_REQUIRE(
                        run->host.request(run->characters[index], requested_at(tick + index * 7))
                            .has_value());
                }
            }
        }
        for (u32 frame = 0; frame < 3; ++frame) {
            CY_REQUIRE(one.host.frame(&workers).has_value());
            for (const EmittedEvent& event : one.host.system->events().events()) {
                CY_REQUIRE(one.events.push_back(event).has_value());
            }
        }
        CY_REQUIRE(three.host.frame(&workers).has_value());
        for (const EmittedEvent& event : three.host.system->events().events()) {
            CY_REQUIRE(three.events.push_back(event).has_value());
        }
    }

    const ecs::ComponentTypeId local = one.host.tree().components().local_transform;
    for (u32 index = 0; index < 8; ++index) {
        const ecs::Entity a = one.characters[index];
        const ecs::Entity b = three.characters[index];
        CY_CHECK(same_bits(one.host.system->poses().current(one.host.system->pose_of(a)),
                           three.host.system->poses().current(three.host.system->pose_of(b))));
        CY_CHECK(same_bits(one.host.system->travelled(a), three.host.system->travelled(b)));
        const auto* placed_a = one.host.simulation.world().get<scene::LocalTransform>(a, local);
        const auto* placed_b = three.host.simulation.world().get<scene::LocalTransform>(b, local);
        CY_CHECK(std::memcmp(&placed_a->value, &placed_b->value, sizeof(Transform)) == 0);
    }
    CY_REQUIRE_EQ(one.events.size(), three.events.size());
    CY_CHECK_GT(one.events.size(), 0U);
    for (usize index = 0; index < one.events.size(); ++index) {
        CY_CHECK_EQ(one.events[index].instance, three.events[index].instance);
        CY_CHECK_EQ(one.events[index].name, three.events[index].name);
        CY_CHECK_EQ(one.events[index].normalised_time, three.events[index].normalised_time);
    }
    one.host.system.reset();
    three.host.system.reset();
    workers.shutdown();
}

CY_TEST_CASE(
    "animation system: an Animator naming no rig is refused with a reason and nothing else stops") {
    LocomotionRig locomotion(allocator());
    CY_REQUIRE(locomotion.build().has_value());
    Host host;
    CY_REQUIRE(host.open({&locomotion.rig}).has_value());
    const ecs::Entity good = host.spawn(animator_on(0));
    const ecs::Entity wrong = host.spawn(animator_on(7));
    CY_REQUIRE(host.simulation.begin_frame(0).ticks == 1U);
    CY_REQUIRE(host.simulation.step(nullptr).has_value());
    CY_REQUIRE(host.simulation.frame(0.0F, nullptr).has_value());
    CY_CHECK(host.system->instance(good) != nullptr);
    CY_CHECK(host.system->instance(wrong) == nullptr);
    CY_CHECK_GE(host.system->stats().refused, 1U);
    CY_CHECK_FALSE(host.system->last_error().has_value());
    // A parameter the program does not declare is refused by name lookup, not ignored.
    CY_CHECK_FALSE(
        host.system->set_parameter(good, Name::intern("no_such_parameter"), 1.0F).has_value());
    CY_CHECK_FALSE(
        host.system
            ->set_parameter(wrong, pose::locomotion_request(pose::LocomotionState::Walk), 1.0F)
            .has_value());
}
