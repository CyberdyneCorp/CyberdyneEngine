// SPDX-License-Identifier: MIT
// The stream player. See cy/audio/streaming.h.

#include <cy/audio/streaming.h>

#include <cy/core/jobs/async.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/math/scalar.h>
#include <cy/core/memory/ownership.h>

#include <cmath>
#include <cstring>

namespace cy::audio {
namespace {

[[nodiscard]] u32 seconds_to_frames(f64 seconds, u32 rate) noexcept {
    return (seconds <= 0.0) ? 0U : static_cast<u32>(std::lround(seconds * rate));
}

/// What a refill job is handed through the job system: the work and its argument.
struct JobWork {
    RefillExecutor::Work work = nullptr;
    void* user = nullptr;
};

void run_job_work(const jobs::TaskContext& context, void* user) noexcept {
    (void)user;
    JobWork work{};
    std::memcpy(&work, context.data, sizeof(work));
    work.work(work.user);
}

}  // namespace

// --- Executors -----------------------------------------------------------------------------------

Status InlineRefillExecutor::submit(Work work, void* user, bool reads_storage) noexcept {
    (void)reads_storage;
    work(user);
    return ok();
}

Status JobRefillExecutor::submit(Work work, void* user, bool reads_storage) noexcept {
    if (reads_storage) {
        // The one thread where blocking is legal: a refill that reads a package waits on the disk
        // here rather than on a worker the rest of the frame is waiting for.
        Expected<jobs::JobHandle, Error> handle =
            async_->submit_blocking(work, user, "audio.stream.refill");
        return handle ? ok() : Status(make_unexpected(handle.error()));
    }
    const JobWork payload{work, user};
    jobs::JobDesc desc;
    desc.body = &run_job_work;
    desc.name = "audio.stream.refill";
    desc.inline_data = &payload;
    desc.inline_size = static_cast<u32>(sizeof(payload));
    Expected<jobs::JobHandle, Error> handle = jobs_->submit(desc);
    return handle ? ok() : Status(make_unexpected(handle.error()));
}

// --- Tracks --------------------------------------------------------------------------------------

Status FileByteSource::open(const char* path) noexcept {
    Expected<assets::File, Error> opened = assets::File::open(path, assets::FileMode::Read);
    if (!opened) {
        return make_unexpected(opened.error());
    }
    const Expected<u64, Error> size = opened->size();
    if (!size) {
        return make_unexpected(size.error());
    }
    file_ = static_cast<assets::File&&>(*opened);
    size_ = *size;
    return ok();
}

Expected<usize, Error> FileByteSource::read(u64 offset, Span<u8> destination) noexcept {
    if (offset >= size_) {
        return usize{0};
    }
    const auto wanted = static_cast<usize>(math::min<u64>(destination.size(), size_ - offset));
    return file_.read_at(offset, destination.data(), wanted);
}

Expected<TrackSource, Error> track_from_cooked(AudioByteSource& asset) noexcept {
    u8 header[kCookedAudioHeaderBytes] = {};
    const Expected<usize, Error> read = asset.read(0, Span<u8>(header, sizeof(header)));
    if (!read) {
        return make_unexpected(read.error());
    }
    Expected<CookedAudioHeader, Error> parsed =
        parse_cooked_audio_header(Span<const u8>(header, *read));
    if (!parsed) {
        return make_unexpected(parsed.error());
    }
    if (parsed->payload_offset + parsed->payload_size > asset.size()) {
        return fail(ErrorCode::OutOfRange, "a cooked track's payload runs past its source");
    }
    TrackSource track;
    track.header = *parsed;
    track.asset = &asset;
    return track;
}

// --- The instance --------------------------------------------------------------------------------

/// One stream. The game thread owns every field except the job's, which it writes only while no
/// refill is in flight and reads only after `in_flight` drops with release.
struct StreamPlayer::Instance {
    explicit Instance(Allocator& allocator) noexcept : ring(allocator), source(allocator) {}

    StreamRing ring;
    StreamSource source;
    RangeByteSource payload;
    StreamPlayer* owner = nullptr;

    // --- Game thread.
    bool live = false;
    StreamId id;
    Name name;
    TrackSource track;
    StreamHandle handle;
    /// Waiting for the ring to prime before the server starts it: the gain to reach, the fade, and
    /// the stream to fade out in the same block (a crossfade's outgoing half).
    bool start_pending = false;
    f32 start_volume = 1.0F;
    u32 start_fade_frames = 0;
    u32 prime_frames = 0;
    StreamId partner;
    f32 volume = 1.0F;
    bool stopping = false;
    bool seek_pending = false;
    /// A seek before the stream started empties the ring itself rather than asking the mixer to.
    bool restart_ring = false;
    u64 seek_target = 0;
    u32 flush_target = 0;
    /// The output frame the ring's `base_read`th frame read is, for the position.
    u64 base_output = 0;
    u64 base_read = 0;
    u32 reported_underruns = 0;
    bool error_reported = false;

