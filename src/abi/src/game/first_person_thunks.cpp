// SPDX-License-Identifier: MIT
// The `first_person_*` and `viewmodel_*` entries, ABI 1.13: the first-person camera's behaviours
// and the weapon view drawn beside it.
//
// Every entry is `[N U]`: the camera is presentation and not authority, and a fixed step that
// kicked it would kick it again on every resimulation. The backend is
// `cy::game_backend::FirstPersonAdapter`; see cy/abi/game/first_person.h for what is checked here
// and what is left to it.

#include <cy/abi/errors.h>
#include <cy/abi/game/first_person.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include <cmath>
#include <numbers>

#include "thunks.h"

namespace cy::abi::game {
namespace {

constexpr u32 kFirstPersonPhases = kPhaseNone | kPhaseFrame;
constexpr f32 kPi = std::numbers::pi_v<f32>;

/// The bound backend for a non-null camera, after services.h's first three steps; null with the
/// refusal reported otherwise.
[[nodiscard]] FirstPersonBackend* enter(CyEngine engine, CyCamera camera,
                                        const char* entry) noexcept {
    if (engine == nullptr) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    if (require_phase(engine->game, kFirstPersonPhases, entry) != CY_RESULT_OK) {
        return nullptr;
    }
    if (engine->game.first_person == nullptr) {
        (void)report(CY_RESULT_UNAVAILABLE, "no first-person camera is bound to this engine");
        return nullptr;
    }
    if (camera == 0U) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, "the camera is null");
        return nullptr;
    }
    return engine->game.first_person;
}

[[nodiscard]] CyResult finish(CyResult result) noexcept {
    if (result == CY_RESULT_OK) {
        clear_last_error();
    }
    return result;
}

[[nodiscard]] bool finite(f32 value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] bool non_negative(f32 value) noexcept {
    return finite(value) && value >= 0.0F;
}

[[nodiscard]] bool positive(f32 value) noexcept {
    return finite(value) && value > 0.0F;
}

[[nodiscard]] bool unit(f32 value) noexcept {
    return finite(value) && value >= 0.0F && value <= 1.0F;
}

/// A vertical field of view a projection can be built from.
[[nodiscard]] bool lens(f32 value) noexcept {
    return finite(value) && value > 0.0F && value < kPi;
}

[[nodiscard]] bool finite3(const float (&values)[3]) noexcept {
    return finite(values[0]) && finite(values[1]) && finite(values[2]);
}

/// Every number of a description in its range and every flag known.
[[nodiscard]] bool well_formed(const CyFirstPersonDesc& desc) noexcept {
    const bool flags = (desc.flags & ~CY_FIRST_PERSON_INVERT_Y) == 0U;
    const bool look = non_negative(desc.look_sensitivity) && non_negative(desc.aim_sensitivity) &&
                      positive(desc.pitch_limit) && desc.pitch_limit <= kPi * 0.5F;
    const bool lenses = lens(desc.hip_fov) && lens(desc.aim_fov) && lens(desc.hip_viewmodel_fov) &&
                        lens(desc.aim_viewmodel_fov) && non_negative(desc.aim_half_life);
    const bool bob = positive(desc.bob_stride) && non_negative(desc.bob_vertical) &&
                     non_negative(desc.bob_lateral) && positive(desc.bob_reference_speed);
    const bool recoil = positive(desc.recoil_frequency) && non_negative(desc.recoil_damping) &&
                        unit(desc.recoil_camera_share) && unit(desc.motion_reduction);
    return flags && look && lenses && bob && recoil && finite3(desc.hip_offset) &&
           finite3(desc.aim_offset);
}

[[nodiscard]] bool well_formed(const CyFirstPersonInput& input) noexcept {
    const bool flags = (input.flags & ~(CY_FIRST_PERSON_AIMING | CY_FIRST_PERSON_AIRBORNE)) == 0U;
    return flags && input.reserved == 0U && finite(input.look[0]) && finite(input.look[1]) &&
           non_negative(input.move_speed);
}

[[nodiscard]] bool well_formed(const CyViewmodelDesc& desc) noexcept {
    constexpr u32 kKnown =
        CY_VIEWMODEL_IGNORE_SHADOWS | CY_VIEWMODEL_CAST_SHADOWS | CY_VIEWMODEL_CONTACT_SHADOWS;
    if ((desc.flags & ~kKnown) != 0U || desc.reserved != 0U) {
        return false;
    }
    if (desc.layers == 0U) {
        return true;  // off: the lens is not read
    }
    const bool fov = desc.fov == 0.0F || lens(desc.fov);
    return fov && positive(desc.near_plane) && finite(desc.far_plane) &&
           desc.far_plane > desc.near_plane && positive(desc.depth_fraction) &&
           desc.depth_fraction <= 0.5F;
}

}  // namespace

