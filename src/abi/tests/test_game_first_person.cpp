// SPDX-License-Identifier: MIT
// ABI 1.13's first-person thunks against a fake backend.
//
// What the thunks own: the phase rule — every entry `[N U]` — the null camera, the argument and
// range checks, `struct_size` both ways, and UNAVAILABLE with no backend bound. The behaviours
// themselves are integration.game_backend_first_person's.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/first_person.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstddef>
#include <limits>

namespace {

using cy::f32;
using cy::u32;

constexpr CyCamera kCamera = 0x1'0000'0002ULL;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

class FakeFirstPerson final : public cy::abi::game::FirstPersonBackend {
public:
    CyFirstPersonDesc configured{};
    CyFirstPersonInput last_input{};
    CyViewmodelDesc weapon{};
    f32 kicked[2] = {0.0F, 0.0F};
    CyCamera released = 0;
    u32 calls = 0;

    CyResult configure(CyCamera /*camera*/, const CyFirstPersonDesc& desc) noexcept override {
        configured = desc;
        ++calls;
        return CY_RESULT_OK;
    }
    CyResult input(CyCamera camera, const CyFirstPersonInput& in) noexcept override {
        if (camera != kCamera) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no first-person camera");
        }
        last_input = in;
        ++calls;
        return CY_RESULT_OK;
    }
    CyResult kick(CyCamera /*camera*/, f32 pitch, f32 yaw) noexcept override {
        kicked[0] = pitch;
        kicked[1] = yaw;
        ++calls;
        return CY_RESULT_OK;
    }
    CyResult reset(CyCamera /*camera*/, f32 /*yaw*/, f32 /*pitch*/) noexcept override {
        ++calls;
        return CY_RESULT_OK;
    }
    CyResult state(CyCamera /*camera*/, CyFirstPersonState& out) const noexcept override {
        out.yaw = 0.5F;
        out.pitch = -0.25F;
        out.aim = 1.0F;
        out.fov = 0.96F;
        out.viewmodel_fov = 0.7F;
        out.weapon.position[2] = -0.3F;
        out.weapon_aim[2] = -1.0F;
        return CY_RESULT_OK;
    }
    CyResult release(CyCamera camera) noexcept override {
        released = camera;
        ++calls;
        return CY_RESULT_OK;
    }
    CyResult configure_viewmodel(CyCamera /*camera*/,
                                 const CyViewmodelDesc& desc) noexcept override {
        weapon = desc;
        ++calls;
        return CY_RESULT_OK;
    }
    CyResult viewmodel(CyCamera /*camera*/, CyViewmodelDesc& out) const noexcept override {
        out.layers = weapon.layers;
        out.fov = 0.7F;
        out.near_plane = weapon.near_plane;
        return CY_RESULT_OK;
    }
};

struct Bound {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakeFirstPerson backend;
    Bound() { host.game.first_person = &backend; }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

[[nodiscard]] CyFirstPersonDesc desc() noexcept {
    CyFirstPersonDesc made{};
    made.struct_size = sizeof(made);
    made.look_sensitivity = 0.0022F;
    made.aim_sensitivity = 0.7F;
    made.pitch_limit = 1.55F;
    made.hip_fov = 1.3F;
    made.aim_fov = 0.96F;
    made.hip_viewmodel_fov = 0.96F;
    made.aim_viewmodel_fov = 0.7F;
    made.aim_half_life = 0.06F;
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
    made.aim_offset[2] = -0.28F;
    return made;
}

[[nodiscard]] CyViewmodelDesc weapon_view() noexcept {
    CyViewmodelDesc made{};
    made.struct_size = sizeof(made);
    made.layers = 1U << 5U;
    made.near_plane = 0.01F;
    made.far_plane = 10.0F;
    made.depth_fraction = 0.0625F;
    return made;
}

}  // namespace

