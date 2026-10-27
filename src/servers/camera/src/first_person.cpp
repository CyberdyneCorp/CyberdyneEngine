// SPDX-License-Identifier: MIT
// The first-person camera's behaviours. See cy/servers/camera/first_person.h.

#include <cy/servers/camera/first_person.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::camera {
namespace {

/// Damping ratios this close to one are stepped as critical: the under- and over-damped forms
/// divide by `sqrt(|1 - ζ²|)`, which loses every digit as it goes to zero.
constexpr f32 kCriticalBand = 1.0e-4F;

/// The eye's orientation for a yaw (right) and a pitch (up): −Z forward, so turning right is a
/// negative rotation about +Y and looking up a positive one about +X.
[[nodiscard]] Quat look_orientation(f32 yaw, f32 pitch) noexcept {
    return Quat::from_axis_angle(Vec3{0.0F, 1.0F, 0.0F}, -yaw) *
           Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, pitch);
}

/// The bob's amplitude factor for a speed: linear in speed up to the reference, clamped at one and
/// a half times it, and none off the ground.
[[nodiscard]] f32 bob_target(const HeadBobSettings& bob, const FirstPersonInput& input) noexcept {
    if (!input.grounded || bob.reference_speed <= 0.0F) {
        return 0.0F;
    }
    return math::clamp(input.move_speed / bob.reference_speed, 0.0F, 1.5F);
}

}  // namespace

void step_spring(f32& position, f32& velocity, f32 frequency_hz, f32 damping_ratio,
                 f32 delta_seconds) noexcept {
    if (delta_seconds <= 0.0F) {
        return;
    }
    if (frequency_hz <= 0.0F) {
        // No spring: the impulse is spent, nothing recovers. A recoil with no spring is a setting
        // somebody zeroed, and freezing the offset is the honest answer rather than a division.
        position += velocity * delta_seconds;
        velocity = 0.0F;
        return;
    }
    const f32 omega = 2.0F * math::kPi * frequency_hz;
    const f32 zeta = damping_ratio < 0.0F ? 0.0F : damping_ratio;
    const f32 t = delta_seconds;
    const f32 x0 = position;
    const f32 v0 = velocity;
    if (std::fabs(zeta - 1.0F) < kCriticalBand) {
        // x = e^(-ωt) (x0 + (v0 + ω x0) t)
        const f32 decay = std::exp(-omega * t);
        const f32 b = v0 + (omega * x0);
        position = decay * (x0 + (b * t));
        velocity = decay * (v0 - (omega * b * t));
        return;
    }
    if (zeta < 1.0F) {
        // x = e^(-ζωt) (x0 cos ωd t + ((v0 + ζω x0) / ωd) sin ωd t)
        const f32 a = zeta * omega;
        const f32 damped = omega * std::sqrt(1.0F - (zeta * zeta));
        const f32 decay = std::exp(-a * t);
        const f32 c = std::cos(damped * t);
        const f32 s = std::sin(damped * t);
        const f32 b = (v0 + (a * x0)) / damped;
        position = decay * ((x0 * c) + (b * s));
        velocity = decay * ((v0 * c) - (((a * b) + (damped * x0)) * s));
        return;
    }
    // x = c1 e^(r1 t) + c2 e^(r2 t), r = -ω (ζ ∓ sqrt(ζ² - 1))
    const f32 root = std::sqrt((zeta * zeta) - 1.0F);
    const f32 r1 = -omega * (zeta - root);
    const f32 r2 = -omega * (zeta + root);
    const f32 c1 = (v0 - (r2 * x0)) / (r1 - r2);
    const f32 c2 = x0 - c1;
    const f32 e1 = std::exp(r1 * t);
    const f32 e2 = std::exp(r2 * t);
    position = (c1 * e1) + (c2 * e2);
    velocity = (c1 * r1 * e1) + (c2 * r2 * e2);
}

FirstPersonCamera::FirstPersonCamera() noexcept : FirstPersonCamera(FirstPersonSettings{}) {}

FirstPersonCamera::FirstPersonCamera(const FirstPersonSettings& settings) noexcept
    : settings_(settings) {
    compose();
}

void FirstPersonCamera::kick(const RecoilImpulse& impulse) noexcept {
    recoil_velocity_.x += impulse.pitch;
    recoil_velocity_.y += impulse.yaw;
}

void FirstPersonCamera::reset(f32 yaw, f32 pitch) noexcept {
    pose_.yaw = yaw;
    pose_.pitch =
        math::clamp(pitch, -settings_.look.pitch_limit_radians, settings_.look.pitch_limit_radians);
    pose_.recoil = Vec2{0.0F, 0.0F};
    recoil_velocity_ = Vec2{0.0F, 0.0F};
    bob_phase_ = 0.0F;
    bob_amount_.reset(0.0F);
    aim_.reset(0.0F);
    compose();
}

