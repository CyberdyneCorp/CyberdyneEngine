// SPDX-License-Identifier: MIT
// cy/abi/game/audio.h — the backend behind ABI 1.3's `audio_*` entries. `add-swift-game-api`.
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
};

}  // namespace cy::abi::game
