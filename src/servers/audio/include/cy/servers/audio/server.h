#pragma once
// `AudioServer`: the engine-owned mixer over the driver layer. Tasks 4.3.4 and 4.3.5.
//
// `audio` reaches **Seed** at M4 — "interfaces, data model and invariants exist; dependents can be
// built against it". What is real and what is deliberately absent is at the bottom of this comment.
//
// ================================================================================================
// THE ENGINE OWNS THE POLICY. THE BACKEND OWNS THE DEVICE.
// ================================================================================================
//
// `audio`: "`AudioServer`, the bus graph, voice management, streaming policy, spatialisation
// policy, and the importance system SHALL be engine code", and "**WHEN** voice limits, tier
// budgets, streaming residency, or job scheduling are decided **THEN** they SHALL be enforced by
// engine code, not delegated to a backend's own policy."
//
// That is why this class mixes rather than asking miniaudio to. miniaudio has a node graph, a
// high-level engine API and its own 3D spatialisation, and the specification says in as many words
// that the engine "SHALL NOT use" any of the three. What it uses miniaudio for is four things —
// device I/O, conversion, decoding, streaming — and keeping the surface that small is what makes
// the backend genuinely replaceable rather than nominally so.
//
// ================================================================================================
// TWO THREADS, AND WHICH ONE OWNS WHAT
// ================================================================================================
//
// GAME THREAD:   `play`, `stop`, `set_*`, `update()`. Owns every `VoiceControl`, computes the
//                importance scores, assigns the tiers, and computes the target gains.
// AUDIO THREAD:  `render()`, called by the backend from its own thread. Owns each voice's playback
//                position, its filter state and its fade. Allocates nothing, takes no lock, opens
//                no file and calls into no script.
//
// They meet in exactly two places, and nowhere else:
//
//   * `CommandQueue` (commands.h), single-producer/single-consumer, drained at the top of every
//     `render()` — `audio`'s "commands SHALL be enqueued to a lock-free queue consumed at the start
//     of each callback";
//   * the DOUBLE-BUFFERED mix state below, published by `update()` with one release store and read
//     by `render()` with one acquire load — the requirement's "voice state SHALL be double
//     buffered".
//
// A voice's immutable half — its clip, its bus, whether it loops — is written by the game thread
// BEFORE the `Play` command is pushed, and read by the audio thread only after it is popped. The
// queue's release/acquire pair is what makes that safe, which is why the immutable half is
// immutable: a later write to it would have no such pairing.
//
// ================================================================================================
// WHAT SEED MEANS HERE
// ================================================================================================
//
// REAL AND EXERCISED: the `AudioBackend` interface and its retained null implementation; the bus
// graph with cycle rejection, solo, sends and a four-effect chain; voices with play, stop, pause,
// seek, loop, volume, pitch, bus assignment, randomised variation and one-shots; sample-accurate
// looping and scheduled starts; block mixing with per-block gain ramps; distance attenuation in
// five models, cones, constant-power stereo panning, Doppler and filter-based occlusion; the
// importance score, the four tiers, their budgets, hysteresis and voice virtualisation; the audio
// clock; the lock-free command queue and the double-buffered mix state; the diagnostics `audio`
// requires.
//
// ABSENT, DELIBERATELY, AND NAMED SO NOBODY MISTAKES IT FOR MORE: decoding and streaming (the
// engine plays float PCM the asset system owns — see voice.h); the sixteen effects beyond the four
// in bus.h; `AcousticsBackend`, HRTF, propagation, reflections and acoustic geometry (M8, and the
// fallback path here is what the specification requires to exist without them); interactive music
// and playlists; the middleware plugin backends. None of them is stubbed: a stub that returns a
// plausible value is worse than an absent function, because the first one produces a mix nobody can
// tell is wrong.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/audio/backend.h>
#include <cy/servers/audio/bus.h>
#include <cy/servers/audio/commands.h>
#include <cy/servers/audio/handles.h>
#include <cy/servers/audio/spatial.h>
#include <cy/servers/audio/voice.h>

#include <atomic>

