// SPDX-License-Identifier: MIT
// The audio server's streams: creation and commands on the game thread, the drain from each ring
// on the audio thread. Issue #87. See cy/servers/audio/server.h and stream.h.

#include <cy/servers/audio/server.h>

#include <cy/core/diagnostics/field.h>
#include <cy/core/diagnostics/log.h>
#include <cy/core/math/scalar.h>

#include <cmath>
#include <cstring>

namespace cy::audio {
namespace {

CY_TRACE_FIELD(stream_index, u64, cy::Privacy::Public)
CY_TRACE_FIELD(underrun_frames, u64, cy::Privacy::Public)
CY_TRACE_FIELD(underrun_blocks, u64, cy::Privacy::Public)
CY_LOG_CATEGORY(log_category, "audio")

[[nodiscard]] u32 float_bits(f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

[[nodiscard]] f32 bits_float(u32 bits) noexcept {
    f32 value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[nodiscard]] u32 seconds_to_frames(f32 seconds, u32 rate) noexcept {
    if (seconds <= 0.0F) {
        return 0;
    }
    return static_cast<u32>(std::lround(static_cast<f64>(seconds) * rate));
}

}  // namespace

const char* stream_state_name(StreamState state) noexcept {
    switch (state) {
        case StreamState::Idle:
            return "idle";
        case StreamState::Priming:
            return "priming";
        case StreamState::Playing:
            return "playing";
        case StreamState::Flushing:
            return "flushing";
        case StreamState::Stopped:
            return "stopped";
        case StreamState::Count:
            break;
    }
    return "unknown";
}

AudioServer::StreamSlot::StreamSlot(StreamSlot&& other) noexcept
    : generation(other.generation),
      live(other.live),
      pending_release(other.pending_release),
      volume(other.volume),
      reported_underruns(other.reported_underruns),
      ring(other.ring),
      bus_index(other.bus_index),
      prime_frames(other.prime_frames),
      active_ring(other.active_ring),
      state(other.state),
      gain(other.gain),
      envelope(other.envelope),
      declick(other.declick),
      published_state(other.published_state.load(std::memory_order_relaxed)),
      frames_mixed(other.frames_mixed.load(std::memory_order_relaxed)),
      underruns(other.underruns.load(std::memory_order_relaxed)),
      underrun_frames(other.underrun_frames.load(std::memory_order_relaxed)),
      flushes(other.flushes.load(std::memory_order_relaxed)),
      published_gain(other.published_gain.load(std::memory_order_relaxed)),
      finished(other.finished.load(std::memory_order_relaxed)) {}

AudioServer::StreamSlot& AudioServer::StreamSlot::operator=(StreamSlot&& other) noexcept {
    if (this != &other) {
        this->~StreamSlot();
        construct_at<StreamSlot>(this, static_cast<StreamSlot&&>(other));
    }
    return *this;
}

// --- Game thread ---------------------------------------------------------------------------------

AudioServer::StreamSlot* AudioServer::stream_slot(StreamHandle handle) noexcept {
    const u32 index = handle.index();
    if (handle.is_null() || index >= streams_.size()) {
        return nullptr;
    }
    StreamSlot& slot = streams_[index];
    return (slot.live && slot.generation == handle.generation()) ? &slot : nullptr;
}

const AudioServer::StreamSlot* AudioServer::stream_slot(StreamHandle handle) const noexcept {
    const u32 index = handle.index();
    if (handle.is_null() || index >= streams_.size()) {
        return nullptr;
    }
    const StreamSlot& slot = streams_[index];
    return (slot.live && slot.generation == handle.generation()) ? &slot : nullptr;
}

Expected<StreamHandle, Error> AudioServer::create_stream(
    const StreamDescription& description) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "the audio server is not initialized");
    }
    if (description.ring == nullptr || description.ring->capacity_frames() == 0) {
        return fail(ErrorCode::InvalidArgument, "a stream needs an allocated ring");
    }
    if (description.ring->channels() != format_.channels()) {
        return fail(ErrorCode::InvalidArgument,
                    "a stream's ring holds frames at the device's layout; the stream source "
                    "converts to it");
    }
    const BusHandle bus = description.bus.is_null() ? buses_.master() : description.bus;
    if (!buses_.alive(bus)) {
        return fail(ErrorCode::NotFound, "a stream names a bus the graph does not hold");
    }
    for (usize i = 0; i < streams_.size(); ++i) {
        StreamSlot& slot = streams_[i];
        if (slot.live || slot.pending_release) {
            continue;
        }
        ++slot.generation;
        slot.live = true;
        slot.volume = math::max(description.volume, 0.0F);
        slot.reported_underruns = 0;
        slot.ring = description.ring;
        slot.bus_index = buses_.index_of(bus);
        slot.prime_frames =
            math::min(description.prime_frames, description.ring->capacity_frames());
        // `state`, `gain`, `envelope`, `declick` and `active_ring` are the audio thread's: the
        // previous stream on this slot finished (or was never started), so they are at rest and
        // the Start command sets them.
        slot.published_state.store(static_cast<u32>(StreamState::Idle), std::memory_order_relaxed);
        slot.frames_mixed.store(0, std::memory_order_relaxed);
        slot.underruns.store(0, std::memory_order_relaxed);
        slot.underrun_frames.store(0, std::memory_order_relaxed);
        slot.flushes.store(0, std::memory_order_relaxed);
        slot.published_gain.store(0, std::memory_order_relaxed);
        slot.finished.store(0, std::memory_order_release);
        return StreamHandle::from_slot(static_cast<u32>(i), slot.generation);
    }
    return fail(ErrorCode::OutOfRange, "the audio server has no free stream slots");
}

