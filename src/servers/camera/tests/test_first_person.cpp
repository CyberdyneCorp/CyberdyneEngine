// SPDX-License-Identifier: MIT
// The first-person camera's behaviours and the weapon view one camera produces beside its primary
// view.
//
// `camera-system` — "One camera, several views", "Stable smoothing" ("WHEN the same scene is played
// at 60 and at 144 frames per second THEN camera smoothing SHALL settle over the same wall-clock
// time"), "Shake, recoil, and the impulse bus" (recoil "SHALL be modelled as an impulse with a
// spring response and recovery"), "Aim is three things" and "Camera settings and accessibility".
//
// EVERY FRAME-RATE CASE RUNS ONE SECOND TWICE — at 60 and at 144 hertz — and holds the two ends to
// each other, which is the property; a case that pinned one frame rate's numbers would pass for a
// per-frame lerp too.

#include <cy/servers/camera/first_person.h>
#include <cy/servers/camera/view.h>
#include <cy/test/test.h>

#include <cmath>

using cy::f32;
using cy::u32;
using cy::Vec2;
using cy::Vec3;
using namespace cy::camera;

namespace {

/// Run `seconds` of a constant input at `hertz` and return the camera.
[[nodiscard]] FirstPersonCamera run(const FirstPersonSettings& settings, FirstPersonInput input,
                                    f32 seconds, u32 hertz, const RecoilImpulse* kick = nullptr) {
    FirstPersonCamera camera(settings);
    if (kick != nullptr) {
        camera.kick(*kick);
    }
    const u32 frames = static_cast<u32>(std::lround(seconds * static_cast<f32>(hertz)));
    input.delta_seconds = 1.0F / static_cast<f32>(hertz);
    for (u32 frame = 0; frame < frames; ++frame) {
        (void)camera.advance(input);
    }
    return camera;
}

[[nodiscard]] f32 length2(Vec2 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y));
}

}  // namespace

CY_TEST_CASE("mouse look turns by the sensitivity once, clamps the pitch and honours inversion") {
    FirstPersonSettings settings;
    settings.look.sensitivity = 0.002F;
    FirstPersonCamera camera(settings);
    FirstPersonInput input;
    input.look = Vec2{100.0F, 50.0F};
    input.delta_seconds = 1.0F / 60.0F;
    const FirstPersonPose& pose = camera.advance(input);
    // 100 counts at 0.002 radians a count is 0.2 radians — the one multiply, and no second one.
    CY_CHECK_NEAR(pose.yaw, 0.2F, 1e-6F);
    CY_CHECK_NEAR(pose.pitch, 0.1F, 1e-6F);
    // Turning right puts the forward axis toward +X; looking up toward +Y.
    CY_CHECK(pose.aims.control_aim.x > 0.0F);
    CY_CHECK(pose.aims.control_aim.y > 0.0F);

    input.look = Vec2{0.0F, 1.0e6F};
    CY_CHECK_NEAR(camera.advance(input).pitch, settings.look.pitch_limit_radians, 1e-6F);

    settings.look.invert_y = true;
    FirstPersonCamera inverted(settings);
    input.look = Vec2{0.0F, 50.0F};
    CY_CHECK_NEAR(inverted.advance(input).pitch, -0.1F, 1e-6F);
}

CY_TEST_CASE("with the sights up the look takes the aim sensitivity, blended by the raise") {
    FirstPersonSettings settings;
    settings.look.sensitivity = 0.001F;
    settings.look.aim_sensitivity = 0.5F;
    FirstPersonInput input;
    input.aiming = true;
    // Raise the sights fully, then turn with them up.
    FirstPersonCamera camera = run(settings, input, 2.0F, 60);
    CY_CHECK_NEAR(camera.pose().aim, 1.0F, 1e-4F);
    const f32 before = camera.pose().yaw;
    input.look = Vec2{100.0F, 0.0F};
    input.delta_seconds = 1.0F / 60.0F;
    CY_CHECK_NEAR(camera.advance(input).yaw - before, 0.05F, 1e-4F);
}