namespace cy::audio {

/// Per-tier voice budgets, in the tier order `SimulationTier` declares them.
///
/// `audio`: "Each tier SHALL have a configurable **budget** as a maximum source count; sources
/// beyond a tier's budget SHALL be demoted to the next tier by importance rank", and "**WHEN** a
/// project targets lower-end hardware **THEN** reducing tier budgets SHALL reduce audio cost
/// predictably without content changes."
struct TierBudgets {
    u32 full_acoustic = 8;
    u32 spatialised = 64;
    u32 simple = 128;

    [[nodiscard]] u32 for_tier(SimulationTier tier) const noexcept;
};

struct AudioServerConfig {
    /// What `initialize()` asks the null backend for, and what a caller passes on to a real one. A
    /// device negotiates; the null backend grants it exactly, which is what makes a test's
    /// assertions about frame counts mean something.
    AudioFormat requested;
    u32 voice_capacity = 256;
    u32 clip_capacity = 256;
    u32 listener_capacity = 4;
    /// The mixer's block size, in frames. The device's buffer is mixed as a whole number of these;
    /// a smaller block costs a little more per-block overhead and buys finer gain ramps.
    u32 block_frames = 120;
    /// The number of buses the mixer's buffers are sized for. Fixed at `initialize()` because the
    /// audio thread must not allocate, so creating more buses than this is refused at creation with
    /// a diagnostic rather than discovered in the callback.
    u32 bus_capacity = 32;

    TierBudgets budgets;
    /// The fade a stop applies, in seconds. Short enough not to be a delay, long enough not to be a
    /// step: 10 ms is about 480 frames at 48 kHz.
    f32 stop_fade_seconds = 0.01F;
    f32 speed_of_sound = 343.0F;
    f32 doppler_factor = 1.0F;
    /// The importance margin a voice must beat to be promoted a tier. Prevents a source hovering at
    /// a boundary from flapping every update — `audio`'s "Hysteresis prevents oscillation".
    f32 tier_hysteresis = 0.05F;
};

/// Per-bus levels, for the meter `audio`'s diagnostics require.
struct BusLevel {
    f32 peak = 0.0F;
    f32 rms = 0.0F;
};

/// What `audio`'s "Audio diagnostics" requirement asks to be reported.
struct AudioStatistics {
    u32 active_voices = 0;
    u32 virtual_voices = 0;
    u32 voices_in_tier[static_cast<usize>(SimulationTier::Count)] = {0, 0, 0, 0};
    /// Voices demoted by budget pressure in the last update. "**WHEN** important sounds are being
    /// demoted **THEN** per-tier counts and demotion statistics SHALL show which budget is
    /// saturated."
    u32 demoted_by_budget = 0;
    /// Voices that asked for `FullAcoustic` with no acoustics backend present, and were rendered as
    /// `Spatialised`. Counted rather than silent: the specification's fallback is legitimate, and a
    /// project that thinks it is getting propagation should be able to see that it is not.
    u32 acoustic_fallbacks = 0;
    /// Blocks the mixer could not produce in time. Zero with the null backend, by construction.
    u32 underruns = 0;
    /// Commands the queue was too full to take. A non-zero value here means a `Stop` may have been
    /// lost, which is why it is counted rather than dropped silently.
    u32 dropped_commands = 0;
    u32 blocks_mixed = 0;
};

/// The audio server.
class AudioServer {
public:
    /// The command queue's depth. A power of two, and generous: a frame that starts two hundred
    /// one-shots is a legitimate explosion, and the cost is 48 bytes a slot.
    static constexpr u32 kCommandCapacity = 1024;

    explicit AudioServer(Allocator& allocator) noexcept;
    ~AudioServer();

    AudioServer(const AudioServer&) = delete;
    AudioServer& operator=(const AudioServer&) = delete;

    // --- The four methods `runtime::Server` declares ---------------------------------------------
    //
    // Same names, same signatures. The runtime's adapter forwards to these and adds nothing; this
    // class does not derive from `runtime::Server` because that interface is at layer 5.

    [[nodiscard]] const char* backend_name() const noexcept;
    /// Bring up with no device: the retained null backend. What a headless build and every test
    /// get.
    [[nodiscard]] Status initialize() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] bool is_null_backend() const noexcept;

    /// Bring up over a supplied backend. The backend is borrowed, not owned — whoever created it
    /// outlives the server, which is the same ownership the server registry uses.
    [[nodiscard]] Status initialize_with(AudioBackend& backend) noexcept;