CY_TEST_CASE("first person: without a bound backend every entry is UNAVAILABLE") {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    const CyFirstPersonDesc made = desc();
    CY_CHECK_EQ(table().first_person_configure(&host, kCamera, &made), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().first_person_kick(&host, kCamera, 1.0F, 0.0F), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().first_person_release(&host, kCamera), CY_RESULT_UNAVAILABLE);
    const CyViewmodelDesc view = weapon_view();
    CY_CHECK_EQ(table().viewmodel_configure(&host, kCamera, &view), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().first_person_configure(nullptr, kCamera, &made),
                CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("first person: a description is whole and in range before the backend sees it") {
    Bound bound;
    const CyFirstPersonDesc good = desc();
    CY_REQUIRE_EQ(table().first_person_configure(bound.engine(), kCamera, &good), CY_RESULT_OK);
    CY_CHECK_EQ(bound.backend.configured.look_sensitivity, 0.0022F);
    CY_CHECK_EQ(bound.backend.configured.aim_offset[2], -0.28F);

    // An older module's shorter struct: what it did not know reads as zero.
    CyFirstPersonDesc shorter = desc();
    shorter.struct_size = offsetof(CyFirstPersonDesc, motion_reduction);
    shorter.motion_reduction = 1.0F;
    CY_REQUIRE_EQ(table().first_person_configure(bound.engine(), kCamera, &shorter), CY_RESULT_OK);
    CY_CHECK_EQ(bound.backend.configured.motion_reduction, 0.0F);
    CY_CHECK_EQ(bound.backend.configured.hip_offset[0], 0.0F);

    const u32 before = bound.backend.calls;
    const auto refused = [&](CyFirstPersonDesc bad) {
        CY_CHECK_EQ(table().first_person_configure(bound.engine(), kCamera, &bad),
                    CY_RESULT_INVALID_ARGUMENT);
    };
    CyFirstPersonDesc bad = desc();
    bad.flags = 0x4U;
    refused(bad);
    bad = desc();
    bad.hip_fov = 3.2F;  // past pi
    refused(bad);
    bad = desc();
    bad.pitch_limit = 1.7F;  // past the pole
    refused(bad);
    bad = desc();
    bad.recoil_frequency = 0.0F;
    refused(bad);
    bad = desc();
    bad.recoil_camera_share = 1.5F;
    refused(bad);
    bad = desc();
    bad.bob_stride = 0.0F;
    refused(bad);
    bad = desc();
    bad.look_sensitivity = std::numeric_limits<float>::quiet_NaN();
    refused(bad);
    bad = desc();
    bad.aim_offset[1] = std::numeric_limits<float>::infinity();
    refused(bad);
    bad = desc();
    bad.struct_size = 2;
    refused(bad);
    CY_CHECK_EQ(table().first_person_configure(bound.engine(), kCamera, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().first_person_configure(bound.engine(), 0U, &good),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(bound.backend.calls, before);
}

CY_TEST_CASE("first person: input, kicks and the weapon view are checked and passed whole") {
    Bound bound;
    CyFirstPersonInput input{};
    input.struct_size = sizeof(input);
    input.look[0] = 12.0F;
    input.move_speed = 3.0F;
    input.flags = CY_FIRST_PERSON_AIMING;
    CY_REQUIRE_EQ(table().first_person_input(bound.engine(), kCamera, &input), CY_RESULT_OK);
    CY_CHECK_EQ(bound.backend.last_input.look[0], 12.0F);
    CY_CHECK_EQ(bound.backend.last_input.flags, CY_FIRST_PERSON_AIMING);
    CY_CHECK_EQ(table().first_person_input(bound.engine(), 0x7U, &input), CY_RESULT_NOT_FOUND);
    CyFirstPersonInput bad = input;
    bad.move_speed = -1.0F;
    CY_CHECK_EQ(table().first_person_input(bound.engine(), kCamera, &bad),
                CY_RESULT_INVALID_ARGUMENT);
    bad = input;
    bad.reserved = 1;
    CY_CHECK_EQ(table().first_person_input(bound.engine(), kCamera, &bad),
                CY_RESULT_INVALID_ARGUMENT);
    bad = input;
    bad.flags = 0x10U;
    CY_CHECK_EQ(table().first_person_input(bound.engine(), kCamera, &bad),
                CY_RESULT_INVALID_ARGUMENT);

    CY_REQUIRE_EQ(table().first_person_kick(bound.engine(), kCamera, 3.0F, -0.5F), CY_RESULT_OK);
    CY_CHECK_EQ(bound.backend.kicked[0], 3.0F);
    CY_CHECK_EQ(bound.backend.kicked[1], -0.5F);
    CY_CHECK_EQ(table().first_person_kick(bound.engine(), kCamera,
                                          std::numeric_limits<float>::infinity(), 0.0F),
                CY_RESULT_INVALID_ARGUMENT);

    const CyViewmodelDesc view = weapon_view();
    CY_REQUIRE_EQ(table().viewmodel_configure(bound.engine(), kCamera, &view), CY_RESULT_OK);
    CY_CHECK_EQ(bound.backend.weapon.layers, view.layers);
    CyViewmodelDesc wrong = view;
    wrong.depth_fraction = 0.75F;
    CY_CHECK_EQ(table().viewmodel_configure(bound.engine(), kCamera, &wrong),
                CY_RESULT_INVALID_ARGUMENT);
    wrong = view;
    wrong.far_plane = wrong.near_plane;
    CY_CHECK_EQ(table().viewmodel_configure(bound.engine(), kCamera, &wrong),
                CY_RESULT_INVALID_ARGUMENT);
    wrong = view;
    wrong.flags = 0x8U;
    CY_CHECK_EQ(table().viewmodel_configure(bound.engine(), kCamera, &wrong),
                CY_RESULT_INVALID_ARGUMENT);
    // Off reads no lens: a zeroed description with no layers is the way to turn it off.
    CyViewmodelDesc off{};
    off.struct_size = sizeof(off);
    CY_CHECK_EQ(table().viewmodel_configure(bound.engine(), kCamera, &off), CY_RESULT_OK);

    CyViewmodelDesc read{};
    read.struct_size = sizeof(read);
    CY_REQUIRE_EQ(table().viewmodel_get(bound.engine(), kCamera, &read), CY_RESULT_OK);
    CY_CHECK_EQ(read.fov, 0.7F);
}

CY_TEST_CASE("first person: a frame update drives it; a fixed step may not touch it") {
    Bound bound;
    const CyFirstPersonDesc made = desc();
    {
        cy::abi::game::PhaseScope fixed(bound.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_CHECK_EQ(table().first_person_configure(bound.engine(), kCamera, &made),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(table().first_person_kick(bound.engine(), kCamera, 1.0F, 0.0F),
                    CY_RESULT_PERMISSION_DENIED);
        CyFirstPersonState state{};
        state.struct_size = sizeof(state);
        CY_CHECK_EQ(table().first_person_state(bound.engine(), kCamera, &state),
                    CY_RESULT_PERMISSION_DENIED);
    }
    CY_CHECK_EQ(bound.backend.calls, 0U);
    cy::abi::game::PhaseScope frame(bound.host.game.clock, CY_PHASE_FRAME_UPDATE);
    CY_REQUIRE_EQ(table().first_person_configure(bound.engine(), kCamera, &made), CY_RESULT_OK);
    CY_REQUIRE_EQ(table().first_person_reset(bound.engine(), kCamera, 0.1F, 0.0F), CY_RESULT_OK);
    // A state written into an older, shorter struct stops at its end.
    CyFirstPersonState state{};
    state.struct_size = offsetof(CyFirstPersonState, fov);
    CY_REQUIRE_EQ(table().first_person_state(bound.engine(), kCamera, &state), CY_RESULT_OK);
    CY_CHECK_EQ(state.yaw, 0.5F);
    CY_CHECK_EQ(state.aim, 1.0F);
    CY_CHECK_EQ(state.fov, 0.0F);
    CY_CHECK_EQ(state.struct_size, static_cast<u32>(offsetof(CyFirstPersonState, fov)));
    CY_CHECK_EQ(table().first_person_state(bound.engine(), kCamera, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_REQUIRE_EQ(table().first_person_release(bound.engine(), kCamera), CY_RESULT_OK);
    CY_CHECK_EQ(bound.backend.released, kCamera);
}
