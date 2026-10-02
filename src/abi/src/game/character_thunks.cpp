// SPDX-License-Identifier: MIT
// The character-controller thunks of ABI 1.5. `add-swift-m12-gaps`.
//
// `character_create` and `character_destroy` are `[N F]`: a character is simulation. A move is a
// fixed step and nothing else — `[F]` — and takes its delta from the clock, so a caller cannot step
// a character with a frame time and get stair behaviour that depends on the frame rate
// (cy/servers/physics/character.h says why that matters). The state is `[N F U]`.

#include <cy/abi/errors.h>
#include <cy/abi/game/character.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include <cmath>

#include "thunks.h"

namespace cy::abi::game {
namespace {

constexpr u32 kLifetimePhases = kPhaseNone | kPhaseFixed;
constexpr u32 kLayerCount = 32;

[[nodiscard]] CharacterBackend* enter(CyEngine engine, u32 phases, const char* entry) noexcept {
    if (engine == nullptr) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    if (require_phase(engine->game, phases, entry) != CY_RESULT_OK) {
        return nullptr;
    }
    if (engine->game.characters == nullptr) {
        (void)report(CY_RESULT_UNAVAILABLE, "no character backend is bound to this engine");
    }
    return engine->game.characters;
}

[[nodiscard]] bool finite_non_negative(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F;
}

/// Every number in a description is a size, a mass or an angle: finite and not negative.
[[nodiscard]] CyResult check_desc(const CyCharacterDesc& desc) noexcept {
    const float scalars[] = {desc.radius,      desc.height,     desc.max_slope_radians,
                             desc.step_offset, desc.skin_width, desc.gravity_scale,
                             desc.mass,        desc.push_force};
    for (const float value : scalars) {
        if (!finite_non_negative(value)) {
            return report(
                CY_RESULT_INVALID_ARGUMENT,
                "a character's sizes, slope, mass and forces are finite and not negative");
        }
    }
    for (const float value : desc.start.position) {
        if (!std::isfinite(value)) {
            return report(CY_RESULT_INVALID_ARGUMENT, "the start pose must be finite");
        }
    }
    for (const float value : desc.start.rotation) {
        if (!std::isfinite(value)) {
            return report(CY_RESULT_INVALID_ARGUMENT, "the start pose must be finite");
        }
    }
    if (desc.layer >= kLayerCount) {
        return report(CY_RESULT_INVALID_ARGUMENT, "the character's layer must be 0 to 31");
    }
    return CY_RESULT_OK;
}

[[nodiscard]] CyResult finish(CyResult result) noexcept {
    if (result == CY_RESULT_OK) {
        clear_last_error();
    }
    return result;
}

}  // namespace

CyResult character_create(CyEngine engine, CyEntity entity, const CyCharacterDesc* desc) {
    CharacterBackend* backend = enter(engine, kLifetimePhases, "character_create");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (entity == CY_ENTITY_NULL || desc == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "character_create needs an entity and a desc");
    }
    CyCharacterDesc whole{};
    if (!read_sized(*desc, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "the character desc has a malformed struct_size");
    }
    if (const CyResult checked = check_desc(whole); checked != CY_RESULT_OK) {
        return checked;
    }
    return finish(backend->create(entity, whole));
}

CyResult character_destroy(CyEngine engine, CyEntity entity) {
    CharacterBackend* backend = enter(engine, kLifetimePhases, "character_destroy");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (entity == CY_ENTITY_NULL) {
        return report(CY_RESULT_INVALID_ARGUMENT, "character_destroy needs an entity");
    }
    return finish(backend->destroy(entity));
}

CyResult character_move(CyEngine engine, CyEntity entity, const CyCharacterInput* input) {
    CharacterBackend* backend = enter(engine, kPhaseFixed, "character_move");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (entity == CY_ENTITY_NULL || input == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "character_move needs an entity and an input");
    }
    CyCharacterInput whole{};
    if (!read_sized(*input, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "the character input has a malformed struct_size");
    }
    const bool finite = std::isfinite(whole.desired_velocity[0]) &&
                        std::isfinite(whole.desired_velocity[1]) &&
                        std::isfinite(whole.desired_velocity[2]) && std::isfinite(whole.jump_speed);
    if (!finite) {
        return report(CY_RESULT_INVALID_ARGUMENT, "a character's input must be finite");
    }
    const auto delta = static_cast<f32>(engine->game.clock.fixed_delta);
    return finish(backend->move(entity, whole, delta));
}

CyResult character_state(CyEngine engine, CyEntity entity, CyCharacterState* out_state) {
    CharacterBackend* backend = enter(engine, kPhaseAny, "character_state");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (entity == CY_ENTITY_NULL || out_state == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "character_state needs an entity and an output");
    }
    if (agreed_size<CyCharacterState>(out_state->struct_size) == 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "the character state has a malformed struct_size");
    }
    CyCharacterState whole{};
    whole.struct_size = static_cast<u32>(sizeof(CyCharacterState));
    if (const CyResult result = backend->state(entity, whole); result != CY_RESULT_OK) {
        return result;
    }
    (void)write_sized(*out_state, whole);
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace cy::abi::game
