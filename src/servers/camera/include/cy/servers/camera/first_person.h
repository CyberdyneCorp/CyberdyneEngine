// SPDX-License-Identifier: MIT
#pragma once
// The first-person camera's behaviours: mouse look with a sensitivity, head bob, recoil kick and
// recovery, and the change of field of view for aiming down the sights — and the weapon view's
// placement and lens that follow from them.
//
// `camera-system` — "Camera intent", "Stable smoothing", "Aim is three things", "Shake, recoil,
// and the impulse bus", "Camera settings and accessibility", and "One camera, several views".
//
// ================================================================================================
// A VALUE, NOT A CAMERA
// ================================================================================================
//
// `FirstPersonCamera` holds the state those behaviours integrate — the look angles, the bob's
// phase, the recoil spring, the aim blend — and turns one frame's input into a `FirstPersonPose`.
// It writes no transform anywhere. The pose is a rig parameter in the requirement's sense: the
// host's first-person rig reads it, places its eye at the character's head and the weapon view's
// instance at `weapon_offset` in that eye's space, and produces the evaluated camera and the two
// views from there. There is no camera base class here and nothing to inherit from.
//
// ================================================================================================
// THE ONE PLACE THE LOOK SENSITIVITY IS APPLIED
// ================================================================================================
//
// "One sensitivity setting — it SHALL apply once in the pipeline, and no camera code SHALL apply it
// a second time". A first-person look is fed the DEVICE's deltas — mouse counts, a stick's
// deflection times the frame's seconds — through an input action with NO `Sensitivity` processor
// on its binding, and `FirstPersonLookSettings::sensitivity` turns them into radians here, once,
// scaled by `aim_sensitivity` as the sights come up. That is the requirement's "rig parameter"
// form of the one place; a binding that ALSO scaled would be the duplication it forbids, and the
// guide says so where it shows the binding.
//
// ================================================================================================
// EVERY BEHAVIOUR IS FRAME-RATE INDEPENDENT, AND THE TESTS SAY HOW
// ================================================================================================
//
// * The bob's phase advances by the DISTANCE walked over the stride length, so a second at 60 Hz
//   and a second at 144 Hz end on the same phase.
// * The bob's amplitude and the aim blend are `SmoothScalar` half-lives (`smoothing.h`), which
//   compose exactly.
// * The recoil is a damped spring stepped by its CLOSED FORM — the exact solution of
//   `x'' + 2ζω x' + ω² x = 0` over the step — so the step size does not change the trajectory.
//   "Recoil SHALL be modelled as an impulse with a spring response and recovery, integrated with
//   the aim layer rather than expressed as random noise."
//
// `tests/test_first_person.cpp` runs each at 60 and 144 hertz and holds the two to each other.

#include <cy/core/base/types.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/transform.h>
#include <cy/core/math/vec.h>
#include <cy/servers/camera/camera.h>
#include <cy/servers/camera/lens.h>
#include <cy/servers/camera/smoothing.h>

namespace cy::camera {

/// Mouse look. See the header comment for why the sensitivity is applied here.
struct FirstPersonLookSettings {
    /// Radians per device count: 0.0022 is about 0.126 degrees a mouse count.
    f32 sensitivity = 0.0022F;
    /// The multiplier on `sensitivity` with the sights fully up, blended by the aim amount — what
    /// `CameraSettings::aim_sensitivity` reports.
    f32 aim_sensitivity = 0.7F;
    bool invert_y = false;
    /// The pitch is clamped to plus or minus this, so the camera never flips over the pole.
    f32 pitch_limit_radians = 1.5533430F;  // 89 degrees
};

/// Head bob: the eye rising and swaying with the stride.
struct HeadBobSettings {
    /// Metres walked per full cycle — two steps. The phase advances by distance, never by time.
    f32 stride_metres = 1.6F;
    /// Metres at `reference_speed`, scaled by speed and clamped at 1.5 times.
    f32 vertical_metres = 0.03F;
    f32 lateral_metres = 0.02F;
    f32 reference_speed = 4.0F;
    /// How fast the bob eases in when walking starts and out when it stops.
    f32 blend_half_life_seconds = 0.08F;
    /// The bob that is left with the sights fully up.
    f32 aim_scale = 0.2F;
    /// The weapon's share of the eye's bob, in the eye's space: a weapon that bobbed exactly with
    /// the eye would not move on screen at all.
    f32 weapon_scale = 0.35F;
};

/// Recoil: an impulse into a damped spring that recovers to the aim.
struct RecoilSettings {
    /// The spring's natural frequency. Higher recovers faster.
    f32 frequency_hz = 6.0F;
    /// Below one the aim springs past and settles; one is critically damped.
    f32 damping_ratio = 0.75F;
    /// How much of the recoil the CAMERA takes; the rest is the weapon's own rotation in the eye's
    /// space. Weapon aim is not view aim — "after offsets, recoil, and animation".
    f32 camera_share = 0.6F;
    /// Metres the weapon slides back toward the eye per radian of recoil.
    f32 kick_back_per_radian = 0.35F;
};

/// Aiming down the sights: the world's lens, the weapon view's lens, and where the weapon is held.
struct AimSettings {
    /// The world's lens from the hip and with the sights up. Blended by `camera::blend`, which
    /// respects the lens model.
    Lens hip_lens;
    Lens aim_lens;
    /// The weapon view's vertical field of view from the hip and with the sights up.
    f32 hip_viewmodel_fov_radians = 0.95993109F;  // 55 degrees
    f32 aim_viewmodel_fov_radians = 0.69813170F;  // 40 degrees
    /// How fast the sights come up and go down.
    f32 half_life_seconds = 0.06F;
    /// The weapon's offset in the EYE's space (−Z forward): at the hip, low and to the right; with
    /// the sights up, centred under the eye so the sight line is the view's.
    Vec3 hip_offset{0.2F, -0.2F, -0.35F};
    Vec3 aim_offset{0.0F, -0.07F, -0.28F};