CyResult first_person_configure(CyEngine engine, CyCamera camera, const CyFirstPersonDesc* desc) {
    FirstPersonBackend* backend = enter(engine, camera, "first_person_configure");
    if (backend == nullptr) {
        return last_error_code();
    }
    CyFirstPersonDesc whole{};
    if (desc == nullptr || !read_sized(*desc, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "first_person_configure needs a description with a valid struct_size");
    }
    if (!well_formed(whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "a first-person number is out of its range or not finite, or a flag is "
                      "unknown");
    }
    return finish(backend->configure(camera, whole));
}

CyResult first_person_input(CyEngine engine, CyCamera camera, const CyFirstPersonInput* input) {
    FirstPersonBackend* backend = enter(engine, camera, "first_person_input");
    if (backend == nullptr) {
        return last_error_code();
    }
    CyFirstPersonInput whole{};
    if (input == nullptr || !read_sized(*input, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "first_person_input needs an input with a valid struct_size");
    }
    if (!well_formed(whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "a first-person input is not finite, has a negative speed, an unknown flag "
                      "or a nonzero reserved word");
    }
    return finish(backend->input(camera, whole));
}

CyResult first_person_kick(CyEngine engine, CyCamera camera, float pitch, float yaw) {
    FirstPersonBackend* backend = enter(engine, camera, "first_person_kick");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (!finite(pitch) || !finite(yaw)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "a recoil kick is not finite");
    }
    return finish(backend->kick(camera, pitch, yaw));
}

CyResult first_person_reset(CyEngine engine, CyCamera camera, float yaw, float pitch) {
    FirstPersonBackend* backend = enter(engine, camera, "first_person_reset");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (!finite(yaw) || !finite(pitch)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "a first-person look is not finite");
    }
    return finish(backend->reset(camera, yaw, pitch));
}

CyResult first_person_state(CyEngine engine, CyCamera camera, CyFirstPersonState* out_state) {
    FirstPersonBackend* backend = enter(engine, camera, "first_person_state");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (out_state == nullptr || agreed_size<CyFirstPersonState>(out_state->struct_size) == 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "first_person_state needs an output with a valid struct_size");
    }
    CyFirstPersonState whole{};
    whole.struct_size = static_cast<u32>(sizeof(CyFirstPersonState));
    if (const CyResult result = backend->state(camera, whole); result != CY_RESULT_OK) {
        return result;
    }
    (void)write_sized(*out_state, whole);
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult first_person_release(CyEngine engine, CyCamera camera) {
    FirstPersonBackend* backend = enter(engine, camera, "first_person_release");
    if (backend == nullptr) {
        return last_error_code();
    }
    return finish(backend->release(camera));
}

CyResult viewmodel_configure(CyEngine engine, CyCamera camera, const CyViewmodelDesc* desc) {
    FirstPersonBackend* backend = enter(engine, camera, "viewmodel_configure");
    if (backend == nullptr) {
        return last_error_code();
    }
    CyViewmodelDesc whole{};
    if (desc == nullptr || !read_sized(*desc, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "viewmodel_configure needs a description with a valid struct_size");
    }
    if (!well_formed(whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "a weapon view's lens is out of range, a flag is unknown or a reserved word "
                      "is not zero");
    }
    return finish(backend->configure_viewmodel(camera, whole));
}

CyResult viewmodel_get(CyEngine engine, CyCamera camera, CyViewmodelDesc* out_desc) {
    FirstPersonBackend* backend = enter(engine, camera, "viewmodel_get");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (out_desc == nullptr || agreed_size<CyViewmodelDesc>(out_desc->struct_size) == 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "viewmodel_get needs an output with a valid struct_size");
    }
    CyViewmodelDesc whole{};
    whole.struct_size = static_cast<u32>(sizeof(CyViewmodelDesc));
    if (const CyResult result = backend->viewmodel(camera, whole); result != CY_RESULT_OK) {
        return result;
    }
    (void)write_sized(*out_desc, whole);
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace cy::abi::game