CY_TEST_CASE("the head bob ends a second on the same phase and height at 60 and at 144 hertz") {
    const FirstPersonSettings settings;
    FirstPersonInput input;
    input.move_speed = 3.5F;
    const FirstPersonCamera slow = run(settings, input, 1.0F, 60);
    const FirstPersonCamera fast = run(settings, input, 1.0F, 144);
    CY_CHECK_NEAR(slow.bob_phase(), fast.bob_phase(), 1e-3F);
    CY_CHECK_NEAR(slow.pose().eye_offset.y, fast.pose().eye_offset.y, 1e-4F);
    CY_CHECK_NEAR(slow.pose().eye_offset.x, fast.pose().eye_offset.x, 1e-4F);
    // It does bob: a second of walking at 3.5 m/s is over two strides.
    CY_CHECK(std::fabs(slow.pose().eye_offset.y) + std::fabs(slow.pose().eye_offset.x) > 1e-3F);
}

CY_TEST_CASE("the bob eases out when the walk stops and vanishes off the ground") {
    const FirstPersonSettings settings;
    FirstPersonInput input;
    input.move_speed = 4.0F;
    FirstPersonCamera camera = run(settings, input, 1.0F, 60);
    input.move_speed = 0.0F;
    input.delta_seconds = 1.0F / 60.0F;
    for (u32 frame = 0; frame < 120; ++frame) {
        (void)camera.advance(input);
    }
    CY_CHECK(std::fabs(camera.pose().eye_offset.y) < 1e-5F);
    input.move_speed = 4.0F;
    input.grounded = false;
    FirstPersonCamera airborne = run(settings, input, 1.0F, 60);
    CY_CHECK_EQ(airborne.bob_phase(), 0.0F);
    CY_CHECK_EQ(airborne.pose().eye_offset.y, 0.0F);
}

CY_TEST_CASE("motion reduction scales the bob and the camera's recoil together") {
    FirstPersonSettings settings;
    FirstPersonInput input;
    input.move_speed = 3.5F;
    const RecoilImpulse kick{2.0F, 0.0F};
    const FirstPersonCamera full = run(settings, input, 0.05F, 60, &kick);
    settings.motion_scale = 0.0F;
    const FirstPersonCamera reduced = run(settings, input, 0.05F, 60, &kick);
    CY_CHECK(std::fabs(full.pose().eye_offset.y) > 1e-4F);
    CY_CHECK_EQ(reduced.pose().eye_offset.y, 0.0F);
    // The weapon keeps its own recoil — the gun still kicks — but the view does not move.
    CY_CHECK_NEAR(reduced.pose().aims.view_aim.y, reduced.pose().aims.control_aim.y, 1e-6F);
    CY_CHECK(full.pose().aims.view_aim.y != full.pose().aims.control_aim.y);
}

CY_TEST_CASE("the spring's closed form is exact: one step of 2dt is two steps of dt") {
    for (const f32 zeta : {0.4F, 1.0F, 1.8F}) {
        f32 one_x = 0.0F;
        f32 one_v = 3.0F;
        step_spring(one_x, one_v, 6.0F, zeta, 0.02F);
        f32 two_x = 0.0F;
        f32 two_v = 3.0F;
        step_spring(two_x, two_v, 6.0F, zeta, 0.01F);
        step_spring(two_x, two_v, 6.0F, zeta, 0.01F);
        CY_CHECK_NEAR(one_x, two_x, 1e-6F);
        CY_CHECK_NEAR(one_v, two_v, 1e-5F);
        CY_CHECK(one_x > 0.0F);
    }
}

