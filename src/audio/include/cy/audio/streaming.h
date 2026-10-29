// SPDX-License-Identifier: MIT
#ifndef CY_AUDIO_STREAMING_H
#define CY_AUDIO_STREAMING_H
// Streamed music and ambience: tracks, the jobs that decode them, crossfades and seeks. Issue #87.
//
// `audio` — "Audio assets and streaming": "**WHEN** a long music track plays **THEN** it SHALL be
// streamed and decoded ahead, with only the ring buffer resident", "Streaming SHALL decode ahead
// into a ring buffer on the asset thread, with underrun producing silence and a diagnostic rather
// than a stall", and the engine's own policy that "streaming residency, or job scheduling … SHALL
// be enforced by engine code, not delegated to a backend's own policy". This is that policy.
//
// ================================================================================================
// THREE THREADS, AND WHAT EACH ONE DOES
// ================================================================================================
//
// GAME THREAD   `play`, `stop`, `crossfade`, `seek`, `set_volume`, and `pump()` once a frame. Pump
//               reaps finished streams, completes seeks, turns underruns into diagnostics and hands
//               every stream whose ring has room to the `RefillExecutor`. It never decodes and
//               never reads a file.
// REFILL        A job: the decoder is opened there (opening a Vorbis stream reads its headers and
//               scans its last page), it seeks there, and `StreamSource::fill` decodes there. One
//               refill per stream is in flight at a time, so a decoder is only ever touched by one
//               thread. `JobRefillExecutor` runs a refill that reads from disk on the
//               `AsyncService` thread — the one place `core-jobs-and-concurrency` lets a thread
//               block — and one that reads memory on a worker.
// AUDIO THREAD  `AudioServer::render`, draining each stream's ring through its gain envelope.
//
// ================================================================================================
// THE BUFFER GUARANTEE, STATED SO A TEST CAN HOLD IT
// ================================================================================================
//
// A refill fills the ring to capacity (up to `StreamPlayerConfig::max_chunks` chunks). So a stream
// whose ring holds R frames does not underrun as long as no refill is delayed by more than R frames
// of playback minus one mixer block — however starved the job system is otherwise.
// integration.audio_streaming starves the executor for exactly that long and requires zero
// underruns, then starves it longer and requires the underrun to be silence plus a diagnostic.
//
// ================================================================================================
// A CROSSFADE IS TWO ENVELOPES ON THE AUDIO THREAD
// ================================================================================================
//
// `crossfade` starts the new track at zero gain and gives both streams an equal-power ramp of the
// same length starting in the same mixer block, so the curve is sample-accurate and the sum of the
// squared gains is one throughout. The outgoing stream stops itself at the end of its ramp.

#include <cy/core/assets/file.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/servers/audio/cooked_audio.h>
#include <cy/servers/audio/decoder.h>
#include <cy/servers/audio/server.h>
#include <cy/servers/audio/stream.h>

#include <atomic>

namespace cy::jobs {
class JobSystem;
class AsyncService;
}  // namespace cy::jobs

namespace cy::audio {

/// Where refills run. See the file comment.
class RefillExecutor {
public:
    using Work = void (*)(void* user) noexcept;

    RefillExecutor() = default;
    virtual ~RefillExecutor() = default;

    RefillExecutor(const RefillExecutor&) = delete;
    RefillExecutor& operator=(const RefillExecutor&) = delete;
    RefillExecutor(RefillExecutor&&) = delete;
    RefillExecutor& operator=(RefillExecutor&&) = delete;

    /// Run `work(user)` once, somewhere. `reads_storage` says the work may block on I/O.
    [[nodiscard]] virtual Status submit(Work work, void* user, bool reads_storage) noexcept = 0;
};

/// Runs a refill at once on the calling thread. What a tool and a headless test use.
class InlineRefillExecutor final : public RefillExecutor {
public:
    [[nodiscard]] Status submit(Work work, void* user, bool reads_storage) noexcept override;
};

/// Runs a refill on the job system: a worker for in-memory sources, the async service's thread for
/// sources that read storage. Both must outlive the executor.
class JobRefillExecutor final : public RefillExecutor {
public:
    JobRefillExecutor(jobs::JobSystem& jobs, jobs::AsyncService& async) noexcept
        : jobs_(&jobs), async_(&async) {}