    // --- Written by the game thread before a submit, read by the job.
    bool needs_open = true;
    bool seek_on_refill = false;
    u64 seek_frame = 0;
    AudioCodecs* codecs = nullptr;
    StreamFormat format;
    u32 max_chunks = 1;

    // --- Written by the job, read by the game thread after `in_flight` drops.
    bool failed = false;
    Error error;
    u64 refills = 0;
    std::atomic<u32> in_flight{0};
};

void StreamPlayer::refill(void* user) noexcept {
    auto& instance = *static_cast<Instance*>(user);
    const auto fail_with = [&](const Error& error) {
        instance.failed = true;
        instance.error = error;
        instance.ring.mark_end();
    };
    if (instance.needs_open) {
        Expected<DecoderPtr, Error> decoder =
            open_cooked_audio(*instance.codecs, instance.track.header, *instance.track.asset,
                              instance.payload, *instance.owner->allocator_);
        Status opened =
            decoder ? instance.source.open(static_cast<DecoderPtr&&>(*decoder),
                                           stream_timing(instance.track.header), instance.format)
                    : Status(make_unexpected(decoder.error()));
        if (!opened) {
            fail_with(opened.error());
            instance.in_flight.store(0, std::memory_order_release);
            return;
        }
        instance.needs_open = false;
    }
    if (instance.seek_on_refill) {
        if (Status sought = instance.source.seek(instance.seek_frame); !sought) {
            fail_with(sought.error());
            instance.in_flight.store(0, std::memory_order_release);
            return;
        }
        instance.seek_on_refill = false;
    }
    const Expected<u32, Error> filled = instance.source.fill(instance.ring, instance.max_chunks);
    if (!filled) {
        fail_with(filled.error());
    }
    ++instance.refills;
    instance.in_flight.store(0, std::memory_order_release);
}

// --- The player ----------------------------------------------------------------------------------

StreamPlayer::StreamPlayer(AudioServer& server, RefillExecutor& executor,
                           Allocator& allocator) noexcept
    : server_(&server),
      executor_(&executor),
      allocator_(&allocator),
      instances_(allocator),
      tracks_(allocator),
      diagnostics_(allocator) {}

StreamPlayer::~StreamPlayer() {
    for (Instance* instance : instances_) {
        // A refill still running owns the instance's decoder until it returns. The executor's
        // jobs are the caller's to have finished — the server and executor outlive this player —
        // so by the time this destructor runs nothing should be in flight; if something is, leaking
        // it is the only safe choice.
        if (instance->in_flight.load(std::memory_order_acquire) != 0) {
            continue;
        }
        if (instance->live) {
            server_->destroy_stream(instance->handle);
        }
        instance->~Instance();
        allocator_->deallocate(instance, sizeof(Instance), alignof(Instance));
    }
}

Status StreamPlayer::configure(const StreamPlayerConfig& config) noexcept {
    if (!instances_.empty()) {
        return fail(ErrorCode::AlreadyExists, "a stream player is configured once");
    }
    if (config.capacity == 0 || config.ring_seconds <= 0.0F || config.chunk_frames == 0 ||
        config.max_chunks == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a stream player needs streams, a ring and non-empty refills");
    }
    const AudioFormat format = server_->format();
    if (format.sample_rate == 0) {
        return fail(ErrorCode::Unavailable, "configure a stream player after its audio server");
    }
    config_ = config;
    if (config_.codecs == nullptr) {
        config_.codecs = &builtin_audio_codecs();
    }
    const u32 ring_frames =
        math::max(seconds_to_frames(static_cast<f64>(config.ring_seconds), format.sample_rate), 1U);
    if (Status reserved = instances_.reserve(config.capacity); !reserved) {
        return reserved;
    }
    for (u32 index = 0; index < config.capacity; ++index) {
        void* storage = allocator_->allocate(sizeof(Instance), alignof(Instance));
        if (storage == nullptr) {
            return fail(ErrorCode::OutOfMemory, "no memory for a stream");
        }
        auto* instance = construct_at<Instance>(storage, *allocator_);
        (void)instances_.push_back(instance);
        instance->owner = this;
        if (Status sized = instance->ring.allocate(ring_frames, format.channels()); !sized) {
            return sized;
        }
    }
    return ok();
}

Status StreamPlayer::add_track(Name name, const TrackSource& track) noexcept {
    if (track.asset == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a track needs a source");
    }
    if (Status valid = validate_cooked_audio_header(track.header); !valid) {
        return valid;
    }
    for (NamedTrack& named : tracks_) {
        if (named.name == name) {
            named.source = track;
            return ok();
        }
    }
    return tracks_.push_back(NamedTrack{name, track});
}

const TrackSource* StreamPlayer::track(Name name) const noexcept {
    for (const NamedTrack& named : tracks_) {
        if (named.name == name) {
            return &named.source;
        }
    }
    return nullptr;
}

StreamPlayer::Instance* StreamPlayer::find(StreamId stream) noexcept {
    for (Instance* instance : instances_) {
        if (instance->live && instance->id == stream) {
            return instance;
        }
    }
    return nullptr;
}

const StreamPlayer::Instance* StreamPlayer::find(StreamId stream) const noexcept {
    for (const Instance* instance : instances_) {
        if (instance->live && instance->id == stream) {
            return instance;
        }
    }
    return nullptr;
}

Expected<StreamPlayer::Instance*, Error> StreamPlayer::acquire(Name name) noexcept {
    const TrackSource* source = track(name);
    if (source == nullptr) {
        return fail(ErrorCode::NotFound, "no track of that name");
    }
    if (!config_.codecs->supports(source->header.codec)) {
        return fail(ErrorCode::Unavailable, "this build cannot decode the track's codec");
    }
    for (Instance* instance : instances_) {
        if (instance->live || instance->in_flight.load(std::memory_order_acquire) != 0) {
            continue;
        }
        const AudioFormat format = server_->format();
        instance->ring.reset();
        instance->source.close();
        instance->live = true;
        instance->id = StreamId{next_id_++};
        instance->name = name;
        instance->track = *source;
        instance->handle = StreamHandle{};
        instance->start_pending = false;
        instance->partner = StreamId{};
        instance->stopping = false;
        instance->seek_pending = false;
        instance->restart_ring = false;
        instance->base_output = 0;
        instance->base_read = 0;
        instance->reported_underruns = 0;
        instance->error_reported = false;
        instance->needs_open = true;
        instance->seek_on_refill = false;
        instance->codecs = config_.codecs;
        instance->format =
            StreamFormat{format.sample_rate, format.channels(), config_.chunk_frames};
        instance->max_chunks = config_.max_chunks;
        instance->failed = false;
        instance->refills = 0;
        return instance;
    }
    return fail(ErrorCode::OutOfRange, "every stream slot is playing");
}

void StreamPlayer::release(Instance& instance) noexcept {
    if (instance.handle) {
        server_->destroy_stream(instance.handle);
    }
    instance.handle = StreamHandle{};
    instance.source.close();
    instance.live = false;
}

Expected<StreamId, Error> StreamPlayer::play(Name name, const StreamPlayOptions& options) noexcept {
    Expected<Instance*, Error> acquired = acquire(name);
    if (!acquired) {
        return make_unexpected(acquired.error());
    }
    Instance& instance = **acquired;
    const CookedAudioHeader& header = instance.track.header;
    instance.volume =
        math::max(options.volume, 0.0F) * (options.normalise ? header.gain_linear() : 1.0F);

    StreamDescription description;
    description.ring = &instance.ring;
    description.bus = options.bus;
    description.volume = instance.volume;
    description.prime_frames = static_cast<u32>(static_cast<f32>(instance.ring.capacity_frames()) *
                                                math::clamp(config_.prime_fraction, 0.0F, 1.0F));
    instance.prime_frames = description.prime_frames;
    Expected<StreamHandle, Error> handle = server_->create_stream(description);
    if (!handle) {
        instance.live = false;
        return make_unexpected(handle.error());
    }
    instance.handle = *handle;
    instance.start_pending = true;
    instance.start_volume = instance.volume;
    instance.start_fade_frames =
        seconds_to_frames(static_cast<f64>(options.fade_in_seconds), server_->format().sample_rate);

    if (options.start_seconds > 0.0) {
        const auto frame = static_cast<u64>(options.start_seconds * header.sample_rate);
        if (frame >= header.frame_count) {
            release(instance);
            return fail(ErrorCode::OutOfRange, "a stream starting past the end of its track");
        }
        instance.seek_on_refill = true;
        instance.seek_frame = frame;
        instance.base_output =
            Resampler::convert_frame(frame, header.sample_rate, server_->format().sample_rate);
    }
    schedule(instance);
    pump_instance(instance);
    return instance.id;
}

Expected<StreamId, Error> StreamPlayer::crossfade(StreamId from, Name name, f32 seconds,
                                                  const StreamPlayOptions& options) noexcept {
    StreamPlayOptions incoming = options;
    incoming.fade_in_seconds = seconds;
    Expected<StreamId, Error> started = play(name, incoming);
    if (!started) {
        return started;
    }
    Instance* outgoing = find(from);
    Instance* instance = find(*started);
    if (outgoing != nullptr && !outgoing->stopping && instance != nullptr) {
        if (instance->start_pending) {
            // Started in the same block as the incoming stream, below; see the file comment.
            instance->partner = from;
        } else {
            (void)server_->fade_stream(outgoing->handle, 0.0F, seconds, FadeCurve::EqualPower,
                                       true);
        }
        outgoing->stopping = true;
    }
    return started;
}

Status StreamPlayer::stop(StreamId stream, f32 fade_seconds) noexcept {
    Instance* instance = find(stream);
    if (instance == nullptr) {
        return ok();  // already gone, which is what a stop wants
    }
    instance->stopping = true;
    if (instance->start_pending) {
        instance->start_pending = false;
        return server_->stop_stream(instance->handle, 0.0F);
    }
    return server_->stop_stream(instance->handle, fade_seconds);
}

Status StreamPlayer::set_volume(StreamId stream, f32 volume, f32 fade_seconds) noexcept {
    Instance* instance = find(stream);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such stream");
    }
    instance->volume =
        math::max(volume, 0.0F) *
        (instance->track.header.gain_db != 0.0F ? instance->track.header.gain_linear() : 1.0F);
    if (instance->start_pending) {
        instance->start_volume = instance->volume;
        return ok();
    }
    return server_->fade_stream(instance->handle, instance->volume, fade_seconds, FadeCurve::Linear,
                                false);
}

