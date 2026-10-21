// SPDX-License-Identifier: MIT
// The `fog_*` entries, ABI 1.10: fog of war.
//
// `fog_set_vision` is `[N F]`: what a team may see is simulation, and a frame update that changed
// it would let presentation decide what a player knows. The questions are `[N F U]`: the frame
// reads the grid to draw it. The backend is `cy::game_backend::FogAdapter`; see cy/abi/game/fog.h
// for what is checked here and what is left to it.

#include <cy/abi/errors.h>
#include <cy/abi/game/fog.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include "thunks.h"

namespace cy::abi::game {
namespace {

constexpr u32 kVisionPhases = kPhaseNone | kPhaseFixed;
constexpr u32 kMaxTeam = 32;

/// The bound backend, after the first three of services.h's steps; null with the refusal reported
/// otherwise.
[[nodiscard]] FogBackend* enter(CyEngine engine, u32 phases, const char* entry) noexcept {
    if (engine == nullptr) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    if (require_phase(engine->game, phases, entry) != CY_RESULT_OK) {
        return nullptr;
    }
    if (engine->game.fog == nullptr) {
        (void)report(CY_RESULT_UNAVAILABLE, "no fog of war is bound to this engine");
    }
    return engine->game.fog;
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

/// Ask the backend for one enumerator.
template <class Fn>
[[nodiscard]] CyResult answer_word(u32* out, const char* missing, Fn&& ask) noexcept {
    if (out == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, missing);
    }
    u32 value = 0;
    if (const CyResult result = ask(value); result != CY_RESULT_OK) {
        return result;
    }
    *out = value;
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace

CyResult fog_set_vision(CyEngine engine, CyEntity entity, const CyFogVision* vision) {
    FogBackend* fog = enter(engine, kVisionPhases, "fog_set_vision");
    if (fog == nullptr) {
        return last_error_code();
    }
    CyFogVision whole{};
    if (vision == nullptr || !read_sized(*vision, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "fog_set_vision needs a vision with a valid struct_size");
    }
    if (whole.team > kMaxTeam || whole.reveal_radius < 0 ||
        (whole.flags & ~CY_FOG_LEAVES_GHOST) != 0U || whole.reserved != 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "fog_set_vision: a team past 32, a negative reveal radius, an unknown flag "
                      "or a nonzero reserved word");
    }
    return finish(fog->set_vision(entity, whole));
}

CyResult fog_get_vision(CyEngine engine, CyEntity entity, CyFogVision* out_vision) {
    FogBackend* fog = enter(engine, kPhaseAny, "fog_get_vision");
    if (fog == nullptr) {
        return last_error_code();
    }
    return answer(out_vision, "fog_get_vision needs an output with a valid struct_size",
                  [fog, entity](CyFogVision& whole) { return fog->vision(entity, whole); });
}

CyResult fog_cell(CyEngine engine, uint32_t team, const CyFixedVec2* position, uint32_t* out_cell) {
    FogBackend* fog = enter(engine, kPhaseAny, "fog_cell");
    if (fog == nullptr) {
        return last_error_code();
    }
    if (position == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "fog_cell needs a position");
    }
    const CyFixedVec2 where = *position;
    return answer_word(out_cell, "fog_cell needs an output",
                       [fog, team, &where](u32& out) { return fog->cell(team, where, out); });
}

CyResult fog_sight(CyEngine engine, uint32_t team, CyEntity entity, uint32_t* out_sight) {
    FogBackend* fog = enter(engine, kPhaseAny, "fog_sight");
    if (fog == nullptr) {
        return last_error_code();
    }
    return answer_word(out_sight, "fog_sight needs an output",
                       [fog, team, entity](u32& out) { return fog->sight(team, entity, out); });
}

CyResult fog_status(CyEngine engine, CyFogStatus* out_status) {
    FogBackend* fog = enter(engine, kPhaseAny, "fog_status");
    if (fog == nullptr) {
        return last_error_code();
    }
    return answer(out_status, "fog_status needs an output with a valid struct_size",
                  [fog](CyFogStatus& whole) { return fog->status(whole); });
}

}  // namespace cy::abi::game