    [[nodiscard]] Status submit(Work work, void* user, bool reads_storage) noexcept override;

private:
    jobs::JobSystem* jobs_;
    jobs::AsyncService* async_;
};

/// A cooked asset read from a file on disk, by range: what `AudioLoadMode::Streamed` reads. A
/// refill reading it blocks on the disk, so `JobRefillExecutor` runs it on the async service.
class FileByteSource final : public AudioByteSource {
public:
    [[nodiscard]] Status open(const char* path) noexcept;

    [[nodiscard]] u64 size() const noexcept override { return size_; }
    [[nodiscard]] Expected<usize, Error> read(u64 offset, Span<u8> destination) noexcept override;

private:
    assets::File file_;
    u64 size_ = 0;
};

/// A cooked track a player can stream: its header, and the bytes of the whole cooked asset (header
/// included) — in memory for `AudioLoadMode::Compressed` or `Decoded`, or a file or package range
/// for `Streamed`. Borrowed: the source outlives every stream of the track.
struct TrackSource {
    CookedAudioHeader header;
    AudioByteSource* asset = nullptr;
};

/// Parse a cooked asset's header from its source and describe it as a track.
[[nodiscard]] Expected<TrackSource, Error> track_from_cooked(AudioByteSource& asset) noexcept;

/// How a player sizes its streams and refills: the knobs the buffer guarantee is stated in.
struct StreamPlayerConfig {
    /// Streams at once. Music, its crossfade partner and a few ambience beds.
    u32 capacity = 6;
    /// Ring buffer per stream, in seconds of output. The buffer guarantee above is stated in it.
    f32 ring_seconds = 0.5F;
    /// The ring holds this fraction before a stream first sounds, and again after a seek.
    f32 prime_fraction = 0.5F;
    /// Source frames decoded per step of a refill.
    u32 chunk_frames = 2048;
    /// Steps per refill at most: the bound on one job's work. Enough to fill an empty ring.
    u32 max_chunks = 32;
    /// The codecs streams are decoded with. Null: `builtin_audio_codecs()`, PCM only.
    AudioCodecs* codecs = nullptr;
};

/// Identifies one playing stream. Zero is none; a stopped stream's id is never reused.
struct StreamId {
    u64 value = 0;
    [[nodiscard]] bool is_null() const noexcept { return value == 0; }
    friend bool operator==(StreamId a, StreamId b) noexcept { return a.value == b.value; }
    friend bool operator!=(StreamId a, StreamId b) noexcept { return a.value != b.value; }
};

/// How one stream of a track starts: where it routes, how loud, how it fades in and from where.
struct StreamPlayOptions {
    /// Null: Master.
    BusHandle bus;
    f32 volume = 1.0F;
    /// Zero starts at full volume as soon as the ring is primed.
    f32 fade_in_seconds = 0.0F;
    /// Where to start, in seconds of the track.
    f64 start_seconds = 0.0;
    /// Apply the track's measured normalisation gain (`CookedAudioHeader::gain_db`).
    bool normalise = true;
};

/// What the player has to say about a stream, kept until `clear_diagnostics`.
struct StreamDiagnostic {
    /// "audio.stream.underrun", "audio.stream.error".
    const char* code = "";
    Name track;
    StreamId stream;
    /// Underrun: frames of silence since the last report. Error: zero.
    u64 frames = 0;
    /// Error: the failure's message.
    const char* message = "";
};

/// One stream's figures, for a debug view and for tests.
struct StreamReport {
    bool live = false;
    Name track;
    StreamState state = StreamState::Idle;
    /// Where the listener is in the track, in seconds, as of the last block mixed.
    f64 position_seconds = 0.0;
    f32 gain = 0.0F;
    u32 underruns = 0;
    u64 underrun_frames = 0;
    /// The ring, and everything the decode side holds: decoder, resampler, staging.
    usize ring_bytes = 0;
    usize decoder_bytes = 0;
    /// Refills run, and the frames they decoded.
    u64 refills = 0;
};

/// Streams cooked tracks into an `AudioServer`. Game thread only. See the file comment.
class StreamPlayer {
public:
    StreamPlayer(AudioServer& server, RefillExecutor& executor, Allocator& allocator) noexcept;
    ~StreamPlayer();

