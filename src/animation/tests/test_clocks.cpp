// SPDX-License-Identifier: MIT
// The clip clocks `advance()` keeps: when a state's clips start, and when a clip stops. Issue #76
// stage 1, the two runtime defects samples/09b-animated-character worked around with a private
// `StateClocks`.
//
// INTEGRATION, with the rest of the runtime's state machine cases: every case compiles the
// locomotion machine and advances it over a second or more of ticks.

#include <cy/animation/evaluate.h>
#include <cy/graph/locomotion.h>
#include <cy/test/test.h>

#include "locomotion_fixture.h"

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;
namespace pose = cy::graph::pose;

namespace {

constexpr f32 kDt = 1.0F / 60.0F;

/// The locomotion rig, one instance of it, and the buffers to evaluate it into.
struct Machine {
    explicit Machine(Allocator& memory) noexcept
        : locomotion(memory), instance(memory), scratch(memory), local(memory) {}

    [[nodiscard]] Status build(LoopMode death_mode) noexcept {
        if (Status built = locomotion.build(death_mode); !built) {
            return built;
        }
        if (Status prepared = instance.prepare(locomotion.rig); !prepared) {
            return prepared;
        }
        if (Status prepared = scratch.prepare(locomotion.rig); !prepared) {
            return prepared;
        }
        return local.resize(kJointCount);
    }

    [[nodiscard]] Status request(pose::LocomotionState state) noexcept {
        return request_state(state, [this](Name name, f32 value) noexcept {
            return instance.set_parameter(locomotion.rig, name, value);
        });
    }

    [[nodiscard]] f32 clock(pose::LocomotionState state) const noexcept {
        return instance.parameter(locomotion.rig, pose::locomotion_clock(state));
    }

    [[nodiscard]] Status evaluate_pose() noexcept {
        locomotion.skeleton.reference_pose(local.span());
        EvaluationStats stats;
        return evaluate(locomotion.rig, instance, 0, scratch, local.span(), stats);
    }

    LocomotionRig locomotion;
    AnimationInstance instance;
    PoseScratch scratch;
    Array<Transform> local;
};

}  // namespace

// REGRESSION: a state's clips restarted at the moment a blend INTO it completed.
// `graph::pose::advance` zeroes `state_time` there and `animation::advance` reset the state's clip
// clocks on every change of `state`, so the incoming clip jumped back by one blend duration on the
// frame it became current — 802 mm of joint travel in one frame of samples/09b-animated-character's
// run. A state's clips start when it starts being sampled: when it becomes a blend's target.
CY_TEST_CASE(
    "animation clocks: a blend's target starts its clip once, and completing does not restart it") {
    Machine machine(allocator());
    CY_REQUIRE(machine.build(LoopMode::None).has_value());
    CY_REQUIRE(machine.request(pose::LocomotionState::Idle).has_value());
    for (u32 tick = 0; tick < 30; ++tick) {
        CY_REQUIRE(advance(machine.locomotion.rig, machine.instance, kDt, nullptr).has_value());
    }

    // Idle to walk blends over 0.2 s, twelve ticks. The walk's clock has been running beside the
    // idle for half a second; it starts again when the walk becomes the target, and from then on it
    // only moves forward, a tick at a time, through the blend's completion.
    CY_REQUIRE(machine.request(pose::LocomotionState::Walk).has_value());
    CY_REQUIRE(advance(machine.locomotion.rig, machine.instance, kDt, nullptr).has_value());
    CY_REQUIRE_EQ(machine.instance.machine().target, static_cast<u16>(pose::LocomotionState::Walk));
    CY_CHECK_EQ(machine.clock(pose::LocomotionState::Walk), 0.0F);

    f32 previous = machine.clock(pose::LocomotionState::Walk);
    bool completed = false;
    for (u32 tick = 0; tick < 30; ++tick) {
        CY_REQUIRE(advance(machine.locomotion.rig, machine.instance, kDt, nullptr).has_value());
        const f32 now = machine.clock(pose::LocomotionState::Walk);
        CY_CHECK_NEAR(now - previous, kDt, 1e-5);
        previous = now;
        completed = completed || machine.instance.machine().state ==
                                     static_cast<u16>(pose::LocomotionState::Walk);
    }
    CY_CHECK(completed);
}

