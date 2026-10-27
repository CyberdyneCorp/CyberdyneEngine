// SPDX-License-Identifier: MIT
// integration.game_backend_first_person — ABI 1.13 through the real behaviours.
//
// A camera given first-person behaviours through the table turns by the look it is fed, once per
// frame the host advances it; kicks, raises its sights and recovers; resets on a cut; and its
// weapon view reaches the frame as the renderer's own settings, its lens following the sights.

#include <cy/abi/cy_abi.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/first_person_backend.h>
#include <cy/test/test.h>

#include <cmath>

namespace {

using cy::f32;
using cy::u32;

constexpr CyCamera kCamera = 0x1'0000'0004ULL;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

struct Bound {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    cy::game_backend::FirstPersonAdapter adapter{cy::system_allocator(cy::MemoryDomain::Renderer)};
    explicit Bound(u32 capacity) {
        CY_REQUIRE(adapter.create(capacity).has_value());
        cy::game_backend::bind_first_person(host, &adapter);
    }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

[[nodiscard]] CyFirstPersonDesc desc() noexcept {
    CyFirstPersonDesc made{};
    made.struct_size = sizeof(made);
    made.look_sensitivity = 0.002F;
    made.aim_sensitivity = 0.5F;
    made.pitch_limit = 1.5F;
    made.hip_fov = 1.3F;
    made.aim_fov = 0.9F;
    made.hip_viewmodel_fov = 0.95F;
    made.aim_viewmodel_fov = 0.7F;
    made.aim_half_life = 0.05F;
    made.bob_stride = 1.6F;
    made.bob_vertical = 0.03F;
    made.bob_lateral = 0.02F;
    made.bob_reference_speed = 4.0F;
    made.recoil_frequency = 6.0F;
    made.recoil_damping = 0.75F;
    made.recoil_camera_share = 0.6F;
    made.hip_offset[0] = 0.2F;
    made.hip_offset[1] = -0.2F;
    made.hip_offset[2] = -0.35F;
    made.aim_offset[1] = -0.07F;
    made.aim_offset[2] = -0.28F;
    return made;
}

[[nodiscard]] CyFirstPersonState state_of(Bound& bound) {
    CyFirstPersonState state{};
    state.struct_size = sizeof(state);
    CY_REQUIRE_EQ(table().first_person_state(bound.engine(), kCamera, &state), CY_RESULT_OK);
    return state;
}

}  // namespace

CY_TEST_CASE("the look a game feeds turns the camera once, when the host advances it") {
    Bound bound(2);
    const CyFirstPersonDesc made = desc();
    CY_REQUIRE_EQ(table().first_person_configure(bound.engine(), kCamera, &made), CY_RESULT_OK);
    CyFirstPersonInput input{};
    input.struct_size = sizeof(input);
    input.look[0] = 60.0F;
    // Two inputs inside one frame add up: 120 counts.
    CY_REQUIRE_EQ(table().first_person_input(bound.engine(), kCamera, &input), CY_RESULT_OK);
    CY_REQUIRE_EQ(table().first_person_input(bound.engine(), kCamera, &input), CY_RESULT_OK);
    CY_CHECK_EQ(state_of(bound).yaw, 0.0F);  // nothing until the frame
    bound.adapter.advance(1.0F / 60.0F);
    CY_CHECK_NEAR(state_of(bound).yaw, 0.24F, 1e-6F);
    // The look is spent: the next frame with no input does not turn again.
    bound.adapter.advance(1.0F / 60.0F);
    CY_CHECK_NEAR(state_of(bound).yaw, 0.24F, 1e-6F);
    CY_CHECK_EQ(bound.adapter.cameras(), 1U);
}

CY_TEST_CASE("a kick climbs and recovers; the sights narrow both lenses; a reset is a cut") {
    Bound bound(1);
    const CyFirstPersonDesc made = desc();
    CY_REQUIRE_EQ(table().first_person_configure(bound.engine(), kCamera, &made), CY_RESULT_OK);
    CY_REQUIRE_EQ(table().first_person_kick(bound.engine(), kCamera, 3.0F, 0.0F), CY_RESULT_OK);
    bound.adapter.advance(0.03F);
    const CyFirstPersonState kicked = state_of(bound);
    CY_CHECK_GT(kicked.recoil[0], 0.0F);
    CY_CHECK_GT(kicked.view_aim[1], 0.0F);  // the view climbed
    for (u32 frame = 0; frame < 60; ++frame) {
        bound.adapter.advance(1.0F / 60.0F);
    }
    CY_CHECK_LT(std::fabs(state_of(bound).recoil[0]), 1e-4F);

    CyFirstPersonInput aiming{};
    aiming.struct_size = sizeof(aiming);
    aiming.flags = CY_FIRST_PERSON_AIMING;
    CY_REQUIRE_EQ(table().first_person_input(bound.engine(), kCamera, &aiming), CY_RESULT_OK);
    for (u32 frame = 0; frame < 60; ++frame) {
        bound.adapter.advance(1.0F / 60.0F);
    }
    const CyFirstPersonState raised = state_of(bound);
    CY_CHECK_NEAR(raised.aim, 1.0F, 1e-4F);
    CY_CHECK_NEAR(raised.fov, 0.9F, 1e-4F);
    CY_CHECK_NEAR(raised.viewmodel_fov, 0.7F, 1e-4F);
    CY_CHECK_NEAR(raised.weapon.position[0], 0.0F, 1e-4F);

    CY_REQUIRE_EQ(table().first_person_reset(bound.engine(), kCamera, 1.0F, 0.2F), CY_RESULT_OK);
    const CyFirstPersonState cut = state_of(bound);
    CY_CHECK_EQ(cut.yaw, 1.0F);
    CY_CHECK_EQ(cut.pitch, 0.2F);
    CY_CHECK_EQ(cut.aim, 0.0F);
    // The held sights were dropped with the cut: the next frame does not raise them again.
    bound.adapter.advance(1.0F / 60.0F);
    CY_CHECK_EQ(state_of(bound).aim, 0.0F);
}

CY_TEST_CASE("the weapon view reaches the frame as the renderer's settings, following the sights") {
    Bound bound(1);
    const CyFirstPersonDesc made = desc();
    CY_REQUIRE_EQ(table().first_person_configure(bound.engine(), kCamera, &made), CY_RESULT_OK);
    CY_CHECK(!bound.adapter.viewmodel(kCamera).enabled());
    CyViewmodelDesc view{};
    view.struct_size = sizeof(view);
    view.layers = 1U << 5U;
    view.near_plane = 0.01F;
    view.far_plane = 8.0F;
    view.depth_fraction = 0.0625F;
    view.flags = CY_VIEWMODEL_CAST_SHADOWS;
    CY_REQUIRE_EQ(table().viewmodel_configure(bound.engine(), kCamera, &view), CY_RESULT_OK);
    const cy::rendering::viewmodel::ViewmodelSettings hip = bound.adapter.viewmodel(kCamera);
    CY_CHECK(hip.enabled());
    CY_CHECK(static_cast<bool>(cy::rendering::viewmodel::validate(hip)));
    CY_CHECK_NEAR(hip.vertical_fov_radians, 0.95F, 1e-6F);
    CY_CHECK(hip.shadows.cast);
    CY_CHECK(hip.shadows.receive);

    CyFirstPersonInput aiming{};
    aiming.struct_size = sizeof(aiming);
    aiming.flags = CY_FIRST_PERSON_AIMING;
    CY_REQUIRE_EQ(table().first_person_input(bound.engine(), kCamera, &aiming), CY_RESULT_OK);
    for (u32 frame = 0; frame < 60; ++frame) {
        bound.adapter.advance(1.0F / 60.0F);
    }
    CY_CHECK_NEAR(bound.adapter.viewmodel(kCamera).vertical_fov_radians, 0.7F, 1e-4F);
    CyViewmodelDesc read{};
    read.struct_size = sizeof(read);
    CY_REQUIRE_EQ(table().viewmodel_get(bound.engine(), kCamera, &read), CY_RESULT_OK);
    CY_CHECK_NEAR(read.fov, 0.7F, 1e-4F);
    CY_CHECK_EQ(read.layers, view.layers);

    // A fixed lens does not follow.
    view.fov = 1.0F;
    CY_REQUIRE_EQ(table().viewmodel_configure(bound.engine(), kCamera, &view), CY_RESULT_OK);
    CY_CHECK_NEAR(bound.adapter.viewmodel(kCamera).vertical_fov_radians, 1.0F, 1e-6F);
}

CY_TEST_CASE("an unknown camera is not found, a full table refuses, a release frees the slot") {
    Bound bound(1);
    CyFirstPersonInput input{};
    input.struct_size = sizeof(input);
    CY_CHECK_EQ(table().first_person_input(bound.engine(), kCamera, &input), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().first_person_kick(bound.engine(), kCamera, 1.0F, 0.0F),
                CY_RESULT_NOT_FOUND);
    const CyFirstPersonDesc made = desc();
    CY_REQUIRE_EQ(table().first_person_configure(bound.engine(), kCamera, &made), CY_RESULT_OK);
    CY_CHECK_EQ(table().first_person_configure(bound.engine(), kCamera + 1, &made),
                CY_RESULT_OUT_OF_RANGE);
    // Reconfiguring the same camera keeps its state.
    input.look[0] = 100.0F;
    CY_REQUIRE_EQ(table().first_person_input(bound.engine(), kCamera, &input), CY_RESULT_OK);
    bound.adapter.advance(0.016F);
    CY_REQUIRE_EQ(table().first_person_configure(bound.engine(), kCamera, &made), CY_RESULT_OK);
    CY_CHECK_NEAR(state_of(bound).yaw, 0.2F, 1e-6F);
    CY_REQUIRE_EQ(table().first_person_release(bound.engine(), kCamera), CY_RESULT_OK);
    CY_CHECK(bound.adapter.pose(kCamera) == nullptr);
    CY_CHECK_EQ(table().first_person_release(bound.engine(), kCamera), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().first_person_configure(bound.engine(), kCamera + 1, &made), CY_RESULT_OK);
}
