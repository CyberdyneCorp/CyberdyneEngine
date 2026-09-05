#pragma once
// Clips, voices, playback state, and the simulation tiers that bound the cost of audio.
// Task 4.3.5.
//
// `audio` — "Audio components and playback" fixes what a source can do: "play, stop, pause, seek,
// loop, volume, pitch, bus assignment, priority, a randomised pitch and volume range, and one-shot
// fire-and-forget playback that does not require keeping a handle." "Audio importance and
// simulation tiers" bounds the cost, and "Voice virtualisation" says what happens past the budget.
//
// ================================================================================================
// A CLIP IS BORROWED, NOT OWNED
// ================================================================================================
//
// `Clip` holds a pointer to interleaved float samples and a description of them. It does not own
// the memory and it does not decode: the asset system owns the bytes, and `audio`'s three load
// modes — fully decoded, compressed in memory, streamed — are that system's to implement over this
// interface. At Seed the engine plays what is already float PCM in memory; Vorbis and Opus decoding
// is a backend capability (`audio`: miniaudio provides "decoding of the formats it supports
// natively") and arrives with the streaming policy at M8.
//
// The consequence a caller must respect, and the reason it is stated here rather than discovered:
// THE SAMPLES MUST OUTLIVE EVERY VOICE PLAYING THEM. `AudioServer::destroy_clip()` stops those
// voices first, which is the only safe order and therefore the one the server does.
//
// ================================================================================================
// VIRTUALISATION IS NOT STOPPING, AND THE DIFFERENCE IS THE WHOLE REQUIREMENT
// ================================================================================================
//
// "A source that is inaudible or beyond budget SHALL be **virtualised** rather than stopped: its
// playback position SHALL continue to advance, but no mixing or DSP SHALL be performed for it…
// **WHEN** the listener leaves and later re-enters a looping ambience's range **THEN** it SHALL
// resume at the position it would have reached, not restart."
//
// So a virtual voice still advances `position_frames` every block. It costs one multiply-add and a
// score, which is what makes thousands of them affordable, and it is why `VoiceState::Virtual` is a
// state of a live voice rather than a stopped one.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/values/name.h>
#include <cy/servers/audio/handles.h>
#include <cy/servers/audio/spatial.h>

namespace cy::audio {

/// Sample data the engine plays. See the header comment: borrowed, never owned.
struct ClipDescription {
    Name name;
    /// Interleaved float samples, `frame_count * channels` of them.
    const f32* samples = nullptr;
    u32 frame_count = 0;
    u32 channels = 1;
    u32 sample_rate = 48000;
    /// Sample-accurate loop points. `loop_end` of zero means "the end of the clip", so a zeroed
    /// description loops the whole clip rather than none of it.
    u32 loop_start = 0;
    u32 loop_end = 0;

    [[nodiscard]] u32 effective_loop_end() const noexcept {
        return (loop_end == 0 || loop_end > frame_count) ? frame_count : loop_end;
    }
};

/// `audio`'s four tiers, most to least expensive.
enum class SimulationTier : u8 {
    /// Propagation, reflections, geometry occlusion and HRTF. Requires an `AcousticsBackend`; with
    /// none present a source assigned this tier is rendered as `Spatialised`, which is the
    /// requirement's own fallback and not a silent demotion — `AudioStatistics` counts it.
    FullAcoustic = 0,
    /// Panning, distance attenuation, filter-based occlusion, reverb send.
    Spatialised,
    /// Distance attenuation only, mixed without panning.
    Simple,
    /// Position advanced, not mixed.
    Virtual,
    Count,
};

[[nodiscard]] const char* simulation_tier_name(SimulationTier tier) noexcept;

enum class VoiceState : u8 {
    Stopped = 0,
    Playing,
    Paused,
    /// Fading out toward a stop. `audio`: "Stopping or destroying a voice SHALL apply a short
    /// **fade-out** rather than cutting", because a mid-waveform stop is a step and a step is a
    /// click.
    Stopping,
    Count,
};

[[nodiscard]] const char* voice_state_name(VoiceState state) noexcept;

/// What a caller asks for when it plays something.
struct VoiceDescription {
    ClipHandle clip;
    /// Null routes to `Master`.
    BusHandle bus;
    f32 volume = 1.0F;
    f32 pitch = 1.0F;
    bool looping = false;
    /// False mixes the clip's own channels straight into the bus — music, interface sounds.
    bool spatialised = false;
    /// `audio` requires a randomised pitch and volume range, because a one-shot fired a hundred
    /// times at one pitch is what makes a game sound synthetic. Applied ONCE at start; a range
    /// re-rolled per block would be a tremolo.
    f32 volume_variation = 0.0F;
    f32 pitch_variation = 0.0F;

    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 velocity{0.0F, 0.0F, 0.0F};
    Vec3 forward{0.0F, 0.0F, -1.0F};
    Attenuation attenuation;
    Cone cone;
    /// 0 is a clear path, 1 fully occluded. Supplied by gameplay or by a physics query; the
    /// acoustics backend replaces it at M8.
    f32 occlusion = 0.0F;

