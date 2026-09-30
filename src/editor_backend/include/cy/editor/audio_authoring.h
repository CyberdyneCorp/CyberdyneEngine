// SPDX-License-Identifier: MIT
// cy/editor/audio_authoring.h — the engine half of the editor's audio tools. Issue #29.
//
// The editor authors two project assets and one scene component, and never mixes a sample:
//
//   *.cymixer                 the bus graph: buses, routing, sends, the effect chain
//   *.cycue                   one playable sound: a clip, its bus, gain, pitch and variation
//   cy::audio::AudioSource    a scene entity that plays a cue at its transform
//
// This file turns each of them into `cy::audio::AudioServer` state. The editor sends the asset's
// text over the backend service (`audio.*` operations, see audio_service.h) and reads back what the
// server now holds, so an author sees the gain the mixer applied rather than the one they typed.
//
// ONE SERVER, THREE USERS. The window runtime owns one `AudioAuthoring`; the mixer editor, cue
// preview and editor Play all go through it, and the Swift adapter it exposes is the one ABI 1.3's
// `audio_*` entries reach during Play. A bus renamed in the mixer is the bus `Audio.bus("Music")`
// finds on the next press of Play.
//
// DRIVEN WHEN THERE IS NO DEVICE. With the null backend nothing pulls the mix, so `pump` advances
// it by the frame's worth of samples; with a device the device thread renders and `pump` only runs
// the game-thread half. Either way the per-bus levels the editor reads are the mixer's own.

#pragma once

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/game_backend/audio_backend.h>
#include <cy/servers/audio/backend.h>
#include <cy/servers/audio/server.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cy::scene::serialization {
class World;
}  // namespace cy::scene::serialization

namespace cy::editor {

/// The scene component a sound-emitting entity carries. Matched by name, like `cy::vfx::Effect`.
inline constexpr std::string_view kAudioSourceComponent = "cy::audio::AudioSource";

/// Where a project keeps its mixer, as `cy_editor_services::audio::DEFAULT_MIXER` says.
inline constexpr std::string_view kProjectMixer = "game/audio/mixer.cymixer";

/// One send of a mixer bus.
struct MixerSend {
    std::string target;
    f32 level = 0.0F;
};

/// One effect in a mixer bus's chain, in the server's own terms.
struct MixerEffect {
    audio::EffectKind kind = audio::EffectKind::Gain;
    f32 parameter_a = 1.0F;
    f32 parameter_b = 0.0F;
    bool bypass = false;
};

/// One bus of a `.cymixer` asset.
struct MixerBus {
    std::string name;
    std::string output;  ///< empty for Master
    f32 volume = 1.0F;
    bool mute = false;
    bool solo = false;
    bool bypass = false;
    std::vector<MixerSend> sends;
    std::vector<MixerEffect> effects;
};

/// A whole `.cymixer` asset, validated: Master first, every name unique, every route known, and
/// the graph acyclic through outputs and sends together.
struct Mixer {
    std::vector<MixerBus> buses;
};

/// Parse and validate `cymixer 1` text. Refuses, naming the line, anything the server would refuse.
[[nodiscard]] Expected<Mixer, Error> parse_mixer(std::string_view text) noexcept;

/// The effect kind a mixer names, or `EffectKind::Count` for a name the server does not have.
[[nodiscard]] audio::EffectKind effect_kind_named(std::string_view name) noexcept;

/// The attenuation model a source names, or `AttenuationModel::Count`.
[[nodiscard]] audio::AttenuationModel attenuation_model_named(std::string_view name) noexcept;

/// A `.cycue` asset.
struct Cue {
    std::string clip;  ///< `tone:<hertz>:<seconds>` or a project-relative `.wav`
    std::string bus = "Master";
    f32 volume = 1.0F;
    f32 pitch = 1.0F;
    f32 volume_variation = 0.0F;
    f32 pitch_variation = 0.0F;
    bool looping = false;
};

/// Parse and validate `cycue 1` text.
[[nodiscard]] Expected<Cue, Error> parse_cue(std::string_view text) noexcept;

/// Where a preview voice is heard from. Non-spatial previews ignore everything but `spatial`.
struct PreviewPlacement {
    bool spatial = false;
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 listener{0.0F, 0.0F, 0.0F};
    Vec3 listener_forward{0.0F, 0.0F, -1.0F};
    f32 min_distance = 1.0F;
    f32 max_distance = 100.0F;
    audio::AttenuationModel model = audio::AttenuationModel::Inverse;
};

/// An authored audio source in the open world, for the viewport's emitter gizmo.
struct EmitterMarker {
    u64 identity = 0;
    Vec3 position{0.0F, 0.0F, 0.0F};
    f32 min_distance = 1.0F;
    f32 max_distance = 100.0F;
    audio::AttenuationModel model = audio::AttenuationModel::Inverse;
    std::string cue;
    bool autoplay = true;
    bool enabled = true;
};

/// Read every `cy::audio::AudioSource` in a world. A malformed source is refused by name rather
/// than skipped, because a silent emitter is indistinguishable from a quiet one.
[[nodiscard]] Expected<std::vector<EmitterMarker>, Error> read_emitters(
    const scene::serialization::World& world) noexcept;

/// What the last preview started, measured by the engine.
struct PreviewReport {
    bool present = false;
    std::string cue;
    bool playing = false;
    f32 distance = 0.0F;
    f32 gain = 1.0F;  ///< the attenuation the server's own curve gives at `distance`
    f32 left = 1.0F;  ///< constant-power pan, in the listener's frame
    f32 right = 1.0F;
};

/// The mixer, the cues, the preview and Play, over one `AudioServer`.
class AudioAuthoring {
public:
    static constexpr u32 kSampleRate = 48000;
    static constexpr u32 kBlockFrames = 240;

