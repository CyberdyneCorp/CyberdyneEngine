// SPDX-License-Identifier: MIT
// The `effect_*` entries, ABI 1.15: effects a game plays while it runs.
//
// `effect_find` is `[N F U]`, so a fixed step may resolve the handle a frame update plays;
// `effect_play` is `[N U]`: an effect is presentation, and a fixed step that played a muzzle burst
// would play it again on every resimulation. The backend is `cy::game_backend::EffectsAdapter`; see
// cy/abi/game/effects.h for what is checked here and what is left to it.

#include <cy/abi/errors.h>
#include <cy/abi/game/effects.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include <cmath>

#include "thunks.h"

namespace cy::abi::game {
namespace {

constexpr u32 kPlayPhases = kPhaseNone | kPhaseFrame;
constexpr u32 kMaxParameters = 8;

/// The bound backend, after the first three of services.h's steps; null with the refusal reported
/// otherwise.
[[nodiscard]] EffectsBackend* enter(CyEngine engine, u32 phases, const char* entry) noexcept {
    if (engine == nullptr) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    if (require_phase(engine->game, phases, entry) != CY_RESULT_OK) {
        return nullptr;
    }
    if (engine->game.effects == nullptr) {
        (void)report(CY_RESULT_UNAVAILABLE, "no effects are bound to this engine");
    }
    return engine->game.effects;
}

[[nodiscard]] CyResult finish(CyResult result) noexcept {
    if (result == CY_RESULT_OK) {
        clear_last_error();
    }
    return result;
}

/// Whether a whole play is one the world can take: the count in range, the scale positive, every
/// number finite, the reserved words zero.
[[nodiscard]] bool well_formed(const CyEffectPlay& play) noexcept {
    bool finite = std::isfinite(play.scale) && std::isfinite(play.position[0]) &&
                  std::isfinite(play.position[1]) && std::isfinite(play.position[2]);
    for (const float value : play.parameters) {
        finite = finite && std::isfinite(value);
    }
    return finite && play.scale > 0.0F && play.parameter_count <= kMaxParameters &&
           play.reserved[0] == 0U && play.reserved[1] == 0U;
}

}  // namespace

CyResult effect_find(CyEngine engine, const char* name, CyEffect* out_effect) {
    EffectsBackend* effects = enter(engine, kPhaseAny, "effect_find");
    if (effects == nullptr) {
        return last_error_code();
    }
    if (name == nullptr || out_effect == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "effect_find needs a name and an output");
    }
    CyEffect found = CY_EFFECT_NULL;
    const CyResult result = effects->find(name, found);
    if (result == CY_RESULT_OK) {
        *out_effect = found;
    }
    return finish(result);
}

CyResult effect_play(CyEngine engine, CyEffect effect, const CyEffectPlay* play) {
    EffectsBackend* effects = enter(engine, kPlayPhases, "effect_play");
    if (effects == nullptr) {
        return last_error_code();
    }
    CyEffectPlay whole{};
    if (play == nullptr || !read_sized(*play, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "effect_play needs a play with a valid struct_size");
    }
    if (!well_formed(whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "an effect's parameter count is past eight, its scale is not positive, a "
                      "number is not finite, or a reserved word is not zero");
    }
    return finish(effects->play(effect, whole));
}

}  // namespace cy::abi::game