    Name category;
    /// Higher survives budget pressure. Combined with audibility into the importance score.
    u8 priority = 128;
    /// Gameplay's floor. "**WHEN** a dialogue line is pinned to at least `Spatialised` **THEN** it
    /// SHALL never be demoted below that tier regardless of distance or budget pressure."
    SimulationTier minimum_tier = SimulationTier::Virtual;
    /// A one-shot releases itself when it finishes and needs no handle kept.
    bool one_shot = false;
    /// Start at this audio-clock frame rather than at the next block. `audio`: "Playback SHALL be
    /// schedulable at a **future audio time**, so sounds can be started exactly on a beat
    /// regardless of frame timing." Zero starts immediately.
    u64 start_frame = 0;
};

/// The half of a voice the GAME THREAD owns. Never read by the audio thread.
///
/// The split is the whole of `audio`'s thread-safety requirement made structural: a field is in
/// exactly one of these three structs, so "which thread may write this" is answered by where it is
/// declared rather than by a comment somebody has to find. See server.h's class comment for the two
/// places the halves meet.
struct VoiceControl {
    VoiceDescription desc;
    /// The variation rolled once at start. Applied to every block; re-rolling per block would be a
    /// tremolo rather than a variation.
    f32 volume_scale = 1.0F;
    f32 pitch_scale = 1.0F;
    /// The score from the last update, kept so the tier assignment has the previous tier as an
    /// input and so the diagnostics can say why a voice was demoted.
    f32 importance = 0.0F;
    SimulationTier tier = SimulationTier::Simple;
    bool live = false;
};

/// The half the AUDIO THREAD owns, plus the immutable fields the game thread writes before the
/// `Play` command is pushed and never touches again.
struct VoicePlayback {
    // --- Immutable after `Play` is pushed. The queue's release/acquire pair publishes them.
    ClipHandle clip;
    u32 bus_index = 0;
    bool looping = false;
    bool spatialised = false;
    bool one_shot = false;
    /// Audio-clock frame this voice starts at. Until then it is live and silent, which is how a
    /// sound lands exactly on a beat regardless of which frame asked for it.
    u64 start_frame = 0;

    // --- Audio thread only.
    VoiceState state = VoiceState::Stopped;
    /// Playback position, in FRAMES OF THE CLIP, as a double so that a pitch of 1.0001 does not
    /// quantise and a long track does not lose precision at the end. A float carries about seven
    /// digits, which a forty-minute track at 48 kHz exhausts.
    f64 position_frames = 0.0;
    /// The gains applied at the START of the current block. `audio`: "Voice gains SHALL be
    /// **interpolated** across the buffer from their previous to current values, so parameter
    /// changes do not click."
    PanGains previous_gains{0.0F, 0.0F};
    f32 previous_pitch = 1.0F;
    /// The fade applied while `Stopping`, from 1 to 0.
    f32 fade = 1.0F;
    /// One-pole occlusion filter state, per channel. Carried across blocks: a filter reset every
    /// block is a filter that clicks every block.
    f32 filter_state[2] = {0.0F, 0.0F};
};

/// What `update()` publishes and `render()` reads, double buffered. See server.h.
struct VoiceMixState {
    PanGains gains{0.0F, 0.0F};
    f32 pitch = 1.0F;
    /// One-pole coefficient for the occlusion filter, in [0, 1). Zero is no filtering.
    f32 filter_coefficient = 0.0F;
    SimulationTier tier = SimulationTier::Simple;
    bool audible = false;
};

/// The importance score `audio` requires: "distance to the listener, emitter volume and priority,
/// listener orientation, sound category, gameplay importance flags, current occlusion estimate, and
/// the source's tier in the previous frame".
///
/// A free function of the game-thread half and a listener, so it is testable without a server and
/// so a project replacing the policy has exactly one thing to replace.
[[nodiscard]] f32 voice_importance(const VoiceControl& voice, const Listener& listener) noexcept;

}  // namespace cy::audio
