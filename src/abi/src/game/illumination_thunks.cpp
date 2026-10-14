// SPDX-License-Identifier: MIT
// The `gi_*` entries, ABI 1.9: global illumination.
//
// `[N U]`, every one: illumination is presentation, and a fixed step that tuned or read the light
// would make the simulation depend on it. The backend is `cy::game_backend::IlluminationAdapter`;
// see cy/abi/game/illumination.h for what is checked here and what is left to it.

#include <cy/abi/errors.h>
#include <cy/abi/game/illumination.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include <cmath>

#include "thunks.h"

namespace cy::abi::game {
namespace {

constexpr u32 kGiPhases = kPhaseNone | kPhaseFrame;
constexpr u32 kKnownVolumeBits = 0x7FU;

/// The bound backend, after the first three of services.h's steps; null with the refusal
/// reported otherwise.
[[nodiscard]] IlluminationBackend* enter(CyEngine engine, const char* entry) noexcept {
    if (engine == nullptr) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    if (require_phase(engine->game, kGiPhases, entry) != CY_RESULT_OK) {
        return nullptr;
    }
    if (engine->game.illumination == nullptr) {
        (void)report(CY_RESULT_UNAVAILABLE, "no global illumination is bound to this engine");
    }
    return engine->game.illumination;
}

[[nodiscard]] CyResult finish(CyResult result) noexcept {
    if (result == CY_RESULT_OK) {
        clear_last_error();
    }
    return result;
}

/// Ask the backend for a whole struct and write the caller's prefix of it.
template <class T, class Fn>
[[nodiscard]] CyResult answer(T* out, const char* malformed, Fn&& ask) noexcept {
    if (out == nullptr || agreed_size<T>(out->struct_size) == 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT, malformed);
    }
    T whole{};
    whole.struct_size = static_cast<u32>(sizeof(T));
    if (const CyResult result = ask(whole); result != CY_RESULT_OK) {
        return result;
    }
    (void)write_sized(*out, whole);
    clear_last_error();
    return CY_RESULT_OK;
}

[[nodiscard]] bool finite3(const f32* values) noexcept {
    return std::isfinite(values[0]) && std::isfinite(values[1]) && std::isfinite(values[2]);
}

[[nodiscard]] bool settings_valid(const CyGiSettings& settings) noexcept {
    return settings.mode <= CY_GI_MODE_HYBRID && settings.quality <= CY_GI_QUALITY_EPIC &&
           settings.tier <= CY_GI_TIER_WORLD_ONLY &&
           (settings.flags & ~CY_GI_HARDWARE_RAY_TRACING) == 0U &&
           std::isfinite(settings.budget_ms) && settings.budget_ms > 0.0F;
}

/// The enumerators and the mask; the numbers are the backend's to judge (gi::validate_gi_volume).
[[nodiscard]] bool volume_enums_valid(const CyGiVolumeDesc& desc) noexcept {
    return (desc.sets & ~kKnownVolumeBits) == 0U && desc.quality <= CY_GI_QUALITY_EPIC &&
           desc.tier <= CY_GI_TIER_WORLD_ONLY;
}

/// The volume a description names, read whole and checked; INVALID_ARGUMENT otherwise.
[[nodiscard]] CyResult read_volume(const CyGiVolumeDesc* desc, CyGiVolumeDesc& whole,
                                   const char* entry) noexcept {
    if (desc == nullptr || !read_sized(*desc, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT, entry);
    }
    if (!volume_enums_valid(whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "a GI volume names an unknown parameter, quality or tier");
    }
    return CY_RESULT_OK;
}

}  // namespace

CyResult gi_get_settings(CyEngine engine, CyGiSettings* out_settings) {
    IlluminationBackend* gi = enter(engine, "gi_get_settings");
    if (gi == nullptr) {
        return last_error_code();
    }
    return answer(out_settings, "gi_get_settings needs an output with a valid struct_size",
                  [gi](CyGiSettings& whole) { return gi->settings(whole); });
}

CyResult gi_set_settings(CyEngine engine, const CyGiSettings* settings) {
    IlluminationBackend* gi = enter(engine, "gi_set_settings");
    if (gi == nullptr) {
        return last_error_code();
    }
    CyGiSettings whole{};
    if (settings == nullptr || !read_sized(*settings, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "gi_set_settings needs settings with a valid struct_size");
    }
    // A short prefix reads its missing budget as zero, which no budget is, so it is refused here.
    if (!settings_valid(whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "gi_set_settings: a mode, quality, tier or flag out of range, or a budget "
                      "that is not a positive number of milliseconds");
    }
    return finish(gi->set_settings(whole));
}