    /// `device`, when given, is borrowed and must outlive this; without one the null backend is
    /// used and `pump` drives it.
    explicit AudioAuthoring(Allocator& allocator, audio::AudioBackend* device = nullptr) noexcept;
    ~AudioAuthoring();
    AudioAuthoring(const AudioAuthoring&) = delete;
    AudioAuthoring& operator=(const AudioAuthoring&) = delete;
    AudioAuthoring(AudioAuthoring&&) = delete;
    AudioAuthoring& operator=(AudioAuthoring&&) = delete;

    [[nodiscard]] Status initialize() noexcept;
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    /// The directory a `.wav` clip and a source's `.cycue` are read from.
    void set_project(std::string root) noexcept { project_ = std::move(root); }

    /// Reconcile the server's bus graph with `cymixer 1` text. Nothing changes when it is refused.
    [[nodiscard]] Status apply_mixer(std::string_view text) noexcept;

    /// Make `cymixer 1` text the graph, keeping each bus that keeps its name.
    [[nodiscard]] Status apply_mixer(const Mixer& mixer) noexcept;

    /// Apply the project's `game/audio/mixer.cymixer`, when it has one. What the runtime does when
    /// it starts and when Play starts, so the graph Play mixes through is the one on disk whether
    /// or not an editor sent it.
    [[nodiscard]] Status apply_project_mixer() noexcept;

    /// Load or replace a cue under `name`, and name it for Swift.
    [[nodiscard]] Status load_cue(std::string_view name, std::string_view text) noexcept;

    /// Play a loaded cue once as a preview. The previous preview voice is stopped first.
    [[nodiscard]] Status preview(std::string_view name, const PreviewPlacement& placement) noexcept;

    /// Stop every preview voice.
    void stop_preview() noexcept;

    /// Start every enabled, autoplaying source in `world`, spatialised at its transform.
    [[nodiscard]] Status start_play(const scene::serialization::World& world) noexcept;

    /// Stop what `start_play` started.
    void stop_play() noexcept;

    /// Hold or resume what `start_play` started, for Play's Pause.
    void pause_play(bool paused) noexcept;

    /// Where the listener is: the camera Play renders from, or the editor's camera for a preview.
    void set_listener(Vec3 position, Vec3 forward) noexcept;

    /// One frame: the adapter's fades, the server's update, and the mix when nothing else pulls it.
    void pump(f32 delta_seconds) noexcept;

    /// Encode the state reply every `audio.*` operation answers with (schema 1).
    [[nodiscard]] Status encode_state(Array<u8>& out) const noexcept;

    /// Encode `audio.capabilities.get`'s reply: the vocabulary the mixer editor offers.
    [[nodiscard]] Status encode_capabilities(Array<u8>& out) const noexcept;

    [[nodiscard]] audio::AudioServer& server() noexcept { return server_; }
    [[nodiscard]] const audio::AudioServer& server() const noexcept { return server_; }
    /// What ABI 1.3's `audio_*` entries reach during Play.
    [[nodiscard]] game_backend::AudioAdapter& adapter() noexcept { return adapter_; }
    [[nodiscard]] audio::BusHandle bus(std::string_view name) const noexcept;
    [[nodiscard]] const PreviewReport& last_preview() const noexcept { return preview_report_; }
    [[nodiscard]] usize play_voices() const noexcept { return play_voices_.size(); }
    [[nodiscard]] bool playing() const noexcept { return play_active_; }
    [[nodiscard]] usize cues() const noexcept { return cues_.size(); }

private:
    /// A voice this object started, and the bus it was started on, which a mixer edit can remove.
    struct ActiveVoice {
        audio::VoiceHandle voice;
        audio::BusHandle bus;
    };

    struct LoadedCue {
        std::string name;
        Cue cue;
        std::vector<f32> samples;
        audio::ClipHandle clip;
    };

    [[nodiscard]] Status reconcile(const Mixer& mixer) noexcept;
    [[nodiscard]] Status configure_bus(const MixerBus& bus) noexcept;
    [[nodiscard]] const LoadedCue* find_cue(std::string_view name) const noexcept;
    [[nodiscard]] Expected<const LoadedCue*, Error> cue_for_source(
        std::string_view reference) noexcept;
    [[nodiscard]] Status render_clip(const Cue& cue, std::vector<f32>& samples,
                                     u32& channels) const noexcept;
    [[nodiscard]] Expected<ActiveVoice, Error> start_voice(const LoadedCue& cue,
                                                           const PreviewPlacement& placement,
                                                           bool looping) noexcept;
    void stop_voices_on_dead_buses() noexcept;
    void forget_finished_voices() noexcept;

    Allocator* allocator_;
    audio::AudioBackend* device_;
    std::unique_ptr<audio::NullAudioBackend> null_device_;
    audio::AudioServer server_;
    game_backend::AudioAdapter adapter_;
    std::string project_;
    std::vector<std::string> bus_names_;
    std::vector<audio::BusHandle> bus_handles_;
    std::vector<std::unique_ptr<LoadedCue>> cues_;
    std::vector<ActiveVoice> preview_voices_;
    std::vector<ActiveVoice> play_voices_;
    std::vector<f32> mix_scratch_;
    audio::ListenerHandle listener_;
    PreviewReport preview_report_;
    f32 pending_seconds_ = 0.0F;
    bool play_active_ = false;
    bool initialized_ = false;
};

}  // namespace cy::editor
