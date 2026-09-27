// SPDX-License-Identifier: MIT
// cy/game_backend/audio_backend.h — the `audio` adapter behind ABI 1.3. `add-swift-game-api`.
//
// OWNER: implementer C. Implements cy/abi/game/audio.h over `cy::audio::AudioServer` and its
// `BusGraph`, to the contract in design.md.
//
// WHAT THE ADAPTER ADDS TO THE SERVER. The server plays clips by handle, stops with its own short
// click guard, and sets bus gains at once. The ABI names cues and buses, fades a stop, a start and
// a bus over a caller's duration, and lets a voice follow an entity. So the adapter keeps three
// small tables — the cue names, the voices it is fading or moving, the buses it is ramping — and
// advances them in `update()`, which the embedder calls once per frame on the game thread, before
// `AudioServer::update`.
//
// HANDLES. `CyAudioCue` carries a `ClipHandle`'s bits, `CyAudioBus` a `BusHandle`'s and
// `CyAudioVoice` a `VoiceHandle`'s, generation included. Nothing is looked up by address, so a
// voice whose slot was reused answers "not playing", and stopping it touches nobody else's sound.

#pragma once

#include <cy/abi/game/audio.h>
#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>
#include <cy/servers/audio/server.h>

namespace cy::scene {
class SceneTree;
}  // namespace cy::scene

namespace cy::game_backend {

/// Implements `cy::abi::game::AudioBackend`.
class AudioAdapter final : public abi::game::AudioBackend {
public:
    /// `server` is borrowed and outlives the adapter. `tree`, when given, is where an attached
    /// voice reads its entity's world position; with none, CY_AUDIO_PLAY_ATTACH is UNAVAILABLE.
    AudioAdapter(audio::AudioServer& server, Allocator& allocator,
                 scene::SceneTree* tree = nullptr) noexcept;

    AudioAdapter(const AudioAdapter&) = delete;
    AudioAdapter& operator=(const AudioAdapter&) = delete;
    AudioAdapter(AudioAdapter&&) = delete;
    AudioAdapter& operator=(AudioAdapter&&) = delete;
    ~AudioAdapter() override = default;

    /// Name a clip the server holds as a cue. Naming it again points the name at the new clip.
    [[nodiscard]] Status add_cue(Name name, audio::ClipHandle clip) noexcept;

    /// Once per frame, game thread: advance fades, move attached voices to their entities, and
    /// forget voices the mixer finished.
    void update(f32 delta_seconds) noexcept;

    /// Voices the adapter is still fading or moving. For a test and for diagnostics.
    [[nodiscard]] usize tracked_voices() const noexcept { return voices_.size(); }

    // --- AudioBackend ---------------------------------------------------------------------------
    [[nodiscard]] CyResult find_cue(const char* name, CyAudioCue& out_cue) noexcept override;
    [[nodiscard]] CyResult play(const CyAudioPlay& play, CyAudioVoice& out_voice) noexcept override;
    [[nodiscard]] CyResult stop(CyAudioVoice voice, f32 fade_out_seconds) noexcept override;
    [[nodiscard]] bool playing(CyAudioVoice voice) const noexcept override;
    [[nodiscard]] CyResult find_bus(const char* name, CyAudioBus& out_bus) noexcept override;
    [[nodiscard]] CyResult set_bus_volume(CyAudioBus bus, f32 volume,
                                          f32 fade_seconds) noexcept override;

private:
    struct Cue {
        Name name;
        audio::ClipHandle clip;
    };

    /// A linear ramp from `from` to `to` over `seconds`.
    struct Ramp {
        f32 from = 0.0F;
        f32 to = 0.0F;
        f32 seconds = 0.0F;
        f32 elapsed = 0.0F;

        [[nodiscard]] bool active() const noexcept { return seconds > 0.0F; }
        [[nodiscard]] bool done() const noexcept { return elapsed >= seconds; }
        /// Advance by `delta` and return the value now.
        f32 advance(f32 delta) noexcept;
    };

    /// A voice with something left to do each frame: a fade in or out, or an entity to follow.
    struct Voice {
        audio::VoiceHandle handle;
        ecs::Entity attached;
        Vec3 offset{0.0F, 0.0F, 0.0F};
        f32 volume = 1.0F;  ///< the gain it is at now, which a fade-out starts from
        Ramp fade;
        bool stopping = false;
    };

    struct BusFade {
        audio::BusHandle bus;
        Ramp ramp;
    };

    [[nodiscard]] Voice* tracked(audio::VoiceHandle handle) noexcept;
    [[nodiscard]] CyResult describe(const CyAudioPlay& play,
                                    audio::VoiceDescription& out) const noexcept;
    [[nodiscard]] CyResult attachment_position(ecs::Entity entity, Vec3 offset,
                                               Vec3& out_position) const noexcept;
    [[nodiscard]] CyResult track(const CyAudioPlay& play, audio::VoiceHandle handle,
                                 f32 volume) noexcept;
    /// One tracked voice's frame. False when the voice is finished and should be forgotten.
    [[nodiscard]] bool advance_voice(Voice& voice, f32 delta_seconds) noexcept;
    void advance_buses(f32 delta_seconds) noexcept;

    audio::AudioServer& server_;
    scene::SceneTree* tree_;
    Array<Cue> cues_;
    Array<Voice> voices_;
    Array<BusFade> bus_fades_;
};

/// Bind `adapter` as `host`'s audio backend (`host.game.audio`), or unbind with null. The one place
/// an embedder wires the audio service; the adapter must outlive the binding.
void bind(cy::abi::Host& host, AudioAdapter* adapter) noexcept;

}  // namespace cy::game_backend