CyResult gi_volume_create(CyEngine engine, const CyGiVolumeDesc* desc, CyGiVolume* out_volume) {
    IlluminationBackend* gi = enter(engine, "gi_volume_create");
    if (gi == nullptr) {
        return last_error_code();
    }
    if (out_volume == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "gi_volume_create needs an output");
    }
    CyGiVolumeDesc whole{};
    if (const CyResult read =
            read_volume(desc, whole, "gi_volume_create needs a desc with a valid struct_size");
        read != CY_RESULT_OK) {
        return read;
    }
    CyGiVolume made = CY_GI_VOLUME_NULL;
    if (const CyResult result = gi->create_volume(whole, made); result != CY_RESULT_OK) {
        return result;
    }
    *out_volume = made;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult gi_volume_update(CyEngine engine, CyGiVolume volume, const CyGiVolumeDesc* desc) {
    IlluminationBackend* gi = enter(engine, "gi_volume_update");
    if (gi == nullptr) {
        return last_error_code();
    }
    CyGiVolumeDesc whole{};
    if (const CyResult read =
            read_volume(desc, whole, "gi_volume_update needs a desc with a valid struct_size");
        read != CY_RESULT_OK) {
        return read;
    }
    return finish(gi->update_volume(volume, whole));
}

CyResult gi_volume_destroy(CyEngine engine, CyGiVolume volume) {
    IlluminationBackend* gi = enter(engine, "gi_volume_destroy");
    return gi == nullptr ? last_error_code() : finish(gi->destroy_volume(volume));
}

CyResult gi_object_set_class(CyEngine engine, CyEntity entity, uint32_t klass) {
    IlluminationBackend* gi = enter(engine, "gi_object_set_class");
    if (gi == nullptr) {
        return last_error_code();
    }
    if (klass > CY_GI_CLASS_AUTOMATIC) {
        return report(CY_RESULT_INVALID_ARGUMENT, "gi_object_set_class: an unknown CyGiClass");
    }
    return finish(gi->set_object_class(entity, klass));
}

CyResult gi_object_class(CyEngine engine, CyEntity entity, uint32_t* out_class) {
    IlluminationBackend* gi = enter(engine, "gi_object_class");
    if (gi == nullptr) {
        return last_error_code();
    }
    if (out_class == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "gi_object_class needs an output");
    }
    u32 klass = 0;
    if (const CyResult result = gi->object_class(entity, klass); result != CY_RESULT_OK) {
        return result;
    }
    *out_class = klass;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult gi_emission_set(CyEngine engine, CyEntity entity, uint64_t material, uint32_t mode) {
    IlluminationBackend* gi = enter(engine, "gi_emission_set");
    if (gi == nullptr) {
        return last_error_code();
    }
    if (mode > CY_GI_EMISSION_OFF) {
        return report(CY_RESULT_INVALID_ARGUMENT, "gi_emission_set: an unknown CyGiEmission");
    }
    return finish(gi->set_emission(entity, material, mode));
}

CyResult gi_sample_irradiance(CyEngine engine, const float* position_xyz, const float* normal_xyz,
                              CyGiIrradiance* out_irradiance) {
    IlluminationBackend* gi = enter(engine, "gi_sample_irradiance");
    if (gi == nullptr) {
        return last_error_code();
    }
    if (position_xyz == nullptr || !finite3(position_xyz) ||
        (normal_xyz != nullptr && !finite3(normal_xyz))) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "gi_sample_irradiance needs a finite position, and a finite normal or none");
    }
    const f32 position[3] = {position_xyz[0], position_xyz[1], position_xyz[2]};
    return answer(out_irradiance, "gi_sample_irradiance needs an output with a valid struct_size",
                  [gi, &position, normal_xyz](CyGiIrradiance& whole) {
                      return gi->sample_irradiance(position, normal_xyz, whole);
                  });
}

CyResult gi_status(CyEngine engine, CyGiStatus* out_status) {
    IlluminationBackend* gi = enter(engine, "gi_status");
    if (gi == nullptr) {
        return last_error_code();
    }
    return answer(out_status, "gi_status needs an output with a valid struct_size",
                  [gi](CyGiStatus& whole) { return gi->status(whole); });
}

}  // namespace cy::abi::game