    AimSettings() noexcept {
        hip_lens.gameplay.vertical_fov_radians = 1.3089969F;  // 75 degrees
        aim_lens.gameplay.vertical_fov_radians = 0.9599311F;  // 55 degrees
    }
};

struct FirstPersonSettings {
    FirstPersonLookSettings look;
    HeadBobSettings bob;
    RecoilSettings recoil;
    AimSettings aim;
    /// `camera-system`'s motion reduction: scales the bob and the camera's share of the recoil
    /// together. One reduces nothing; zero leaves neither. `CameraSettings::reduce_motion` is a
    /// host's reason to lower it.
    f32 motion_scale = 1.0F;
};

/// One frame's input.
struct FirstPersonInput {
    /// The device's deltas since the last frame — mouse counts, or a stick's deflection times the
    /// frame's seconds — with no sensitivity applied. +x turns right, +y looks up.
    Vec2 look{0.0F, 0.0F};
    /// Metres a second over the ground.
    f32 move_speed = 0.0F;
    /// Off the ground the stride stops and the bob eases out.
    bool grounded = true;
    /// The sights are held up.
    bool aiming = false;
    f32 delta_seconds = 0.0F;
};

/// A shot's kick: an impulse into the recoil spring, as angular velocity. With the default spring
/// the displacement peaks about 30 milliseconds later at roughly `pitch / 85` radians and is back
/// within two per cent of that peak a fifth of a second after the shot.
struct RecoilImpulse {
    /// Radians a second, up.
    f32 pitch = 0.0F;
    /// Radians a second, right.
    f32 yaw = 0.0F;
};

/// What one frame of the behaviours produced. Everything a first-person rig needs to place the eye
/// and the weapon view.
struct FirstPersonPose {
    /// The look the player controls, recoil excluded: what travels in commands as control aim.
    f32 yaw = 0.0F;
    f32 pitch = 0.0F;
    /// The recoil spring's displacement, (pitch, yaw) radians, before it is shared out.
    Vec2 recoil{0.0F, 0.0F};
    /// The eye's orientation: the look plus the camera's share of the recoil.
    Quat orientation = Quat::identity();
    /// The bob, in the EYE's space, metres: added to the head position the rig follows.
    Vec3 eye_offset{0.0F, 0.0F, 0.0F};
    /// The world's lens, the sights blended in.
    Lens lens;
    /// The weapon view's vertical field of view, the sights blended in.
    f32 viewmodel_fov_radians = 0.95993109F;
    /// The weapon's placement in the EYE's space: the held offset, its share of the bob, the
    /// recoil's slide back and the recoil the camera did not take.
    Transform weapon;
    /// 0 at the hip, 1 with the sights fully up.
    f32 aim = 0.0F;
    /// `camera-system`'s three aims, as directions in the world: where the camera looks (recoil
    /// included), where the player aims (none), and where the weapon points (all of it).
    AimState aims;
};

/// The behaviours' state. One per first-person camera; stepped once a frame.
class FirstPersonCamera {
public:
    FirstPersonCamera() noexcept;
    explicit FirstPersonCamera(const FirstPersonSettings& settings) noexcept;

    void set_settings(const FirstPersonSettings& settings) noexcept { settings_ = settings; }
    [[nodiscard]] const FirstPersonSettings& settings() const noexcept { return settings_; }

    /// Add a shot's kick to the recoil spring. It takes effect from the next `advance`.
    void kick(const RecoilImpulse& impulse) noexcept;

    /// One frame. A non-positive delta advances nothing time-dependent but still applies the look.
    const FirstPersonPose& advance(const FirstPersonInput& input) noexcept;

    /// A cut: the look set, the bob, the recoil and the aim at rest — `camera-system`'s "smoothing
    /// state SHALL be resettable on cuts and teleports".
    void reset(f32 yaw, f32 pitch) noexcept;

    [[nodiscard]] const FirstPersonPose& pose() const noexcept { return pose_; }
    /// The bob's phase, radians, for a test that holds two frame rates to each other.
    [[nodiscard]] f32 bob_phase() const noexcept { return bob_phase_; }
    /// The recoil spring's velocity, (pitch, yaw) radians a second.
    [[nodiscard]] Vec2 recoil_velocity() const noexcept { return recoil_velocity_; }

private:
    void compose() noexcept;

    FirstPersonSettings settings_;
    FirstPersonPose pose_;
    f32 bob_phase_ = 0.0F;
    SmoothScalar bob_amount_;
    SmoothScalar aim_;
    Vec2 recoil_velocity_{0.0F, 0.0F};
};

/// The closed-form step of a damped spring at rest at zero: `x'' + 2ζω x' + ω² x = 0` from
/// (`position`, `velocity`) over `delta_seconds`. Exact for every step size, which is what makes
/// the recoil frame-rate independent. Public so the property can be tested on its own.
void step_spring(f32& position, f32& velocity, f32 frequency_hz, f32 damping_ratio,
                 f32 delta_seconds) noexcept;

/// The eye's pose in the world: the head position the rig follows plus the bob, in the pose's
/// orientation.
[[nodiscard]] Transform first_person_eye(const FirstPersonPose& pose, Vec3 head) noexcept;

/// The weapon view's instance placement in the world: `pose.weapon` in the eye's space, composed
/// with the eye. What a host writes as the weapon's instance row every frame — camera-attached, so
/// it is lit where it is.
[[nodiscard]] Transform first_person_weapon(const FirstPersonPose& pose,
                                            const Transform& eye) noexcept;

}  // namespace cy::camera
