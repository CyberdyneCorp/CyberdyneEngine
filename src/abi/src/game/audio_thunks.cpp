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

// --- 1.18: streamed tracks ----------------------------------------------------------------------

namespace {

[[nodiscard]] CyResult no_streams() noexcept {
    return report(CY_RESULT_UNAVAILABLE, "this engine's audio backend has no stream player");
}

/// The numbers in a stream request: a gain, a fade and a start that are finite and not negative.
[[nodiscard]] bool valid_stream_play(const CyAudioStreamPlay& play) noexcept {
    return non_negative(play.volume) && non_negative(play.fade_seconds) &&
           std::isfinite(play.start_seconds) && play.start_seconds >= 0.0;
}

}  // namespace

CyResult AudioBackend::find_track(const char* name, CyAudioTrack& out_track) noexcept {
    (void)name;
    (void)out_track;
    return no_streams();
}

CyResult AudioBackend::stream_play(const CyAudioStreamPlay& play,
                                   CyAudioStream& out_stream) noexcept {
    (void)play;
    (void)out_stream;
    return no_streams();
}

CyResult AudioBackend::stream_stop(CyAudioStream stream, f32 fade_out_seconds) noexcept {
    (void)stream;
    (void)fade_out_seconds;
    return no_streams();
}

CyResult AudioBackend::stream_seek(CyAudioStream stream, f64 seconds) noexcept {
    (void)stream;
    (void)seconds;
    return no_streams();
}

CyResult AudioBackend::stream_set_volume(CyAudioStream stream, f32 volume,
                                         f32 fade_seconds) noexcept {
    (void)stream;
    (void)volume;
    (void)fade_seconds;
    return no_streams();
}

CyResult AudioBackend::stream_state(CyAudioStream stream, CyAudioStreamState& out_state) noexcept {
    (void)stream;
    (void)out_state;
    return no_streams();
}

CyResult audio_find_track(CyEngine engine, const char* name, CyAudioTrack* out_track) {
    AudioBackend* backend = enter(engine, "audio_find_track");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (name == nullptr || out_track == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "audio_find_track needs a name and an output");
    }
    CyAudioTrack track = 0;
    if (const CyResult result = backend->find_track(name, track); result != CY_RESULT_OK) {
        return result;
    }
    *out_track = track;
    return finish(CY_RESULT_OK);
}

CyResult audio_stream_play(CyEngine engine, const CyAudioStreamPlay* play,
                           CyAudioStream* out_stream) {
    AudioBackend* backend = enter(engine, "audio_stream_play");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (play == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "audio_stream_play needs a CyAudioStreamPlay");
    }
    CyAudioStreamPlay whole{};
    if (!read_sized(*play, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "audio_stream_play: the CyAudioStreamPlay struct_size is malformed");
    }
    if (!valid_stream_play(whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "audio_stream_play: volume, fade and start must be finite and not negative");
    }
    if (engine->game.clock.resimulating()) {
        if (out_stream != nullptr) {
            *out_stream = 0;
        }
        return finish(CY_RESULT_OK);
    }
    CyAudioStream stream = 0;
    if (const CyResult result = backend->stream_play(whole, stream); result != CY_RESULT_OK) {
        return result;
    }
    if (out_stream != nullptr) {
        *out_stream = stream;
    }
    return finish(CY_RESULT_OK);
}

CyResult audio_stream_stop(CyEngine engine, CyAudioStream stream, float fade_out_seconds) {
    AudioBackend* backend = enter(engine, "audio_stream_stop");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (!non_negative(fade_out_seconds)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "audio_stream_stop: the fade must be finite and not negative");
    }
    if (engine->game.clock.resimulating() || stream == 0) {
        return finish(CY_RESULT_OK);
    }
    return finish(backend->stream_stop(stream, fade_out_seconds));
}

CyResult audio_stream_seek(CyEngine engine, CyAudioStream stream, double seconds) {
    AudioBackend* backend = enter(engine, "audio_stream_seek");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "audio_stream_seek: the position must be finite and not negative");
    }
    if (engine->game.clock.resimulating()) {
        return finish(CY_RESULT_OK);
    }
    return finish(backend->stream_seek(stream, seconds));
}

CyResult audio_stream_set_volume(CyEngine engine, CyAudioStream stream, float volume,
                                 float fade_seconds) {
    AudioBackend* backend = enter(engine, "audio_stream_set_volume");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (!non_negative(volume) || !non_negative(fade_seconds)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "audio_stream_set_volume: the volume and fade must be finite and not "
                      "negative");
    }
    if (engine->game.clock.resimulating()) {
        return finish(CY_RESULT_OK);
    }
    return finish(backend->stream_set_volume(stream, volume, fade_seconds));
}

CyResult audio_stream_state(CyEngine engine, CyAudioStream stream, CyAudioStreamState* out_state) {
    AudioBackend* backend = enter(engine, "audio_stream_state");
    if (backend == nullptr) {
        return last_error_code();
    }
    if (out_state == nullptr || agreed_size<CyAudioStreamState>(out_state->struct_size) == 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "audio_stream_state needs an output with a valid struct_size");
    }
    CyAudioStreamState state{};
    state.struct_size = static_cast<u32>(sizeof(CyAudioStreamState));
    if (const CyResult result = backend->stream_state(stream, state); result != CY_RESULT_OK) {
        return result;
    }
    state.struct_size = static_cast<u32>(sizeof(CyAudioStreamState));
    (void)write_sized(*out_state, state);
    return finish(CY_RESULT_OK);
}

}  // namespace cy::abi::game
