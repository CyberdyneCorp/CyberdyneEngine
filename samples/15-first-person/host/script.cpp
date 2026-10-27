// SPDX-License-Identifier: MIT
// script.cpp — the scripted player. See script.h.

#include "script.h"

#include <cy/core/math/scalar.h>

#include <cmath>
#include <numbers>

namespace sample::fps {
namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using cy::input::Key;
using cy::input::MouseControl;

/// The mouse's sensitivity the game sets (game/Contract.swift), radians a count.
constexpr f32 kRadiansPerCount = 0.0022F;
/// The most the hand moves the mouse in a frame, in counts: a quick, not an instant, turn.
constexpr f32 kMaxCounts = 45.0F;
/// Close enough, radians: the aim error under which the hand stops moving the mouse.
constexpr f32 kOnAim = 0.0015F;
/// Frames the hand holds still before it checks its aim and fires, and frames it waits after a
/// shot for the kick to settle.
constexpr u32 kSettleFrames = 4;
constexpr u32 kRecoverFrames = 16;
/// The most frames one aim may take before the hand gives it up.
constexpr u32 kTaskFrames = 150;
/// When the shooting starts.
constexpr u64 kFirstAim = 270;

/// What the hand shoots at, in order.
struct Plan {
    u32 mark = 2;  // 0 target, 1 prop, 2 point
    u32 index = 0;
    Vec3 point;
    u32 shots = 1;
    bool sights = false;
};
constexpr Plan kPlan[] = {
    {0, 1, Vec3{}, 3, false},                    // T1, three times: it goes down
    {1, 2, Vec3{}, 1, false},                    // the top crate of the stack
    {2, 0, Vec3{1.6F, 2.2F, -14.0F}, 1, false},  // the red north wall
    {2, 0, Vec3{4.6F, 0.0F, -5.2F}, 1, false},   // the floor ahead: a mark, and bouncing sparks
    {0, 0, Vec3{}, 2, true},                     // T0, down the sights, twice: it is hurt
};
constexpr u32 kPlanSize = sizeof(kPlan) / sizeof(kPlan[0]);

[[nodiscard]] f32 wrap(f32 angle) noexcept {
    constexpr f32 kPi = std::numbers::pi_v<f32>;
    while (angle > kPi) {
        angle -= 2.0F * kPi;
    }
    while (angle < -kPi) {
        angle += 2.0F * kPi;
    }
    return angle;
}

}  // namespace

cy::Status ScriptedPlayer::before_frame(u64 frame) noexcept {
    if (cy::Status walked = walk(frame); !walked) {
        return walked;
    }
    return frame >= kFirstAim ? shoot(frame) : cy::ok();
}

cy::Status ScriptedPlayer::walk(u64 frame) noexcept {
    // THE WALK, as key edges on a timeline. North (W) for a second; crouch (C) and keep walking
    // under the beam, letting go of C while still under it; sprint (Shift); stop; jump.
    struct Edge {
        u64 frame;
        Key key;
        bool down;
    };
    static constexpr Edge kEdges[] = {
        {30, Key::W, true},          {90, Key::C, true},       {148, Key::C, false},
        {190, Key::LeftShift, true}, {226, Key::W, false},     {226, Key::LeftShift, false},
        {236, Key::Space, true},     {238, Key::Space, false},
    };
    for (const Edge& edge : kEdges) {
        if (edge.frame == frame) {
            if (cy::Status pressed = host_->press_key(edge.key, edge.down); !pressed) {
                return pressed;
            }
        }
    }
    return cy::ok();
}

Vec3 ScriptedPlayer::aim_point(const Task& task) const noexcept {
    switch (task.mark) {
        case Mark::Target:
            return host_->target(task.index).feet + Vec3{0.0F, 1.0F, 0.0F};
        case Mark::Prop:
            return host_->prop(task.index).translation;
        case Mark::Point:
            break;
    }
    return task.point;
}

bool ScriptedPlayer::mark_gone(const Task& task) const noexcept {
    return task.mark == Mark::Target && host_->target(task.index).mode == TargetMode::Down;
}

