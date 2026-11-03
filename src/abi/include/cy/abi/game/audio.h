// SPDX-License-Identifier: MIT
// cy/abi/game/audio.h — the backend behind ABI 1.3's `audio_*` entries. `add-swift-game-api`.
// ABI 1.18 adds the streamed-track entries (`audio_find_track`, `audio_stream_*`), issue #87.
//
// OWNER: implementer C (audio, spawning and time). See design.md in the change.
//
// Over `cy::audio::AudioServer` (cy/servers/audio/server.h) and its `BusGraph`. A cue name resolves
// to the authored clip or cue; `CyAudioVoice` carries a `VoiceHandle`'s bits, generation included,
// so a voice that ended and whose slot was reused answers "not playing" rather than stopping a
// stranger.
//
// The thunks drop `play` while the tick is a resimulation (it answers OK with a null voice), so a
// backend never plays a sound twice for one tick. An attached voice's position is refreshed from
// its entity once per frame by the adapter, not by a script.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>

namespace cy::abi::game {

/// The audio server as ABI 1.3's `audio_*` entries see it: cues, voices and mix buses by name.
class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    /// `audio_find_cue`: NOT_FOUND.
    [[nodiscard]] virtual CyResult find_cue(const char* name, CyAudioCue& out_cue) noexcept = 0;

    /// `audio_play`: `play` is whole. `out_voice` is written even when the caller passed null (the
    /// thunk supplies a scratch). Losing the mixer's priority contest is OK with a live handle that
    /// `playing` reports false.
    [[nodiscard]] virtual CyResult play(const CyAudioPlay& play,
                                        CyAudioVoice& out_voice) noexcept = 0;

    /// `audio_stop`: idempotent; an ended or stale voice is OK.
    [[nodiscard]] virtual CyResult stop(CyAudioVoice voice, f32 fade_out_seconds) noexcept = 0;

    /// `audio_voice_playing`: false for a null, stale or ended voice.
    [[nodiscard]] virtual bool playing(CyAudioVoice voice) const noexcept = 0;

    /// `audio_find_bus`: NOT_FOUND.
    [[nodiscard]] virtual CyResult find_bus(const char* name, CyAudioBus& out_bus) noexcept = 0;

    /// `audio_set_bus_volume`: `volume >= 0`, checked by the thunk.
    [[nodiscard]] virtual CyResult set_bus_volume(CyAudioBus bus, f32 volume,
                                                  f32 fade_seconds) noexcept = 0;

    // --- ABI 1.18: streamed tracks --------------------------------------------------------------
    //
    // Not pure: a backend with no stream player answers UNAVAILABLE, which is what a build that
    // only plays cues should say. The thunk has checked every number for finiteness and sign, and
    // drops the writes while the tick is a resimulation.

    /// `audio_find_track`: NOT_FOUND.
    [[nodiscard]] virtual CyResult find_track(const char* name, CyAudioTrack& out_track) noexcept;
    /// `audio_stream_play`: `play` is whole.
    [[nodiscard]] virtual CyResult stream_play(const CyAudioStreamPlay& play,
                                               CyAudioStream& out_stream) noexcept;
    /// `audio_stream_stop`: idempotent.
    [[nodiscard]] virtual CyResult stream_stop(CyAudioStream stream, f32 fade_out_seconds) noexcept;
    /// `audio_stream_seek`: `seconds >= 0`.
    [[nodiscard]] virtual CyResult stream_seek(CyAudioStream stream, f64 seconds) noexcept;
    /// `audio_stream_set_volume`: `volume >= 0`.
    [[nodiscard]] virtual CyResult stream_set_volume(CyAudioStream stream, f32 volume,
                                                     f32 fade_seconds) noexcept;
    /// `audio_stream_state`: NOT_FOUND once ended.
    [[nodiscard]] virtual CyResult stream_state(CyAudioStream stream,
                                                CyAudioStreamState& out_state) noexcept;
};

}  // namespace cy::abi::game
