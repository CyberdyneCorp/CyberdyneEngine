// SPDX-License-Identifier: MIT
// The stream ring and the source that fills it. See cy/servers/audio/stream.h.

#include <cy/servers/audio/stream.h>

#include <cy/core/math/scalar.h>

#include <cmath>
#include <cstring>

namespace cy::audio {
namespace {

constexpr u64 kUnbounded = ~0ULL;

}  // namespace

const char* fade_curve_name(FadeCurve curve) noexcept {
    switch (curve) {
        case FadeCurve::Linear:
            return "linear";
        case FadeCurve::EqualPower:
            return "equal_power";
        case FadeCurve::Count:
            break;
    }
    return "unknown";
}

f32 fade_curve_gain(FadeCurve curve, f32 from, f32 to, f32 t) noexcept {
    if (t <= 0.0F) {
        return from;
    }
    if (t >= 1.0F) {
        return to;  // exactly: cos(pi/2) in float is not zero, and a finished fade-out is silence
    }
    const f32 x = t;
    if (curve != FadeCurve::EqualPower) {
        return from + ((to - from) * x);
    }
    const f32 quarter = 0.5F * math::kPi;
    if (to >= from) {
        return from + ((to - from) * std::sin(x * quarter));
    }
    return to + ((from - to) * std::cos(x * quarter));
}

// --- StreamRing ----------------------------------------------------------------------------------

StreamRing::StreamRing(Allocator& allocator) noexcept : frames_(allocator) {}

Status StreamRing::allocate(u32 capacity_frames, u32 channels) noexcept {
    if (capacity_frames == 0 || channels == 0 || channels > 2) {
        return fail(ErrorCode::InvalidArgument,
                    "a stream ring holds frames of one or two channels");
    }
    if (Status sized = frames_.allocate(static_cast<usize>(capacity_frames) * channels); !sized) {
        return sized;
    }
    capacity_ = capacity_frames;
    channels_ = channels;
    reset();
    return ok();
}

void StreamRing::reset() noexcept {
    written_.store(0, std::memory_order_relaxed);
    read_.store(0, std::memory_order_relaxed);
    end_.store(0, std::memory_order_release);
}

u32 StreamRing::writable_frames() const noexcept {
    // acquire on the reader's count: every frame it released is free to overwrite.
    const u64 read = read_.load(std::memory_order_acquire);
    const u64 written = written_.load(std::memory_order_relaxed);
    return capacity_ - static_cast<u32>(written - read);
}

u32 StreamRing::readable_frames() const noexcept {
    const u64 written = written_.load(std::memory_order_acquire);
    const u64 read = read_.load(std::memory_order_relaxed);
    return static_cast<u32>(written - read);
}

u32 StreamRing::write(const f32* frames, u32 count) noexcept {
    const u32 room = writable_frames();
    const u32 take = math::min(room, count);
    const u64 written = written_.load(std::memory_order_relaxed);
    u32 copied = 0;
    while (copied < take) {
        const auto slot = static_cast<u32>((written + copied) % capacity_);
        const u32 run = math::min(take - copied, capacity_ - slot);
        std::memcpy(frames_.data() + (static_cast<usize>(slot) * channels_),
                    frames + (static_cast<usize>(copied) * channels_),
                    static_cast<usize>(run) * channels_ * sizeof(f32));
        copied += run;
    }
    // release: the frames are visible before the count that exposes them.
    written_.store(written + take, std::memory_order_release);
    return take;
}

u32 StreamRing::read(f32* out, u32 count) noexcept {
    const u32 available = readable_frames();
    const u32 take = math::min(available, count);
    const u64 read = read_.load(std::memory_order_relaxed);
    u32 copied = 0;
    while (copied < take) {
        const auto slot = static_cast<u32>((read + copied) % capacity_);
        const u32 run = math::min(take - copied, capacity_ - slot);
        std::memcpy(out + (static_cast<usize>(copied) * channels_),
                    frames_.data() + (static_cast<usize>(slot) * channels_),
                    static_cast<usize>(run) * channels_ * sizeof(f32));
        copied += run;
    }
    read_.store(read + take, std::memory_order_release);
    return take;
}

void StreamRing::discard() noexcept {
    const u64 written = written_.load(std::memory_order_acquire);
    read_.store(written, std::memory_order_release);
}

bool StreamRing::drained() const noexcept {
    return end_.load(std::memory_order_acquire) != 0 && readable_frames() == 0;
}

// --- StreamSource --------------------------------------------------------------------------------

StreamTiming stream_timing(const CookedAudioHeader& header) noexcept {
    StreamTiming timing;
    timing.play_start = header.play_start;
    timing.frame_count = header.frame_count;
    timing.looping = header.looping;
    timing.loop_start = header.loop_start;
    timing.loop_end = header.effective_loop_end();
    return timing;
}

StreamSource::StreamSource(Allocator& allocator) noexcept
    : resampler_(allocator), decoded_(allocator), pending_(allocator) {}

Status StreamSource::open(DecoderPtr decoder, const StreamTiming& timing,
                          const StreamFormat& format) noexcept {
    close();
    if (!decoder) {
        return fail(ErrorCode::InvalidArgument, "a stream needs a decoder");
    }
    if (format.sample_rate == 0 || format.channels == 0 || format.channels > 2 ||
        format.chunk_frames == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a stream converts to a rate, one or two channels, in non-empty chunks");
    }
    const DecodedFormat source = decoder->format();
    if (source.channels == 0 || source.channels > 2 || source.sample_rate == 0) {
        return fail(ErrorCode::Unsupported, "a stream's source is mono or stereo");
    }
    if (timing.looping) {
        const u64 end = (timing.loop_end == 0) ? timing.frame_count : timing.loop_end;
        if (end != 0 && timing.loop_start >= end) {
            return fail(ErrorCode::InvalidArgument, "a stream's loop starts after it ends");
        }
    }
    if (Status configured =
            resampler_.configure(source.sample_rate, format.sample_rate, format.channels);
        !configured) {
        return configured;
    }
    const u32 widest = math::max(source.channels, format.channels);
    if (Status sized = decoded_.allocate(static_cast<usize>(format.chunk_frames) * widest);
        !sized) {
        return sized;
    }
    // Room for one chunk's output, or a drain's, whichever is larger.
    const u32 most = math::max(resampler_.max_output(format.chunk_frames),
                               resampler_.max_output(Resampler::kHalfTaps * 2));
    if (Status sized = pending_.allocate(static_cast<usize>(most) * format.channels); !sized) {
        return sized;
    }
    decoder_ = static_cast<DecoderPtr&&>(decoder);
    source_ = source;
    timing_ = timing;
    format_ = format;
    return seek(0);
}

void StreamSource::close() noexcept {
    decoder_.reset();
    pending_frames_ = 0;
    pending_offset_ = 0;
    position_ = 0;
    ended_ = false;
    drained_ = false;
}

Status StreamSource::seek(u64 frame) noexcept {
    if (!decoder_) {
        return fail(ErrorCode::Unavailable, "a stream with no decoder cannot seek");
    }
    if (timing_.frame_count != 0 && frame >= timing_.frame_count) {
        return fail(ErrorCode::OutOfRange, "a stream seek past the end of the played range");
    }
    if (Status sought = decoder_->seek(timing_.play_start + frame); !sought) {
        return sought;
    }
    resampler_.reset();
    position_ = frame;
    pending_frames_ = 0;
    pending_offset_ = 0;
    ended_ = false;
    drained_ = false;
    return ok();
}

u64 StreamSource::segment_end() const noexcept {
    if (timing_.looping) {
        if (timing_.loop_end != 0) {
            return timing_.loop_end;
        }
        return (timing_.frame_count != 0) ? timing_.frame_count : kUnbounded;
    }
    return (timing_.frame_count != 0) ? timing_.frame_count : kUnbounded;
}

Status StreamSource::wrap_or_end(u32 decoded) noexcept {
    if (!timing_.looping) {
        ended_ = true;
        return ok();
    }
    // A loop whose start the decoder cannot reach again would spin here forever; a pass that
    // decoded nothing from the loop's own start is the end instead.
    if (decoded == 0 && position_ == timing_.loop_start) {
        ended_ = true;
        return ok();
    }
    if (Status sought = decoder_->seek(timing_.play_start + timing_.loop_start); !sought) {
        return sought;
    }
    position_ = timing_.loop_start;
    return ok();
}

Expected<u32, Error> StreamSource::decode_step() noexcept {
    const u64 end = segment_end();
    const u64 left = (end == kUnbounded) ? format_.chunk_frames : (end - math::min(end, position_));
    const auto want = static_cast<u32>(math::min<u64>(format_.chunk_frames, left));
    u32 got = 0;
    if (want > 0) {
        const Expected<u32, Error> decoded = decoder_->decode(decoded_.data(), want);
        if (!decoded) {
            return decoded;
        }
        got = *decoded;
        position_ += got;
    }
    if (got < want || position_ >= end) {
        if (Status turned = wrap_or_end(got); !turned) {
            return make_unexpected(turned.error());
        }
    }
    return got;
}

u32 StreamSource::flush_pending(StreamRing& ring) noexcept {
    if (pending_frames_ == 0) {
        return 0;
    }
    const u32 wrote =
        ring.write(pending_.data() + (static_cast<usize>(pending_offset_) * format_.channels),
                   pending_frames_);
    pending_frames_ -= wrote;
    pending_offset_ = (pending_frames_ == 0) ? 0 : pending_offset_ + wrote;
    return wrote;
}

Expected<u32, Error> StreamSource::fill(StreamRing& ring, u32 max_chunks) noexcept {
    if (!decoder_) {
        return fail(ErrorCode::Unavailable, "a stream with no decoder cannot fill");
    }
    if (ring.channels() != format_.channels) {
        return fail(ErrorCode::InvalidArgument, "a stream's ring is not at the stream's layout");
    }
    u32 written = flush_pending(ring);
    for (u32 chunk = 0; chunk < max_chunks && pending_frames_ == 0 && !drained_; ++chunk) {
        if (ended_) {
            pending_frames_ = resampler_.drain(pending_.data());
            drained_ = true;
        } else {
            const Expected<u32, Error> got = decode_step();
            if (!got) {
                return got;
            }
            convert_channels(decoded_.data(), source_.channels, decoded_.data(), format_.channels,
                             *got);
            pending_frames_ = resampler_.process(decoded_.data(), *got, pending_.data());
        }
        pending_offset_ = 0;
        written += flush_pending(ring);
    }
    if (drained_ && pending_frames_ == 0) {
        ring.mark_end();
    }
    return written;
}

usize StreamSource::resident_bytes() const noexcept {
    usize bytes = resampler_.resident_bytes();
    bytes += decoded_.bytes() + pending_.bytes();
    if (decoder_) {
        bytes += decoder_->resident_bytes();
    }
    return bytes;
}

}  // namespace cy::audio