CY_TEST_CASE("recoil kicks the aim up and recovers to it over the same time at 60 and 144 hertz") {
    const FirstPersonSettings settings;
    const FirstPersonInput input;
    const RecoilImpulse kick{3.0F, -0.6F};
    // Early in the kick, both rates are part way up the same curve.
    const FirstPersonCamera slow_early = run(settings, input, 1.0F / 12.0F, 60, &kick);
    const FirstPersonCamera fast_early = run(settings, input, 1.0F / 12.0F, 144, &kick);
    CY_CHECK(slow_early.pose().recoil.x > 0.0F);
    CY_CHECK(slow_early.pose().recoil.y < 0.0F);
    CY_CHECK_NEAR(slow_early.pose().recoil.x, fast_early.pose().recoil.x, 1e-5F);
    CY_CHECK_NEAR(slow_early.pose().recoil.y, fast_early.pose().recoil.y, 1e-5F);
    // A second later it has recovered, at both rates, and the view is the control aim again.
    const FirstPersonCamera slow = run(settings, input, 1.0F, 60, &kick);
    const FirstPersonCamera fast = run(settings, input, 1.0F, 144, &kick);
    CY_CHECK(length2(slow.pose().recoil) < 1e-4F);
    CY_CHECK(length2(fast.pose().recoil) < 1e-4F);
    CY_CHECK_NEAR(slow.pose().aims.view_aim.y, slow.pose().aims.control_aim.y, 1e-4F);
}

CY_TEST_CASE("the kick's peak is the documented one: about 30 ms in, at about pitch / 85") {
    FirstPersonCamera camera;
    camera.kick(RecoilImpulse{1.0F, 0.0F});
    FirstPersonInput input;
    input.delta_seconds = 0.001F;
    f32 peak = 0.0F;
    u32 peak_ms = 0;
    for (u32 ms = 1; ms <= 200; ++ms) {
        const f32 x = camera.advance(input).recoil.x;
        if (x > peak) {
            peak = x;
            peak_ms = ms;
        }
    }
    CY_CHECK_GE(peak_ms, 25U);
    CY_CHECK_LE(peak_ms, 35U);
    CY_CHECK_NEAR(peak, 1.0F / 85.0F, 0.002F);
    CY_CHECK(std::fabs(camera.pose().recoil.x) < 0.02F * peak);  // two per cent, at 200 ms
}

CY_TEST_CASE("aiming down the sights narrows both lenses over the same time at 60 and 144 hertz") {
    const FirstPersonSettings settings;
    FirstPersonInput input;
    input.aiming = true;
    // A twelfth of a second: five frames at 60 hertz and twelve at 144, the same wall clock.
    const FirstPersonCamera slow = run(settings, input, 1.0F / 12.0F, 60);
    const FirstPersonCamera fast = run(settings, input, 1.0F / 12.0F, 144);
    CY_CHECK_NEAR(slow.pose().aim, fast.pose().aim, 1e-4F);
    CY_CHECK_NEAR(slow.pose().lens.vertical_fov_radians(), fast.pose().lens.vertical_fov_radians(),
                  1e-4F);
    const FirstPersonCamera raised = run(settings, input, 1.0F, 60);
    CY_CHECK_NEAR(raised.pose().lens.vertical_fov_radians(),
                  settings.aim.aim_lens.vertical_fov_radians(), 1e-4F);
    CY_CHECK_NEAR(raised.pose().viewmodel_fov_radians, settings.aim.aim_viewmodel_fov_radians,
                  1e-4F);
    // The weapon comes in under the eye: the sight line is the view's.
    CY_CHECK_NEAR(raised.pose().weapon.translation.x, settings.aim.aim_offset.x, 1e-4F);
    // And the hip lens is the hip lens.
    const FirstPersonCamera hip = run(settings, FirstPersonInput{}, 1.0F, 60);
    CY_CHECK_NEAR(hip.pose().lens.vertical_fov_radians(),
                  settings.aim.hip_lens.vertical_fov_radians(), 1e-6F);
}

CY_TEST_CASE("a reset is a cut: the look set and every smoothed state at rest") {
    FirstPersonCamera camera;
    camera.kick(RecoilImpulse{2.0F, 1.0F});
    FirstPersonInput input;
    input.move_speed = 4.0F;
    input.aiming = true;
    input.delta_seconds = 0.05F;
    (void)camera.advance(input);
    camera.reset(1.0F, 0.25F);
    CY_CHECK_EQ(camera.pose().yaw, 1.0F);
    CY_CHECK_EQ(camera.pose().pitch, 0.25F);
    CY_CHECK_EQ(camera.pose().aim, 0.0F);
    CY_CHECK_EQ(camera.pose().recoil.x, 0.0F);
    CY_CHECK_EQ(camera.recoil_velocity().x, 0.0F);
    CY_CHECK_EQ(camera.bob_phase(), 0.0F);
}