Status AudioServer::start_stream(StreamHandle handle, f32 fade_in_seconds,
                                 FadeCurve curve) noexcept {
    StreamSlot* slot = stream_slot(handle);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "no such stream");
    }
    AudioCommand command;
    command.kind = CommandKind::StreamStart;
    command.stream = handle;
    command.scalar = slot->volume;
    command.frames = seconds_to_frames(fade_in_seconds, format_.sample_rate);
    command.curve = static_cast<u8>(curve);
    return enqueue(command) ? ok() : fail(ErrorCode::OutOfRange, "the audio command queue is full");
}

Status AudioServer::fade_stream(StreamHandle handle, f32 gain, f32 seconds, FadeCurve curve,
                                bool stop_when_done) noexcept {
    if (stream_slot(handle) == nullptr) {
        return fail(ErrorCode::NotFound, "no such stream");
    }
    AudioCommand command;
    command.kind = CommandKind::StreamFade;
    command.stream = handle;
    command.scalar = math::max(gain, 0.0F);
    command.frames = seconds_to_frames(seconds, format_.sample_rate);
    command.curve = static_cast<u8>(curve);
    command.flag = stop_when_done ? 1 : 0;
    return enqueue(command) ? ok() : fail(ErrorCode::OutOfRange, "the audio command queue is full");
}

Status AudioServer::stop_stream(StreamHandle handle, f32 fade_seconds) noexcept {
    if (fade_seconds > 0.0F) {
        return fade_stream(handle, 0.0F, fade_seconds, FadeCurve::EqualPower, true);
    }
    if (stream_slot(handle) == nullptr) {
        return fail(ErrorCode::NotFound, "no such stream");
    }
    AudioCommand command;
    command.kind = CommandKind::StreamStop;
    command.stream = handle;
    return enqueue(command) ? ok() : fail(ErrorCode::OutOfRange, "the audio command queue is full");
}

Status AudioServer::flush_stream(StreamHandle handle) noexcept {
    if (stream_slot(handle) == nullptr) {
        return fail(ErrorCode::NotFound, "no such stream");
    }
    AudioCommand command;
    command.kind = CommandKind::StreamFlush;
    command.stream = handle;
    return enqueue(command) ? ok() : fail(ErrorCode::OutOfRange, "the audio command queue is full");
}

bool AudioServer::stream_finished(StreamHandle handle) const noexcept {
    const StreamSlot* slot = stream_slot(handle);
    return slot != nullptr && slot->finished.load(std::memory_order_acquire) != 0;
}