Status StreamPlayer::seek(StreamId stream, f64 seconds) noexcept {
    Instance* instance = find(stream);
    if (instance == nullptr) {
        return fail(ErrorCode::NotFound, "no such stream");
    }
    const CookedAudioHeader& header = instance->track.header;
    const auto frame = static_cast<u64>(math::max(seconds, 0.0) * header.sample_rate);
    if (frame >= header.frame_count) {
        return fail(ErrorCode::OutOfRange, "a seek past the end of the track");
    }
    instance->seek_pending = true;
    instance->seek_target = frame;
    if (instance->start_pending) {
        // Not started: the mixer has never seen the ring, so it is this thread's to empty once no
        // refill is writing to it, with no flush to wait for.
        instance->restart_ring = true;
        instance->flush_target = 0;
        return ok();
    }
    if (Status flushed = server_->flush_stream(instance->handle); !flushed) {
        instance->seek_pending = false;
        return flushed;
    }
    instance->flush_target = server_->stream_status(instance->handle).flushes + 1;
    return ok();
}

void StreamPlayer::pump() noexcept {
    for (Instance* instance : instances_) {
        if (instance->live) {
            pump_instance(*instance);
        }
    }
}

void StreamPlayer::pump_instance(Instance& instance) noexcept {
    if (instance.in_flight.load(std::memory_order_acquire) != 0) {
        report_underruns(instance);
        return;
    }
    if (instance.failed && !instance.error_reported) {
        record("audio.stream.error", instance, 0, instance.error.message);
        instance.error_reported = true;
        instance.stopping = true;
        instance.start_pending = false;
        (void)server_->stop_stream(instance.handle, 0.0F);
    }
    report_underruns(instance);
    // Finished — stopped, faded out, or played to the end of a track that does not loop: the mixer
    // has let go of the ring, so the slot is free.
    if (!instance.start_pending && server_->stream_finished(instance.handle)) {
        release(instance);
        return;
    }
    if (instance.seek_pending) {
        complete_seek(instance);
    }
    if (instance.start_pending && !instance.needs_open &&
        (instance.ring.readable_frames() >= instance.prime_frames || instance.ring.ended())) {
        // Primed: start it, and fade the crossfade partner out in the same block.
        instance.start_pending = false;
        (void)server_->start_stream(instance.handle,
                                    static_cast<f32>(instance.start_fade_frames) /
                                        static_cast<f32>(server_->format().sample_rate),
                                    FadeCurve::EqualPower);
        if (Instance* partner = find(instance.partner); partner != nullptr) {
            (void)server_->fade_stream(partner->handle, 0.0F,
                                       static_cast<f32>(instance.start_fade_frames) /
                                           static_cast<f32>(server_->format().sample_rate),
                                       FadeCurve::EqualPower, true);
        }
    }
    if (!instance.failed && !instance.seek_pending) {
        schedule(instance);
    }
}

