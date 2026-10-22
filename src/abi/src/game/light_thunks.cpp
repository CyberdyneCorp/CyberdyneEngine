// SPDX-License-Identifier: MIT
// The `light_*` entries, ABI 1.11: lights a game makes while it runs.
//
// Every entry is `[N U]`: a light is presentation, and a fixed step that spawned a muzzle flash
// would spawn it again on every resimulation. The backend is `cy::game_backend::LightsAdapter`; see
// cy/abi/game/lights.h for what is checked here and what is left to it.

#include <cy/abi/errors.h>
#include <cy/abi/game/lights.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include <cmath>

#include "thunks.h"

namespace cy::abi::game {
namespace {

constexpr u32 kLightPhases = kPhaseNone | kPhaseFrame;

/// The bound backend, after the first three of services.h's steps; null with the refusal reported
/// otherwise.
[[nodiscard]] LightsBackend* enter(CyEngine engine, const char* entry) noexcept {
    if (engine == nullptr) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    if (require_phase(engine->game, kLightPhases, entry) != CY_RESULT_OK) {
        return nullptr;
    }
    if (engine->game.lights == nullptr) {
        (void)report(CY_RESULT_UNAVAILABLE, "no light pool is bound to this engine");
    }
    return engine->game.lights;
}

[[nodiscard]] CyResult finish(CyResult result) noexcept {
    if (result == CY_RESULT_OK) {
        clear_last_error();
    }
    return result;
}

[[nodiscard]] bool non_negative(f32 value) noexcept {
    return std::isfinite(value) && value >= 0.0F;
}

/// Whether a whole description is one the pool can hold: every enumerator and flag known, every
/// number finite and none negative, the reserved words zero.
[[nodiscard]] bool well_formed(const CyLightDesc& desc) noexcept {
    const bool kind = desc.kind >= CY_LIGHT_POINT && desc.kind <= CY_LIGHT_TUBE;
    const bool fade = desc.fade <= CY_LIGHT_FADE_QUADRATIC;
    const bool flags = (desc.flags & ~CY_LIGHT_TWO_SIDED) == 0U;
    const bool reserved = desc.reserved[0] == 0U && desc.reserved[1] == 0U;
    const bool numbers = non_negative(desc.intensity) && non_negative(desc.range) &&
                         non_negative(desc.size[0]) && non_negative(desc.size[1]) &&
                         non_negative(desc.lifetime) && non_negative(desc.inner_cone) &&
                         non_negative(desc.outer_cone);
    return kind && fade && flags && reserved && numbers;
}

/// Read a caller's description whole, or report why not.
[[nodiscard]] bool read_desc(const CyLightDesc* desc, CyLightDesc& whole,
                             const char* entry) noexcept {
    if (desc == nullptr || !read_sized(*desc, whole)) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, entry);
        return false;
    }
    if (!well_formed(whole)) {
        (void)report(CY_RESULT_INVALID_ARGUMENT,
                     "a light's kind, fade or flag is out of range, a number is negative or not "
                     "finite, or a reserved word is not zero");
        return false;
    }
    return true;
}

}  // namespace

CyResult light_spawn(CyEngine engine, const CyLightDesc* desc, CyLight* out_light) {
    LightsBackend* lights = enter(engine, "light_spawn");
    if (lights == nullptr) {
        return last_error_code();
    }
    if (out_light == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "light_spawn needs an output");
    }
    CyLightDesc whole{};
    if (!read_desc(desc, whole, "light_spawn needs a description with a valid struct_size")) {
        return last_error_code();
    }
    CyLight made = CY_LIGHT_NULL;
    const CyResult result = lights->spawn(whole, made);
    if (result == CY_RESULT_OK) {
        *out_light = made;
    }
    return finish(result);
}

CyResult light_update(CyEngine engine, CyLight light, const CyLightDesc* desc) {
    LightsBackend* lights = enter(engine, "light_update");
    if (lights == nullptr) {
        return last_error_code();
    }
    CyLightDesc whole{};
    if (!read_desc(desc, whole, "light_update needs a description with a valid struct_size")) {
        return last_error_code();
    }
    return finish(lights->update(light, whole));
}

CyResult light_destroy(CyEngine engine, CyLight light) {
    LightsBackend* lights = enter(engine, "light_destroy");
    if (lights == nullptr) {
        return last_error_code();
    }
    return finish(lights->destroy(light));
}

CyResult light_alive(CyEngine engine, CyLight light, uint32_t* out_alive) {
    LightsBackend* lights = enter(engine, "light_alive");
    if (lights == nullptr) {
        return last_error_code();
    }
    if (out_alive == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "light_alive needs an output");
    }
    u32 alive = 0;
    const CyResult result = lights->alive(light, alive);
    if (result == CY_RESULT_OK) {
        *out_alive = alive;
    }
    return finish(result);
}

CyResult light_status(CyEngine engine, CyLightStatus* out_status) {
    LightsBackend* lights = enter(engine, "light_status");
    if (lights == nullptr) {
        return last_error_code();
    }
    if (out_status == nullptr || agreed_size<CyLightStatus>(out_status->struct_size) == 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "light_status needs an output with a valid struct_size");
    }
    CyLightStatus whole{};
    whole.struct_size = static_cast<u32>(sizeof(CyLightStatus));
    if (const CyResult result = lights->status(whole); result != CY_RESULT_OK) {
        return result;
    }
    (void)write_sized(*out_status, whole);
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace cy::abi::game