StreamStatus AudioServer::stream_status(StreamHandle handle) const noexcept {
    StreamStatus status;
    const StreamSlot* slot = stream_slot(handle);
    if (slot == nullptr) {
        status.state = StreamState::Stopped;
        return status;
    }
    status.state = static_cast<StreamState>(slot->published_state.load(std::memory_order_acquire));
    status.frames_mixed = slot->frames_mixed.load(std::memory_order_relaxed);
    status.underruns = slot->underruns.load(std::memory_order_relaxed);
    status.underrun_frames = slot->underrun_frames.load(std::memory_order_relaxed);
    status.flushes = slot->flushes.load(std::memory_order_acquire);
    status.gain = bits_float(slot->published_gain.load(std::memory_order_relaxed));
    return status;
}

void AudioServer::destroy_stream(StreamHandle handle) noexcept {
    StreamSlot* slot = stream_slot(handle);
    if (slot == nullptr) {
        return;
    }
    if (slot->finished.load(std::memory_order_acquire) == 0) {
        // STOP FIRST, RELEASE LATER — `destroy_clip()`'s argument, for a ring instead of samples.
        AudioCommand command;
        command.kind = CommandKind::StreamStop;
        command.stream = handle;
        (void)enqueue(command);
        slot->pending_release = true;
    }
    slot->live = false;
}

void AudioServer::update_streams() noexcept {
    for (usize i = 0; i < streams_.size(); ++i) {
        StreamSlot& slot = streams_[i];
        if (slot.pending_release && slot.finished.load(std::memory_order_acquire) != 0) {
            slot.pending_release = false;
        }
        if (!slot.live) {
            continue;
        }
        // THE DIAGNOSTIC HALF OF "silence and a diagnostic". The audio thread can only count; the
        // game thread is where a count becomes a record somebody reads.
        const u32 underruns = slot.underruns.load(std::memory_order_relaxed);
        if (underruns == slot.reported_underruns) {
            continue;
        }
        const u64 frames = slot.underrun_frames.load(std::memory_order_relaxed);
        statistics_.stream_underruns += underruns - slot.reported_underruns;
        u64 total = 0;
        for (const StreamSlot& other : streams_) {
            total += other.underrun_frames.load(std::memory_order_relaxed);
        }
        statistics_.stream_underrun_frames = math::max(statistics_.stream_underrun_frames, total);
        CY_LOG(log_category(), diag::LogLevel::Warning, "audio.stream.underrun",
               diag::field_u64(stream_index(), i),
               diag::field_u64(underrun_blocks(), underruns - slot.reported_underruns),
               diag::field_u64(underrun_frames(), frames));
        slot.reported_underruns = underruns;
    }
}

// --- Audio thread --------------------------------------------------------------------------------

void AudioServer::apply_stream_command(const AudioCommand& command) noexcept {
    const u32 index = command.stream.index();
    if (index >= streams_.size()) {
        return;
    }
    StreamSlot& slot = streams_[index];
    if (slot.generation != command.stream.generation()) {
        return;  // a stream that was created again since; the command is for the old one
    }
    switch (command.kind) {
        case CommandKind::StreamStart:
            slot.active_ring = slot.ring;  // published by this command's release/acquire
            slot.declick = false;
            slot.state = StreamState::Priming;
            slot.gain = (command.frames == 0) ? command.scalar : 0.0F;
            slot.envelope = StreamEnvelope{
                0.0F, command.scalar, command.frames, 0, static_cast<FadeCurve>(command.curve),
                false};
            break;
        case CommandKind::StreamFade:
            if (command.frames == 0) {
                slot.gain = command.scalar;
                slot.envelope = StreamEnvelope{};
                if (command.flag != 0) {
                    slot.state = StreamState::Stopped;
                }
            } else {
                slot.envelope = StreamEnvelope{slot.gain,
                                               command.scalar,
                                               command.frames,
                                               0,
                                               static_cast<FadeCurve>(command.curve),
                                               command.flag != 0};
            }
            break;
        case CommandKind::StreamFlush:
            if (slot.state == StreamState::Playing) {
                slot.state = StreamState::Flushing;
            } else if (slot.state == StreamState::Priming) {
                // Started and not yet sounding, so there is nothing to fade: drop the ring now.
                slot.active_ring->discard();
                slot.declick = true;
                slot.flushes.fetch_add(1, std::memory_order_release);
            } else {
                // Not started, or stopped: the ring is not this thread's to touch.
                slot.flushes.fetch_add(1, std::memory_order_release);
            }
            break;
        case CommandKind::StreamStop:
            slot.state = StreamState::Stopped;
            break;
        default:
            break;
    }
    if (slot.state == StreamState::Stopped) {
        slot.active_ring = nullptr;
        slot.finished.store(1, std::memory_order_release);
    }
    slot.published_state.store(static_cast<u32>(slot.state), std::memory_order_release);
}