void StreamPlayer::complete_seek(Instance& instance) noexcept {
    if (server_->stream_status(instance.handle).flushes < instance.flush_target) {
        return;  // the audio thread has not dropped the ring yet; nothing may write to it
    }
    const CookedAudioHeader& header = instance.track.header;
    if (instance.restart_ring) {
        instance.ring.reset();
        instance.restart_ring = false;
    }
    instance.seek_pending = false;
    instance.seek_on_refill = true;
    instance.seek_frame = instance.seek_target;
    instance.base_output = Resampler::convert_frame(instance.seek_target, header.sample_rate,
                                                    server_->format().sample_rate);
    instance.base_read = instance.ring.frames_read();
}

void StreamPlayer::schedule(Instance& instance) noexcept {
    // A stopping stream is still refilled: a fade-out or the outgoing half of a crossfade needs
    // frames until the mixer says it is done, and starving it would cut the fade to silence.
    if (instance.failed) {
        return;
    }
    const bool due = instance.needs_open || instance.seek_on_refill ||
                     (!instance.ring.ended() &&
                      instance.ring.writable_frames() >=
                          math::min(instance.format.chunk_frames, instance.ring.capacity_frames()));
    if (!due) {
        return;
    }
    instance.in_flight.store(1, std::memory_order_relaxed);
    const bool reads_storage = instance.track.header.load_mode == AudioLoadMode::Streamed;
    if (Status submitted = executor_->submit(&StreamPlayer::refill, &instance, reads_storage);
        !submitted) {
        instance.in_flight.store(0, std::memory_order_relaxed);
        record("audio.stream.error", instance, 0, submitted.error().message);
    }
}

