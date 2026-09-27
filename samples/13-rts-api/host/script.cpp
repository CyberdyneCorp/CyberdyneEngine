// SPDX-License-Identifier: MIT
#include "script.h"

namespace sample::rts {
namespace {

using cy::f32;
using cy::u64;
using cy::Vec3;
using cy::input::Key;
using cy::input::MouseControl;

constexpr f32 kCentreX = 640.0F;
constexpr f32 kCentreY = 360.0F;
constexpr f32 kLeftEdgeX = 2.0F;

/// Half a worker's height: aim at the middle of the capsule, not at its feet.
constexpr f32 kAimHeight = 0.9F;
/// The ground point the selected unit is sent to.
constexpr Vec3 kOrderTarget{22.0F, 0.0F, 26.0F};

constexpr u64 kKeysDown = 1;
constexpr u64 kKeysUp = 31;
constexpr u64 kEdgeStart = 40;
constexpr u64 kEdgeEnd = 70;
constexpr u64 kSelectAim = 80;
constexpr u64 kOrderAim = 90;
constexpr u64 kBuildDown = 360;
constexpr u64 kBuildUp = 361;

/// Frames after which the camera is sampled. A `setTarget` made in a frame's `onUpdate` shows when
/// the next frame evaluates the rig, so frame 1 shows where frame 0 placed it — before the first
/// pan — and each later sample is taken one frame past the pan's last input.
constexpr u64 kSampleStart = kKeysDown;
constexpr u64 kSampleKeys = kKeysUp + 2;
constexpr u64 kSampleEdge = kEdgeEnd + 2;

}  // namespace

cy::Status Player::before_frame(u64 frame) noexcept {
    if (cy::Status camera = camera_steps(frame); !camera) {
        return camera;
    }
    if (cy::Status order = order_steps(frame); !order) {
        return order;
    }
    if (frame == kBuildDown || frame == kBuildUp) {
        return host_->press_key(Key::B, frame == kBuildDown);
    }
    return cy::ok();
}

cy::Status Player::camera_steps(u64 frame) noexcept {
    switch (frame) {
        case 0:
        case kEdgeEnd:
            return host_->move_pointer(kCentreX, kCentreY);
        case kKeysDown:
        case kKeysUp:
            return host_->press_key(Key::D, frame == kKeysDown);
        case kEdgeStart:
            return host_->move_pointer(kLeftEdgeX, kCentreY);
        default:
            return cy::ok();
    }
}

cy::Status Player::order_steps(u64 frame) noexcept {
    switch (frame) {
        case kSelectAim:
            return aim_at_second_unit();
        case kSelectAim + 1:
        case kSelectAim + 2:
            return host_->press_button(MouseControl::Left, frame == kSelectAim + 1);
        case kOrderAim:
            findings_.target = kOrderTarget;
            return aim_at(kOrderTarget);
        case kOrderAim + 1:
        case kOrderAim + 2:
            return host_->press_button(MouseControl::Right, frame == kOrderAim + 1);
        default:
            return cy::ok();
    }
}

cy::Status Player::aim_at_second_unit() noexcept {
    if (host_->unit_count() < 2U) {
        ++findings_.missed_aims;
        return cy::ok();
    }
    findings_.bystander = host_->unit(0);
    findings_.clicked = host_->unit(1);
    findings_.bystander_start = host_->unit_position(findings_.bystander);
    const Vec3 feet = host_->unit_position(findings_.clicked);
    return aim_at(Vec3{feet.x, feet.y + kAimHeight, feet.z});
}

cy::Status Player::aim_at(Vec3 point) noexcept {
    f32 x = 0.0F;
    f32 y = 0.0F;
    if (!host_->project(point, x, y)) {
        ++findings_.missed_aims;
        return cy::ok();
    }
    return host_->move_pointer(x, y);
}

void Player::after_frame(u64 frame) noexcept {
    const f32 camera_x = host_->camera_position().x;
    if (frame == kSampleStart) {
        findings_.camera_start = camera_x;
    } else if (frame == kSampleKeys) {
        findings_.camera_after_keys = camera_x;
    } else if (frame == kSampleEdge) {
        findings_.camera_after_edge = camera_x;
    }
    if (findings_.arrived_frame == 0U && findings_.clicked != CY_ENTITY_NULL &&
        host_->unit_status(findings_.clicked) == CY_NAV_PATH_STATUS_ARRIVED) {
        findings_.arrived_frame = frame;
    }
    const cy::u32 voices = host_->observe().active_voices;
    findings_.peak_voices = voices > findings_.peak_voices ? voices : findings_.peak_voices;
}

}  // namespace sample::rts
