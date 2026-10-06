// SPDX-License-Identifier: MIT
// The animation thunks of ABI 1.7. Issue #76 stage 4.
//
// What a character DOES is simulation: attaching, playing, stopping, raising a parameter and
// choosing where root motion goes are `[N F]`, and taking root motion is a fixed step's alone. What
// it LOOKS like is read anywhere (`[N F U]`), except the two answers that are presentation — the
// frame's events and a joint's evaluated pose — which are `[N U]`.

#include <cy/abi/errors.h>
#include <cy/abi/game/animation.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include <cmath>

#include "thunks.h"

namespace cy::abi::game {
namespace {

constexpr u32 kSimulationPhases = kPhaseNone | kPhaseFixed;
constexpr u32 kPresentationPhases = kPhaseNone | kPhaseFrame;
constexpr u32 kTierCount = 4;

[[nodiscard]] AnimationBackend* enter(CyEngine engine, u32 phases, const char* entry) noexcept {
    if (engine == nullptr) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    if (require_phase(engine->game, phases, entry) != CY_RESULT_OK) {
        return nullptr;
    }
    if (engine->game.animation == nullptr) {
        (void)report(CY_RESULT_UNAVAILABLE, "no animation backend is bound to this engine");
    }
    return engine->game.animation;
}

[[nodiscard]] CyResult finish(CyResult result) noexcept {
    if (result == CY_RESULT_OK) {
        clear_last_error();
    }
    return result;
}

/// A duration a blend can last: finite and not negative.
[[nodiscard]] bool duration(float seconds) noexcept {
    return std::isfinite(seconds) && seconds >= 0.0F;
}

[[nodiscard]] bool known_mode(u32 mode) noexcept {
    return mode <= static_cast<u32>(CY_ROOT_MOTION_CHARACTER);
}

/// Every entry that names an entity and a string: both present.
[[nodiscard]] CyResult need(CyEntity entity, const void* pointer, const char* sentence) noexcept {
    if (entity == CY_ENTITY_NULL || pointer == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, sentence);
    }
    return CY_RESULT_OK;
}

/// Write a whole struct into the caller's prefix.
template <class T>
[[nodiscard]] CyResult hand_back(T& out, const T& whole) noexcept {
    (void)write_sized(out, whole);
    clear_last_error();
    return CY_RESULT_OK;
}

/// Read a root-motion answer from the backend into the caller's struct.
template <class Fn>
[[nodiscard]] CyResult answer_motion(CyEntity entity, CyRootMotion* out, Fn&& ask) noexcept {
    if (const CyResult checked = need(entity, out,
                                      "an animation root-motion read needs an entity "
                                      "and an output");
        checked != CY_RESULT_OK) {
        return checked;
    }
    if (agreed_size<CyRootMotion>(out->struct_size) == 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT, "the root motion has a malformed struct_size");
    }
    CyRootMotion whole{};
    whole.struct_size = static_cast<u32>(sizeof(CyRootMotion));
    if (const CyResult result = ask(whole); result != CY_RESULT_OK) {
        return result;
    }
    return hand_back(*out, whole);
}

}  // namespace

CyResult animation_attach(CyEngine engine, CyEntity entity, const CyAnimatorDesc* desc) {
    AnimationBackend* backend = enter(engine, kSimulationPhases, "animation_attach");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (const CyResult checked = need(entity, desc, "animation_attach needs an entity and a desc");
        checked != CY_RESULT_OK) {
        return checked;
    }
    CyAnimatorDesc whole{};
    if (!read_sized(*desc, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "the animator desc has a malformed struct_size");
    }
    if (whole.rig == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "an animator names the rig it plays");
    }
    if (whole.tier >= kTierCount || !known_mode(whole.root_motion)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "an animator's tier and root-motion mode are CyAnimationTier and "
                      "CyRootMotionMode values");
    }
    if (!std::isfinite(whole.play_rate) || whole.play_rate < 0.0F) {
        return report(CY_RESULT_INVALID_ARGUMENT, "a play rate is finite and not negative");
    }
    const CyResult result = backend->attach(entity, whole);
    // STRUCTURAL: the entity gained a component, so every borrow taken before reads as stale.
    if (result == CY_RESULT_OK && engine->world != nullptr) {
        engine->world->bump_epoch();
    }
    return finish(result);
}

CyResult animation_detach(CyEngine engine, CyEntity entity) {
    AnimationBackend* backend = enter(engine, kSimulationPhases, "animation_detach");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (entity == CY_ENTITY_NULL) {
        return report(CY_RESULT_INVALID_ARGUMENT, "animation_detach needs an entity");
    }
    const CyResult result = backend->detach(entity);
    if (result == CY_RESULT_OK && engine->world != nullptr) {
        engine->world->bump_epoch();
    }
    return finish(result);
}

CyResult animation_play(CyEngine engine, CyEntity entity, const char* state,
                        float crossfade_seconds) {
    AnimationBackend* backend = enter(engine, kSimulationPhases, "animation_play");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (const CyResult checked = need(entity, state, "animation_play needs an entity and a state");
        checked != CY_RESULT_OK) {
        return checked;
    }
    if (!duration(crossfade_seconds)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "a crossfade lasts a finite, non-negative time");
    }
    return finish(backend->play(entity, state, crossfade_seconds));
}

CyResult animation_stop(CyEngine engine, CyEntity entity, float blend_seconds) {
    AnimationBackend* backend = enter(engine, kSimulationPhases, "animation_stop");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (entity == CY_ENTITY_NULL) {
        return report(CY_RESULT_INVALID_ARGUMENT, "animation_stop needs an entity");
    }
    if (!duration(blend_seconds)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "a blend lasts a finite, non-negative time");
    }
    return finish(backend->stop(entity, blend_seconds));
}