const FirstPersonPose& FirstPersonCamera::advance(const FirstPersonInput& input) noexcept {
    const FirstPersonLookSettings& look = settings_.look;
    const f32 dt = input.delta_seconds > 0.0F ? input.delta_seconds : 0.0F;

    // THE LOOK, AND THE ONE PLACE ITS SENSITIVITY IS APPLIED — see the header comment. The aim
    // multiplier blends with how far the sights are up, so a flick during the raise is neither the
    // hip's nor the sight's speed but the frame's.
    const f32 scale = look.sensitivity * math::lerp(1.0F, look.aim_sensitivity, aim_.value());
    pose_.yaw += input.look.x * scale;
    const f32 up = look.invert_y ? -input.look.y : input.look.y;
    pose_.pitch = math::clamp(pose_.pitch + (up * scale), -look.pitch_limit_radians,
                              look.pitch_limit_radians);

    // THE SIGHTS, as a half-life toward up or down.
    aim_.advance(input.aiming ? 1.0F : 0.0F, settings_.aim.half_life_seconds, dt);

    // THE BOB: a phase advanced by the distance walked, an amplitude eased toward the speed's.
    const HeadBobSettings& bob = settings_.bob;
    if (input.grounded && bob.stride_metres > 0.0F) {
        bob_phase_ += 2.0F * math::kPi * (input.move_speed * dt) / bob.stride_metres;
        bob_phase_ = std::fmod(bob_phase_, 2.0F * math::kPi);
    }
    bob_amount_.advance(bob_target(bob, input), bob.blend_half_life_seconds, dt);

    // THE RECOIL, stepped exactly, pitch and yaw as two springs with one character.
    const RecoilSettings& recoil = settings_.recoil;
    step_spring(pose_.recoil.x, recoil_velocity_.x, recoil.frequency_hz, recoil.damping_ratio, dt);
    step_spring(pose_.recoil.y, recoil_velocity_.y, recoil.frequency_hz, recoil.damping_ratio, dt);

    compose();
    return pose_;
}

void FirstPersonCamera::compose() noexcept {
    const f32 motion = math::clamp(settings_.motion_scale, 0.0F, 1.0F);
    const f32 aim = aim_.value();
    pose_.aim = aim;

    // THE CAMERA'S SHARE OF THE RECOIL, reduced with the rest of the head's motion.
    const RecoilSettings& recoil = settings_.recoil;
    const f32 share = math::clamp(recoil.camera_share, 0.0F, 1.0F);
    const Vec2 camera_recoil = pose_.recoil * (share * motion);
    pose_.orientation =
        look_orientation(pose_.yaw + camera_recoil.y, pose_.pitch + camera_recoil.x);

    const HeadBobSettings& bob = settings_.bob;
    const f32 amount = bob_amount_.value() * math::lerp(1.0F, bob.aim_scale, aim) * motion;
    pose_.eye_offset = Vec3{bob.lateral_metres * amount * std::sin(bob_phase_),
                            bob.vertical_metres * amount * std::sin(2.0F * bob_phase_), 0.0F};

    const AimSettings& sights = settings_.aim;
    pose_.lens = blend(sights.hip_lens, sights.aim_lens, aim);
    pose_.viewmodel_fov_radians =
        math::lerp(sights.hip_viewmodel_fov_radians, sights.aim_viewmodel_fov_radians, aim);

    // THE WEAPON, in the eye's space: held, bobbing a little against the eye, slid back by the
    // recoil and turned by the share of it the camera did not take.
    const Vec3 held = sights.hip_offset + ((sights.aim_offset - sights.hip_offset) * aim);
    const f32 slide =
        std::sqrt((pose_.recoil.x * pose_.recoil.x) + (pose_.recoil.y * pose_.recoil.y)) *
        recoil.kick_back_per_radian;
    pose_.weapon = Transform::identity();
    pose_.weapon.translation =
        held - (pose_.eye_offset * bob.weapon_scale) + Vec3{0.0F, 0.0F, slide};
    const Vec2 weapon_recoil = pose_.recoil * (1.0F - share);
    pose_.weapon.rotation = look_orientation(weapon_recoil.y, weapon_recoil.x);

    // THE THREE AIMS.
    const Vec3 forward{0.0F, 0.0F, -1.0F};
    pose_.aims.view_aim = pose_.orientation * forward;
    pose_.aims.control_aim = look_orientation(pose_.yaw, pose_.pitch) * forward;
    pose_.aims.weapon_aim = (pose_.orientation * pose_.weapon.rotation) * forward;
}

Transform first_person_eye(const FirstPersonPose& pose, Vec3 head) noexcept {
    Transform eye = Transform::from_rotation(pose.orientation);
    eye.translation = head + (pose.orientation * pose.eye_offset);
    return eye;
}

Transform first_person_weapon(const FirstPersonPose& pose, const Transform& eye) noexcept {
    return eye * pose.weapon;
}

}  // namespace cy::camera