    [[nodiscard]] Status configure(const AudioServerConfig& config) noexcept;
    [[nodiscard]] const AudioServerConfig& configuration() const noexcept { return config_; }
    [[nodiscard]] AudioFormat format() const noexcept { return format_; }

    /// Reinitialise after the device changed, keeping every playing voice. `audio`: "**WHEN** the
    /// default output device changes **THEN** the driver SHALL reinitialise at the new device's mix
    /// rate and channel layout, and playback SHALL continue."
    [[nodiscard]] Status device_changed() noexcept;

    // --- The bus graph ---------------------------------------------------------------------------

    [[nodiscard]] BusGraph& buses() noexcept { return buses_; }
    [[nodiscard]] const BusGraph& buses() const noexcept { return buses_; }

    // --- Clips -----------------------------------------------------------------------------------

    /// Register sample data. The bytes are borrowed — see voice.h.
    [[nodiscard]] Expected<ClipHandle, Error> create_clip(const ClipDescription& clip) noexcept;
    /// Stop every voice playing the clip, then release it. That order is the only safe one, so it
    /// is the one the server does rather than one a caller has to remember.
    void destroy_clip(ClipHandle handle) noexcept;
    [[nodiscard]] const ClipDescription* clip(ClipHandle handle) const noexcept;

    // --- Listeners -------------------------------------------------------------------------------

    [[nodiscard]] Expected<ListenerHandle, Error> create_listener(
        const Listener& listener) noexcept;
    [[nodiscard]] Status set_listener(ListenerHandle handle, const Listener& listener) noexcept;
    void destroy_listener(ListenerHandle handle) noexcept;
    /// The highest-priority active listener, which is what a mono mix spatialises against. Split
    /// screen renders per listener at M8; at Seed the interface is here and the mix is one.
    [[nodiscard]] const Listener* primary_listener() const noexcept;

    // --- Playback --------------------------------------------------------------------------------

    [[nodiscard]] Expected<VoiceHandle, Error> play(const VoiceDescription& description) noexcept;
    /// Fire and forget. `audio`: "**WHEN** a one-shot is fired **THEN** it SHALL play to completion
    /// and release itself, with no gameplay handle required."
    [[nodiscard]] Status play_one_shot(const VoiceDescription& description) noexcept;

    [[nodiscard]] Status stop(VoiceHandle handle) noexcept;
    [[nodiscard]] Status pause(VoiceHandle handle) noexcept;
    [[nodiscard]] Status resume(VoiceHandle handle) noexcept;
    [[nodiscard]] Status seek(VoiceHandle handle, u32 frame) noexcept;
    [[nodiscard]] Status set_volume(VoiceHandle handle, f32 volume) noexcept;
    [[nodiscard]] Status set_pitch(VoiceHandle handle, f32 pitch) noexcept;
    [[nodiscard]] Status set_position(VoiceHandle handle, Vec3 position, Vec3 velocity) noexcept;
    [[nodiscard]] Status set_occlusion(VoiceHandle handle, f32 occlusion) noexcept;

    [[nodiscard]] bool playing(VoiceHandle handle) const noexcept;
    [[nodiscard]] SimulationTier tier_of(VoiceHandle handle) const noexcept;
    /// Playback position in clip frames. Advances for a virtual voice too, which is the whole point
    /// of virtualising rather than stopping.
    [[nodiscard]] f64 position_of(VoiceHandle handle) const noexcept;

    // --- The frame
    // ---------------------------------------------------------------------------------

    /// GAME THREAD. Score every voice, assign tiers against the budgets, compute the target gains,
    /// and publish them. Reaps voices the mixer finished.
    void update(f32 delta_seconds) noexcept;

    /// AUDIO THREAD. Produce `frame_count` interleaved frames. Called by the backend.
    ///
    /// The realtime contract in full: allocates nothing, takes no lock, touches no file and calls
    /// into no script. Everything it reads was published by `update()` or arrived through the
    /// command queue.
    void render(f32* frames, u32 frame_count, u32 channels) noexcept;

    /// The static function a backend's `start()` takes, which forwards to `render()`.
    static void render_callback(f32* frames, u32 frame_count, u32 channels, void* user) noexcept;