CyResult animation_set_float(CyEngine engine, CyEntity entity, const char* parameter, float value) {
    AnimationBackend* backend = enter(engine, kSimulationPhases, "animation_set_float");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (const CyResult checked =
            need(entity, parameter, "animation_set_float needs an entity and a parameter");
        checked != CY_RESULT_OK) {
        return checked;
    }
    if (!std::isfinite(value)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "a parameter's value must be finite");
    }
    return finish(backend->set_parameter(entity, parameter, value));
}

CyResult animation_set_bool(CyEngine engine, CyEntity entity, const char* parameter, bool value) {
    AnimationBackend* backend = enter(engine, kSimulationPhases, "animation_set_bool");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (const CyResult checked =
            need(entity, parameter, "animation_set_bool needs an entity and a parameter");
        checked != CY_RESULT_OK) {
        return checked;
    }
    return finish(backend->set_parameter(entity, parameter, value ? 1.0F : 0.0F));
}

CyResult animation_fire_trigger(CyEngine engine, CyEntity entity, const char* parameter) {
    AnimationBackend* backend = enter(engine, kSimulationPhases, "animation_fire_trigger");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (const CyResult checked =
            need(entity, parameter, "animation_fire_trigger needs an entity and a parameter");
        checked != CY_RESULT_OK) {
        return checked;
    }
    return finish(backend->fire_trigger(entity, parameter));
}

CyResult animation_get_float(CyEngine engine, CyEntity entity, const char* parameter,
                             float* out_value) {
    AnimationBackend* backend = enter(engine, kPhaseAny, "animation_get_float");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (const CyResult checked =
            need(entity, parameter, "animation_get_float needs an entity and a parameter");
        checked != CY_RESULT_OK) {
        return checked;
    }
    if (out_value == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "animation_get_float needs an output");
    }
    f32 value = 0.0F;
    if (const CyResult result = backend->parameter(entity, parameter, value);
        result != CY_RESULT_OK) {
        return result;
    }
    *out_value = value;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult animation_state(CyEngine engine, CyEntity entity, CyAnimatorState* out_state) {
    AnimationBackend* backend = enter(engine, kPhaseAny, "animation_state");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (const CyResult checked =
            need(entity, out_state, "animation_state needs an entity and an output");
        checked != CY_RESULT_OK) {
        return checked;
    }
    if (agreed_size<CyAnimatorState>(out_state->struct_size) == 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT, "the animator state has a malformed struct_size");
    }
    CyAnimatorState whole{};
    whole.struct_size = static_cast<u32>(sizeof(CyAnimatorState));
    if (const CyResult result = backend->state(entity, whole); result != CY_RESULT_OK) {
        return result;
    }
    return hand_back(*out_state, whole);
}

CyResult animation_events(CyEngine engine, CyAnimationEvent* out_events, uint32_t capacity,
                          uint32_t* out_count) {
    AnimationBackend* backend = enter(engine, kPresentationPhases, "animation_events");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (out_count == nullptr || (out_events == nullptr && capacity != 0U)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "animation_events needs a count, and a buffer for any capacity");
    }
    const Span<const CyAnimationEvent> events = backend->events();
    *out_count = static_cast<u32>(events.size());
    // THE SIZING PATTERN: the count is always written, and nothing else is until it fits. A null
    // buffer with no capacity is the question "how many", and answering it is not a failure.
    const bool asking = out_events == nullptr && capacity == 0U;
    if (events.size() > capacity && !asking) {
        return report(CY_RESULT_BUFFER_TOO_SMALL,
                      "the buffer holds fewer events than this frame has; the count says how many");
    }
    if (asking) {
        clear_last_error();
        return CY_RESULT_OK;
    }
    for (usize index = 0; index < events.size(); ++index) {
        out_events[index] = events[index];
    }
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult animation_root_motion(CyEngine engine, CyEntity entity, CyRootMotion* out_motion) {
    AnimationBackend* backend = enter(engine, kPhaseAny, "animation_root_motion");
    if (backend == nullptr) {
        return last_error_code();
    }
    return answer_motion(entity, out_motion, [backend, entity](CyRootMotion& whole) noexcept {
        return backend->root_motion(entity, whole);
    });
}

CyResult animation_take_root_motion(CyEngine engine, CyEntity entity, CyRootMotion* out_motion) {
    AnimationBackend* backend = enter(engine, kPhaseFixed, "animation_take_root_motion");
    if (backend == nullptr) {
        return last_error_code();
    }
    return answer_motion(entity, out_motion, [backend, entity](CyRootMotion& whole) noexcept {
        return backend->take_root_motion(entity, whole);
    });
}

CyResult animation_set_root_motion(CyEngine engine, CyEntity entity, uint32_t mode) {
    AnimationBackend* backend = enter(engine, kSimulationPhases, "animation_set_root_motion");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (entity == CY_ENTITY_NULL || !known_mode(mode)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "animation_set_root_motion needs an entity and a CyRootMotionMode");
    }
    return finish(backend->set_root_motion(entity, static_cast<CyRootMotionMode>(mode)));
}

CyResult animation_joint_pose(CyEngine engine, CyEntity entity, const char* joint,
                              CyPose* out_pose) {
    AnimationBackend* backend = enter(engine, kPresentationPhases, "animation_joint_pose");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (const CyResult checked =
            need(entity, joint, "animation_joint_pose needs an entity and a joint");
        checked != CY_RESULT_OK) {
        return checked;
    }
    if (out_pose == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "animation_joint_pose needs an output");
    }
    CyPose pose{};
    if (const CyResult result = backend->joint_pose(entity, joint, pose); result != CY_RESULT_OK) {
        return result;
    }
    *out_pose = pose;
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace cy::abi::game