    StreamPlayer(const StreamPlayer&) = delete;
    StreamPlayer& operator=(const StreamPlayer&) = delete;
    StreamPlayer(StreamPlayer&&) = delete;
    StreamPlayer& operator=(StreamPlayer&&) = delete;

    /// Size the slots. Call once, after the server is initialised.
    [[nodiscard]] Status configure(const StreamPlayerConfig& config) noexcept;

    /// Name a track. Naming it again points the name at the new source.
    [[nodiscard]] Status add_track(Name name, const TrackSource& track) noexcept;
    [[nodiscard]] const TrackSource* track(Name name) const noexcept;

    [[nodiscard]] Expected<StreamId, Error> play(Name name,
                                                 const StreamPlayOptions& options) noexcept;
    /// Fade out over `fade_seconds` (zero: within the block) and release.
    [[nodiscard]] Status stop(StreamId stream, f32 fade_seconds) noexcept;
    /// Start the track `name` and fade it in while `from` fades out, both over `seconds`,
    /// equal-power. A null or finished `from` is a plain fade-in.
    [[nodiscard]] Expected<StreamId, Error> crossfade(StreamId from, Name name, f32 seconds,
                                                      const StreamPlayOptions& options) noexcept;
    /// Ramp the stream's volume.
    [[nodiscard]] Status set_volume(StreamId stream, f32 volume, f32 fade_seconds) noexcept;
    /// Jump to `seconds` in the track: the current sound fades out over one block, the ring is
    /// refilled from the new position, and the stream sounds again once primed.
    [[nodiscard]] Status seek(StreamId stream, f64 seconds) noexcept;

    /// Once per frame, game thread. See the file comment.
    void pump() noexcept;

    [[nodiscard]] bool playing(StreamId stream) const noexcept;
    [[nodiscard]] StreamReport report(StreamId stream) const noexcept;
    [[nodiscard]] Span<const StreamDiagnostic> diagnostics() const noexcept {
        return diagnostics_.span();
    }
    void clear_diagnostics() noexcept { diagnostics_.clear(); }
    /// Refills waiting on, or running in, the executor.
    [[nodiscard]] u32 refills_in_flight() const noexcept;
    [[nodiscard]] const StreamPlayerConfig& configuration() const noexcept { return config_; }

private:
    struct Instance;

    [[nodiscard]] Instance* find(StreamId stream) noexcept;
    [[nodiscard]] const Instance* find(StreamId stream) const noexcept;
    [[nodiscard]] Expected<Instance*, Error> acquire(Name name) noexcept;
    void release(Instance& instance) noexcept;
    void pump_instance(Instance& instance) noexcept;
    void complete_seek(Instance& instance) noexcept;
    void report_underruns(Instance& instance) noexcept;
    void schedule(Instance& instance) noexcept;
    void record(const char* code, const Instance& instance, u64 frames,
                const char* message) noexcept;
    static void refill(void* user) noexcept;

    AudioServer* server_;
    RefillExecutor* executor_;
    Allocator* allocator_;
    StreamPlayerConfig config_;
    Array<Instance*> instances_;
    struct NamedTrack {
        Name name;
        TrackSource source;
    };
    Array<NamedTrack> tracks_;
    Array<StreamDiagnostic> diagnostics_;
    u64 next_id_ = 1;
};

}  // namespace cy::audio

#endif  // CY_AUDIO_STREAMING_H
