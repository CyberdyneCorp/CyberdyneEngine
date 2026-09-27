// SPDX-License-Identifier: MIT
// The `audio` thunks of ABI 1.3. `add-swift-game-api`.
//
// OWNER: implementer C (audio, spawning and time). See
// openspec/changes/add-swift-game-api/design.md.
//
// Each thunk takes cy/abi/game/services.h's steps in order — engine, phase, backend, pointers,
// `struct_size`, the resimulation drop — and only then asks the `AudioBackend`. What reaches the
// backend is whole and finite: a non-null name, a `CyAudioPlay` of this build's size, gains and
// fades that are numbers and not negative.
//
// PHASES. Audio is presentation — nothing in the simulation reads it back — so every entry is
// `[N F U]`. The price is the resimulation rule: while CY_TIME_RESIMULATING is set, the three
// writes (`play`, `stop`, `set_bus_volume`) answer OK and do nothing, so a rolled-back tick does
// not play its sounds twice. The reads still answer, because they change nothing.

#include <cy/abi/errors.h>
#include <cy/abi/game/audio.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include <cmath>

#include "thunks.h"

namespace cy::abi::game {
namespace {

/// Steps 1 to 3: the engine, the phase, the backend. The bound backend, or null having reported
/// why — which the caller returns as `last_error_code()`.
[[nodiscard]] AudioBackend* enter(CyEngine engine, const char* entry) noexcept {
    if (engine == nullptr) {
        (void)report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    if (require_phase(engine->game, kPhaseAny, entry) != CY_RESULT_OK) {
        return nullptr;
    }
    if (engine->game.audio == nullptr) {
        (void)report(CY_RESULT_UNAVAILABLE, "no audio backend is bound to this engine");
    }
    return engine->game.audio;
}

/// A duration or a gain: a finite number that is not negative.
[[nodiscard]] bool non_negative(f32 value) noexcept {
    return std::isfinite(value) && value >= 0.0F;
}

[[nodiscard]] bool finite3(const float (&values)[3]) noexcept {
    return std::isfinite(values[0]) && std::isfinite(values[1]) && std::isfinite(values[2]);
}

[[nodiscard]] CyResult finish(CyResult result) noexcept {
    if (result == CY_RESULT_OK) {
        clear_last_error();
    }
    return result;
}

}  // namespace

CyResult audio_find_cue(CyEngine engine, const char* name, CyAudioCue* out_cue) {
    AudioBackend* backend = enter(engine, "audio_find_cue");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (name == nullptr || out_cue == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "audio_find_cue needs a name and an output");
    }
    CyAudioCue cue = 0;
    if (const CyResult result = backend->find_cue(name, cue); result != CY_RESULT_OK) {
        return result;
    }
    *out_cue = cue;
    return finish(CY_RESULT_OK);
}

CyResult audio_play(CyEngine engine, const CyAudioPlay* play, CyAudioVoice* out_voice) {
    AudioBackend* backend = enter(engine, "audio_play");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (play == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "audio_play needs a CyAudioPlay");
    }
    CyAudioPlay whole{};
    if (!read_sized(*play, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "audio_play: the CyAudioPlay struct_size is malformed");
    }
    if (!non_negative(whole.volume) || !non_negative(whole.pitch) ||
        !non_negative(whole.fade_in_seconds) || !finite3(whole.position)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "audio_play: volume, pitch and fade must be finite and not negative, and "
                      "the position finite");
    }
    if (engine->game.clock.resimulating()) {
        if (out_voice != nullptr) {
            *out_voice = 0;
        }
        return finish(CY_RESULT_OK);
    }
    CyAudioVoice voice = 0;
    if (const CyResult result = backend->play(whole, voice); result != CY_RESULT_OK) {
        return result;
    }
    if (out_voice != nullptr) {
        *out_voice = voice;
    }
    return finish(CY_RESULT_OK);
}

CyResult audio_stop(CyEngine engine, CyAudioVoice voice, float fade_out_seconds) {
    AudioBackend* backend = enter(engine, "audio_stop");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (!non_negative(fade_out_seconds)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "audio_stop: the fade must be finite and not negative");
    }
    if (engine->game.clock.resimulating() || voice == 0) {
        return finish(CY_RESULT_OK);
    }
    return finish(backend->stop(voice, fade_out_seconds));
}

bool audio_voice_playing(CyEngine engine, CyAudioVoice voice) {
    AudioBackend* backend = enter(engine, "audio_voice_playing");
    if (backend == nullptr || voice == 0) {
        return false;
    }
    return backend->playing(voice);
}

CyResult audio_find_bus(CyEngine engine, const char* name, CyAudioBus* out_bus) {
    AudioBackend* backend = enter(engine, "audio_find_bus");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (name == nullptr || out_bus == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "audio_find_bus needs a name and an output");
    }
    CyAudioBus bus = 0;
    if (const CyResult result = backend->find_bus(name, bus); result != CY_RESULT_OK) {
        return result;
    }
    *out_bus = bus;
    return finish(CY_RESULT_OK);
}

CyResult audio_set_bus_volume(CyEngine engine, CyAudioBus bus, float volume, float fade_seconds) {
    AudioBackend* backend = enter(engine, "audio_set_bus_volume");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (!std::isfinite(volume) || !non_negative(fade_seconds)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "audio_set_bus_volume: the volume must be finite and the fade finite and "
                      "not negative");
    }
    if (volume < 0.0F) {
        return report(CY_RESULT_OUT_OF_RANGE, "audio_set_bus_volume: the volume is below zero");
    }
    if (engine->game.clock.resimulating()) {
        return finish(CY_RESULT_OK);
    }
    return finish(backend->set_bus_volume(bus, volume, fade_seconds));
}

}  // namespace cy::abi::game
