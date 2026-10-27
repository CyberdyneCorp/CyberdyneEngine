// SPDX-License-Identifier: MIT
// The `decal_*` entries, ABI 1.15: decals a game spawns while it runs.
//
// `decal_find_material` is `[N F U]`, so a fixed step may resolve the handle a frame update spawns
// with; `decal_spawn` and `decal_status` are `[N U]`: a decal is presentation, and a fixed step
// that left a mark would leave it again on every resimulation. The backend is
// `cy::game_backend::DecalsAdapter`; see cy/abi/game/decals.h for what is checked here and what is
// left to it.

#include <cy/abi/errors.h>
#include <cy/abi/game/decals.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include <cmath>
#include <numbers>

#include "thunks.h"

namespace cy::abi::game {
namespace {

constexpr u32 kSpawnPhases = kPhaseNone | kPhaseFrame;

/// The bound backend, after the first three of services.h's steps; null with the refusal reported
/// otherwise.
[[nodiscard]] DecalsBackend* enter(CyEngine engine, u32 phases, const char* entry) noexcept {
    if (engine == nullptr) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    if (require_phase(engine->game, phases, entry) != CY_RESULT_OK) {
        return nullptr;
    }
    if (engine->game.decals == nullptr) {
        (void)report(CY_RESULT_UNAVAILABLE, "no decal budget is bound to this engine");
    }
    return engine->game.decals;
}

[[nodiscard]] CyResult finish(CyResult result) noexcept {
    if (result == CY_RESULT_OK) {
        clear_last_error();
    }
    return result;
}

[[nodiscard]] bool all_finite(const float* values, usize count) noexcept {
    for (usize index = 0; index < count; ++index) {
        if (!std::isfinite(values[index])) {
            return false;
        }
    }
    return true;
}

/// Whether a whole description is one the budget can hold: every extent positive, the importance
/// and the fade angle in range, the fade's end past its start (once the zero defaults are taken),
/// every number finite and the reserved words zero.
[[nodiscard]] bool well_formed(const CyDecalDesc& desc) noexcept {
    const float scalars[] = {desc.importance, desc.fade_angle, desc.fade_start, desc.fade_end};
    if (!all_finite(desc.pose.position, 3) || !all_finite(desc.pose.rotation, 4) ||
        !all_finite(desc.half_extent, 3) || !all_finite(scalars, 4)) {
        return false;
    }
    const bool extents =
        desc.half_extent[0] > 0.0F && desc.half_extent[1] > 0.0F && desc.half_extent[2] > 0.0F;
    const bool importance = desc.importance >= 0.0F && desc.importance <= 1.0F;
    const bool angle =
        desc.fade_angle >= 0.0F && desc.fade_angle <= std::numbers::pi_v<float> * 0.5F;
    const float start = desc.fade_start == 0.0F ? 20.0F : desc.fade_start;
    const float end = desc.fade_end == 0.0F ? 40.0F : desc.fade_end;
    const bool fade = desc.fade_start >= 0.0F && end > start;
    const bool reserved = desc.reserved0 == 0U && desc.reserved[0] == 0U && desc.reserved[1] == 0U;
    return extents && importance && angle && fade && reserved;
}

}  // namespace

CyResult decal_find_material(CyEngine engine, const char* name, CyDecalMaterial* out_material) {
    DecalsBackend* decals = enter(engine, kPhaseAny, "decal_find_material");
    if (decals == nullptr) {
        return last_error_code();
    }
    if (name == nullptr || out_material == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "decal_find_material needs a name and an output");
    }
    CyDecalMaterial found = CY_DECAL_MATERIAL_NULL;
    const CyResult result = decals->find_material(name, found);
    if (result == CY_RESULT_OK) {
        *out_material = found;
    }
    return finish(result);
}

CyResult decal_spawn(CyEngine engine, const CyDecalDesc* desc, CyDecal* out_decal) {
    DecalsBackend* decals = enter(engine, kSpawnPhases, "decal_spawn");
    if (decals == nullptr) {
        return last_error_code();
    }
    if (out_decal == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "decal_spawn needs an output");
    }
    CyDecalDesc whole{};
    if (desc == nullptr || !read_sized(*desc, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "decal_spawn needs a description with a valid struct_size");
    }
    if (!well_formed(whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "a decal's extent is not positive, its importance or fade angle is out of "
                      "range, its fade ends before it starts, a number is not finite, or a "
                      "reserved word is not zero");
    }
    CyDecal made = CY_DECAL_NULL;
    const CyResult result = decals->spawn(whole, made);
    if (result == CY_RESULT_OK) {
        *out_decal = made;
    }
    return finish(result);
}

CyResult decal_status(CyEngine engine, CyDecalStatus* out_status) {
    DecalsBackend* decals = enter(engine, kSpawnPhases, "decal_status");
    if (decals == nullptr) {
        return last_error_code();
    }
    if (out_status == nullptr || agreed_size<CyDecalStatus>(out_status->struct_size) == 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "decal_status needs an output with a valid struct_size");
    }
    CyDecalStatus whole{};
    whole.struct_size = static_cast<u32>(sizeof(CyDecalStatus));
    if (const CyResult result = decals->status(whole); result != CY_RESULT_OK) {
        return result;
    }
    (void)write_sized(*out_status, whole);
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace cy::abi::game