CY_TEST_CASE("the weapon is placed in the eye's space and follows the eye into the world") {
    FirstPersonCamera camera;
    FirstPersonInput input;
    input.look = Vec2{1.5708F / 0.0022F, 0.0F};  // a quarter turn right
    input.delta_seconds = 1.0F / 60.0F;
    const FirstPersonPose& pose = camera.advance(input);
    const cy::Transform eye = first_person_eye(pose, Vec3{10.0F, 1.7F, 0.0F});
    const cy::Transform weapon = first_person_weapon(pose, eye);
    // Facing +X now, the weapon held 0.35 m ahead is 0.35 m along +X from the eye.
    CY_CHECK_NEAR(weapon.translation.x - eye.translation.x, 0.35F, 1e-3F);
    CY_CHECK_NEAR(weapon.translation.y - eye.translation.y, -0.2F, 1e-3F);
}

CY_TEST_CASE("one camera, two views: the weapon view shares the pose and keeps its own history") {
    EvaluatedCamera camera;
    camera.pose.translation = Vec3{1.0F, 1.7F, -3.0F};
    camera.history_id = 77;
    camera.lens.gameplay.vertical_fov_radians = 1.3F;
    RenderViewRequest primary;
    primary.viewport = cy::render::ViewportRect{0, 0, 1280, 720};
    primary.layer_mask = 0x1;
    Lens weapon_lens;
    weapon_lens.gameplay.vertical_fov_radians = 0.9F;
    weapon_lens.gameplay.near_plane = 0.01F;

    cy::render::ViewDescription world;
    cy::render::ViewDescription weapon;
    CY_REQUIRE(static_cast<bool>(produce_view(camera, primary, world)));
    CY_REQUIRE(static_cast<bool>(produce_weapon_view(camera, primary, weapon_lens, 0x2, weapon)));
    CY_CHECK(weapon.camera == world.camera);
    CY_CHECK(weapon.purpose == cy::render::ViewPurpose::Viewmodel);
    CY_CHECK_EQ(weapon.layer_mask, 0x2U);
    CY_CHECK_NEAR(weapon.projection.fov_y_radians, 0.9F, 1e-6F);
    CY_CHECK_NEAR(world.projection.fov_y_radians, 1.3F, 1e-6F);
    CY_CHECK_NE(weapon.history_id, world.history_id);
    CY_CHECK_EQ(weapon.history_id, history_identity(camera, kWeaponViewKey));
    // A weapon view of no layer draws nothing and is refused.
    CY_CHECK(!produce_weapon_view(camera, primary, weapon_lens, 0, weapon));
}

CY_TEST_CASE("the three aims: the view takes its share of the recoil, the weapon all of it") {
    FirstPersonSettings settings;
    settings.recoil.camera_share = 0.5F;
    FirstPersonCamera camera(settings);
    camera.kick(RecoilImpulse{4.0F, 0.0F});
    FirstPersonInput input;
    input.delta_seconds = 0.03F;
    const FirstPersonPose& pose = camera.advance(input);
    const f32 control = std::asin(pose.aims.control_aim.y);
    const f32 view = std::asin(pose.aims.view_aim.y);
    const f32 weapon = std::asin(pose.aims.weapon_aim.y);
    // Control aim is the player's look alone — what a command carries and a server validates —
    // the view climbs by half the recoil, and the weapon, turned in the eye by the other half,
    // by all of it: shots follow the weapon while the reticle stays on the control aim.
    CY_CHECK_NEAR(control, 0.0F, 1e-6F);
    CY_CHECK_NEAR(view, pose.recoil.x * 0.5F, 1e-5F);
    CY_CHECK_NEAR(weapon, pose.recoil.x, 1e-5F);
    CY_CHECK_GT(pose.recoil.x, 0.01F);
}