void StreamPlayer::report_underruns(Instance& instance) noexcept {
    if (!instance.handle) {
        return;
    }
    const StreamStatus status = server_->stream_status(instance.handle);
    if (status.underruns == instance.reported_underruns) {
        return;
    }
    instance.reported_underruns = status.underruns;
    record("audio.stream.underrun", instance, status.underrun_frames, "");
}

void StreamPlayer::record(const char* code, const Instance& instance, u64 frames,
                          const char* message) noexcept {
    StreamDiagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.track = instance.name;
    diagnostic.stream = instance.id;
    diagnostic.frames = frames;
    diagnostic.message = message;
    (void)diagnostics_.push_back(diagnostic);
}

bool StreamPlayer::playing(StreamId stream) const noexcept {
    const Instance* instance = find(stream);
    return instance != nullptr && !instance->stopping;
}

u32 StreamPlayer::refills_in_flight() const noexcept {
    u32 count = 0;
    for (const Instance* instance : instances_) {
        count += instance->in_flight.load(std::memory_order_acquire);
    }
    return count;
}

StreamReport StreamPlayer::report(StreamId stream) const noexcept {
    StreamReport report;
    const Instance* instance = find(stream);
    if (instance == nullptr) {
        return report;
    }
    const StreamStatus status = server_->stream_status(instance->handle);
    const CookedAudioHeader& header = instance->track.header;
    const u32 rate = server_->format().sample_rate;
    report.live = true;
    report.track = instance->name;
    report.state = status.state;
    report.gain = status.gain;
    report.underruns = status.underruns;
    report.underrun_frames = status.underrun_frames;
    report.ring_bytes = instance->ring.resident_bytes();
    report.refills =
        (instance->in_flight.load(std::memory_order_acquire) == 0) ? instance->refills : 0;
    if (instance->in_flight.load(std::memory_order_acquire) == 0) {
        report.decoder_bytes = instance->source.resident_bytes();
    }
    u64 played = instance->base_output;
    if (!instance->seek_pending) {
        played += instance->ring.frames_read() - instance->base_read;
    }
    if (header.looping) {
        const u64 loop_start =
            Resampler::convert_frame(header.loop_start, header.sample_rate, rate);
        const u64 loop_end =
            Resampler::convert_frame(header.effective_loop_end(), header.sample_rate, rate);
        if (played >= loop_end && loop_end > loop_start) {
            played = loop_start + ((played - loop_start) % (loop_end - loop_start));
        }
    }
    report.position_seconds = static_cast<f64>(played) / static_cast<f64>(rate);
    return report;
}

}  // namespace cy::audio