    [[nodiscard]] AudioClock clock() const noexcept;
    [[nodiscard]] const AudioStatistics& statistics() const noexcept { return statistics_; }
    [[nodiscard]] Span<const BusLevel> bus_levels() const noexcept { return levels_.span(); }

private:
    struct VoiceSlot {
        VoiceControl control;
        VoicePlayback playback;
        /// Double buffered: `update()` writes the index `render()` is not reading. See the class
        /// comment.
        VoiceMixState mix[2];
        u32 generation = 0;
        /// Set by the audio thread when a clip ran out or a fade finished; read and cleared by
        /// `update()`. `u32` rather than `bool` because a relaxed atomic on a byte and on a word
        /// cost the same and the word is what the platform actually does.
        std::atomic<u32> finished{0};
    };

    struct ClipSlot {
        ClipDescription desc;
        u32 generation = 0;
        bool live = false;
        /// `destroy_clip()` sets this and stops the voices playing the clip. The slot is reused
        /// only once `update()` has seen those voices finish — the audio thread may still be inside
        /// a block that reads the samples, and there is no lock to wait on it with. See the method.
        bool pending_release = false;
    };

    struct ListenerSlot {
        Listener listener;
        u32 generation = 0;
        bool live = false;
    };

    [[nodiscard]] Status allocate_buffers() noexcept;
    /// The deterministic stream the pitch and volume variation are rolled from. Seeded at
    /// construction and advanced once per `play()`, so a fixed sequence of calls produces a fixed
    /// sequence of variations — `simulation-and-determinism` requires randomness to come from a
    /// declared stream, never from a global generator.
    [[nodiscard]] f32 next_variation() noexcept;
    [[nodiscard]] Expected<u32, Error> allocate_voice(const VoiceDescription& description) noexcept;
    [[nodiscard]] u32 slot_of(VoiceHandle handle) const noexcept;
    [[nodiscard]] bool enqueue(const AudioCommand& command) noexcept;

    /// `update()`'s three phases, split so each is one idea.
    void score_voices(const Listener& listener) noexcept;
    void assign_tiers() noexcept;
    void publish_mix_state(const Listener& listener) noexcept;

    /// `render()`'s phases.
    void drain_commands() noexcept;
    void mix_block(u32 block_frames, u32 channels, u32 published) noexcept;
    void mix_voice(VoiceSlot& slot, const VoiceMixState& mix, u32 block_frames,
                   u32 channels) noexcept;
    /// Advance a virtualised voice's position without mixing it.
    void advance_virtual(VoiceSlot& slot, const VoiceMixState& mix, u32 block_frames) noexcept;
    void process_buses(u32 block_frames, u32 channels) noexcept;
    /// Sum one processed bus into its output and every send.
    void route_bus(BusHandle handle, const f32* buffer, usize count, u32 channels) noexcept;
    [[nodiscard]] f32* bus_buffer(u32 bus_index, u32 channels) noexcept;

    Allocator* allocator_;
    AudioServerConfig config_;
    AudioFormat format_;
    AudioBackend* backend_ = nullptr;
    NullAudioBackend null_backend_;
    bool initialized_ = false;

    BusGraph buses_;
    Array<VoiceSlot> voices_;
    Array<ClipSlot> clips_;
    Array<ListenerSlot> listeners_;

    /// bus_capacity * channels * block_frames floats, allocated once at `initialize()`.
    Array<f32> bus_buffers_;
    Array<BusLevel> levels_;
    /// Scratch for reading a voice's samples before they are panned. One block, one channel pair.
    Array<f32> voice_scratch_;
    /// Voice slots ordered by importance, rebuilt by `update()` on the game thread. Never touched
    /// by the mixer.
    Array<u32> ranking_;

    CommandQueue<kCommandCapacity> commands_;
    /// Which of the two `VoiceMixState` buffers `render()` should read. Written by `update()` with
    /// a release store, read by `render()` with an acquire load.
    std::atomic<u32> published_mix_{0};
    std::atomic<u64> frames_played_{0};

    AudioStatistics statistics_;
    u64 variation_state_ = 0x9E3779B97F4A7C15ULL;
};

}  // namespace cy::audio