bool ScriptedPlayer::on_mark(const Task& task) const noexcept {
    // The hand's own eye: a ray from where the frame was drawn along where the weapon points.
    const Vec3 origin = host_->eye().translation;
    cy::physics::RayCastHit hit;
    const CyEntity entity = host_->raycast(origin, host_->weapon_aim(), kShotRange, &hit);
    switch (task.mark) {
        case Mark::Target:
            return entity == host_->target_entity(task.index);
        case Mark::Prop:
            return entity == host_->prop_entity(task.index);
        case Mark::Point:
            break;
    }
    return !hit.body.is_null() && cy::length(hit.position - task.point) < 0.4F;
}

cy::Status ScriptedPlayer::next_task(u64 frame) noexcept {
    if (task_ < 8U) {
        findings_.finished[task_] = frame;
    }
    ++task_;
    findings_.tasks = task_;
    fired_ = 0;
    task_frames_ = 0;
    phase_ = Phase::Aim;
    phase_frames_ = 0;
    const bool sights = task_ < kPlanSize && kPlan[task_].sights;
    if (sights != sights_down_) {
        sights_down_ = sights;
        return host_->press_button(MouseControl::Right, sights);
    }
    return cy::ok();
}

cy::Status ScriptedPlayer::shoot(u64 frame) noexcept {
    if (trigger_down_) {
        trigger_down_ = false;
        if (cy::Status released = host_->press_button(MouseControl::Left, false); !released) {
            return released;
        }
    }
    if (task_ >= kPlanSize) {
        return cy::ok();
    }
    if (frame == kFirstAim && kPlan[0].sights) {
        sights_down_ = true;
        if (cy::Status pressed = host_->press_button(MouseControl::Right, true); !pressed) {
            return pressed;
        }
    }
    const Plan& plan = kPlan[task_];
    Task task;
    task.mark = static_cast<Mark>(plan.mark);
    task.index = plan.index;
    task.point = plan.point;
    task.shots = plan.shots;
    task.sights = plan.sights;
    ++task_frames_;
    ++phase_frames_;
    if (mark_gone(task) || fired_ >= task.shots) {
        return next_task(frame);
    }
    if (task_frames_ > kTaskFrames) {
        ++findings_.abandoned;
        return next_task(frame);
    }
    return advance(task);
}

cy::Status ScriptedPlayer::advance(const Task& task) noexcept {
    switch (phase_) {
        case Phase::Aim:
            return aim(task);
        case Phase::Settle:
            if (phase_frames_ >= kSettleFrames) {
                phase_ = on_mark(task) ? Phase::Fire : Phase::Aim;
                phase_frames_ = 0;
            }
            return cy::ok();
        case Phase::Fire:
            ++fired_;
            ++findings_.trigger_pulls;
            trigger_down_ = true;
            phase_ = Phase::Recover;
            phase_frames_ = 0;
            return host_->press_button(MouseControl::Left, true);
        case Phase::Recover:
            if (phase_frames_ >= kRecoverFrames) {
                phase_ = Phase::Aim;
                phase_frames_ = 0;
            }
            return cy::ok();
    }
    return cy::ok();
}

cy::Status ScriptedPlayer::aim(const Task& task) noexcept {
    const Vec3 eye = host_->eye().translation;
    const Vec3 to = aim_point(task) - eye;
    const f32 flat = std::sqrt((to.x * to.x) + (to.z * to.z));
    const f32 yaw_error = wrap(std::atan2(to.x, -to.z) - host_->look_yaw());
    const f32 pitch_error = std::atan2(to.y, flat) - host_->look_pitch();
    if (std::fabs(yaw_error) < kOnAim && std::fabs(pitch_error) < kOnAim) {
        phase_ = Phase::Settle;
        phase_frames_ = 0;
        return cy::ok();
    }
    // Mouse counts: +x turns right, and the pointer's +y is down, so looking up is −y.
    const f32 dx = cy::math::clamp(yaw_error / kRadiansPerCount, -kMaxCounts, kMaxCounts);
    const f32 dy = cy::math::clamp(-pitch_error / kRadiansPerCount, -kMaxCounts, kMaxCounts);
    return host_->move_pointer(std::round(dx), std::round(dy));
}

}  // namespace sample::fps