// REGRESSION: `animation::advance` wrapped every clip clock by its clip's duration whatever the
// loop flag, so a clip compiled as non-looping restarted — a death that fell, stood up and fell
// again. The clock now holds at the duration, and the pose there is the last frame.
CY_TEST_CASE("animation clocks: a non-looping clip holds its last frame") {
    Machine machine(allocator());
    CY_REQUIRE(machine.build(LoopMode::None).has_value());
    CY_REQUIRE(machine.request(pose::LocomotionState::Die).has_value());
    for (u32 tick = 0; tick < 150; ++tick) {
        CY_REQUIRE(advance(machine.locomotion.rig, machine.instance, kDt, nullptr).has_value());
    }
    CY_REQUIRE_EQ(machine.instance.machine().state, static_cast<u16>(pose::LocomotionState::Die));
    CY_CHECK_EQ(machine.clock(pose::LocomotionState::Die), machine.locomotion.die.duration());

    // The death walks the root to z = -1 over its one second, and holds it there.
    CY_REQUIRE(machine.evaluate_pose().has_value());
    CY_CHECK_NEAR(machine.local[kRoot].translation.z, -1.0F, 1e-3);
    CY_REQUIRE(advance(machine.locomotion.rig, machine.instance, kDt, nullptr).has_value());
    CY_CHECK_EQ(machine.clock(pose::LocomotionState::Die), machine.locomotion.die.duration());
}

// A program that compiled the death as non-looping over a clip ASSET that still loops — every
// imported clip loops, because FBX carries no loop flag — holds on the last key too, rather than on
// the first frame the asset's own loop mode would wrap the duration to.
CY_TEST_CASE(
    "animation clocks: a held clip over a looping asset holds the last key, not the first") {
    Machine machine(allocator());
    CY_REQUIRE(machine.build(LoopMode::Loop).has_value());
    CY_REQUIRE(machine.request(pose::LocomotionState::Die).has_value());
    for (u32 tick = 0; tick < 150; ++tick) {
        CY_REQUIRE(advance(machine.locomotion.rig, machine.instance, kDt, nullptr).has_value());
    }
    CY_CHECK_EQ(machine.clock(pose::LocomotionState::Die), machine.locomotion.die.duration());
    CY_REQUIRE(machine.evaluate_pose().has_value());
    CY_CHECK_NEAR(machine.local[kRoot].translation.z, -1.0F, 1e-3);
}

// REGRESSION: every looping clock wrapped by its clip's DURATION, so a `LoopMode::PingPong` clip
// never played its way back: the clock returned to zero at the end of the forward leg and the
// sampler, which folds the second half of a 2 × duration period, never saw that half. A ping-pong
// clock wraps by twice the duration.
CY_TEST_CASE("animation clocks: a ping-pong clip's clock runs through its return leg") {
    Machine machine(allocator());
    CY_REQUIRE(machine.build(LoopMode::None).has_value());
    machine.locomotion.idle.set_loop_mode(LoopMode::PingPong);
    CY_REQUIRE(machine.locomotion.rig
                   .bind(machine.locomotion.skeleton, machine.locomotion.program,
                         machine.locomotion.table.span())
                   .has_value());
    CY_REQUIRE(machine.instance.prepare(machine.locomotion.rig).has_value());
    CY_REQUIRE(machine.request(pose::LocomotionState::Idle).has_value());

    // A second and a half into a one-second clip: half-way back along the return leg.
    for (u32 tick = 0; tick < 90; ++tick) {
        CY_REQUIRE(advance(machine.locomotion.rig, machine.instance, kDt, nullptr).has_value());
    }
    CY_REQUIRE_EQ(machine.instance.machine().state, static_cast<u16>(pose::LocomotionState::Idle));
    CY_CHECK_NEAR(machine.clock(pose::LocomotionState::Idle), 1.5F, 1e-3);

    // And a full period wraps it to where it started.
    for (u32 tick = 0; tick < 60; ++tick) {
        CY_REQUIRE(advance(machine.locomotion.rig, machine.instance, kDt, nullptr).has_value());
    }
    CY_CHECK_NEAR(machine.clock(pose::LocomotionState::Idle), 0.5F, 1e-3);
}

// REGRESSION: a clip ASSET authored with `LoopMode::None` under a program slot compiled as looping
// had its clock wrapped by the duration all the same, so the asset's own decision that it stops
// was ignored and it restarted. Either flag saying "does not loop" holds the clock.
CY_TEST_CASE("animation clocks: a non-looping clip asset holds even where the program loops it") {
    Machine machine(allocator());
    CY_REQUIRE(machine.build(LoopMode::None).has_value());
    machine.locomotion.idle.set_loop_mode(LoopMode::None);
    CY_REQUIRE(machine.locomotion.rig
                   .bind(machine.locomotion.skeleton, machine.locomotion.program,
                         machine.locomotion.table.span())
                   .has_value());
    CY_REQUIRE(machine.instance.prepare(machine.locomotion.rig).has_value());
    CY_REQUIRE(machine.request(pose::LocomotionState::Idle).has_value());
    for (u32 tick = 0; tick < 90; ++tick) {
        CY_REQUIRE(advance(machine.locomotion.rig, machine.instance, kDt, nullptr).has_value());
    }
    CY_REQUIRE_EQ(machine.instance.machine().state, static_cast<u16>(pose::LocomotionState::Idle));
    CY_CHECK_EQ(machine.clock(pose::LocomotionState::Idle), machine.locomotion.idle.duration());
}