void AudioServer::mix_streams(u32 block_frames, u32 channels) noexcept {
    // Only what the audio thread itself knows: a slot's state and its `active_ring`, both set by
    // commands. The game-thread half of a slot is not read here.
    for (StreamSlot& slot : streams_) {
        const StreamState before = slot.state;
        if (before != StreamState::Priming && before != StreamState::Playing &&
            before != StreamState::Flushing) {
            continue;
        }
        mix_stream(slot, block_frames, channels);
        if (slot.state == StreamState::Stopped) {
            slot.active_ring = nullptr;
            slot.finished.store(1, std::memory_order_release);
        }
        slot.published_state.store(static_cast<u32>(slot.state), std::memory_order_release);
        slot.published_gain.store(float_bits(slot.gain), std::memory_order_relaxed);
    }
}

void AudioServer::mix_stream(StreamSlot& slot, u32 block_frames, u32 channels) noexcept {
    StreamRing& ring = *slot.active_ring;
    bool ramp_in = false;
    if (slot.state == StreamState::Priming) {
        const bool primed = ring.readable_frames() >= math::max(slot.prime_frames, 1U);
        if (!primed && !ring.ended()) {
            return;  // silence, and not an underrun: nothing has started
        }
        slot.state = StreamState::Playing;
        ramp_in = slot.declick;
        slot.declick = false;
    }

    f32* scratch = stream_scratch_.data();
    const u32 got = ring.read(scratch, block_frames);
    bool finishing = false;
    if (got < block_frames) {
        std::memset(static_cast<void*>(scratch + (static_cast<usize>(got) * channels)), 0,
                    static_cast<usize>(block_frames - got) * channels * sizeof(f32));
        if (ring.ended() && ring.readable_frames() == 0) {
            finishing = true;  // the track ran out, which is an end and not an underrun
        } else {
            slot.underruns.fetch_add(1, std::memory_order_relaxed);
            slot.underrun_frames.fetch_add(block_frames - got, std::memory_order_relaxed);
        }
    }

    const bool flushing = slot.state == StreamState::Flushing;
    f32* destination = bus_buffer(slot.bus_index, channels);
    for (u32 frame = 0; frame < block_frames; ++frame) {
        StreamEnvelope& envelope = slot.envelope;
        if (envelope.done < envelope.total) {
            ++envelope.done;
            slot.gain =
                fade_curve_gain(envelope.curve, envelope.from, envelope.to,
                                static_cast<f32>(envelope.done) / static_cast<f32>(envelope.total));
        }
        // A flush fades the old material out across this one block: a seek is a jump, and a jump
        // mid-waveform is a click.
        const f32 step = static_cast<f32>(frame + 1) / static_cast<f32>(block_frames);
        f32 gain = slot.gain;
        if (flushing) {
            gain *= 1.0F - step;
        } else if (ramp_in) {
            gain *= step;
        }
        for (u32 channel = 0; channel < channels; ++channel) {
            const usize at = (static_cast<usize>(frame) * channels) + channel;
            destination[at] += scratch[at] * gain;
        }
    }
    slot.frames_mixed.fetch_add(got, std::memory_order_relaxed);

    if (flushing) {
        ring.discard();
        slot.state = StreamState::Priming;
        slot.declick = true;
        slot.flushes.fetch_add(1, std::memory_order_release);
    }
    const StreamEnvelope& envelope = slot.envelope;
    if (finishing || (envelope.stop_when_done && envelope.done >= envelope.total)) {
        slot.state = StreamState::Stopped;
    }
}

}  // namespace cy::audio
