// SPDX-License-Identifier: MIT
#pragma once
// Streaming: a bounded ring of decoded frames between a decode job and the mixer, and the source
// that fills it. Issue #87.
//
// `audio` — "Audio assets and streaming": "Streaming SHALL decode ahead into a ring buffer on the
// asset thread, with underrun producing silence and a diagnostic rather than a stall", and
// "**WHEN** a long music track plays **THEN** it SHALL be streamed and decoded ahead, with only the
// ring buffer resident", and "**WHEN** a track declares loop points **THEN** looping SHALL be
// sample-accurate with no gap or click."
//
// ================================================================================================
// THE TWO HALVES, AND THE ONE THING THEY SHARE
// ================================================================================================
//
// PRODUCER: `StreamSource::fill`, run by whoever refills the stream — a job (src/audio's
//           `StreamPlayer` schedules it) or a test that is deliberately starving it. It decodes,
//           converts channels, resamples to the device's rate and writes into the ring. It may
//           allocate nothing either (everything is sized at `open`), but it may block on I/O,
//           which is why it is never the audio thread.
// CONSUMER: `AudioServer::render`, the realtime callback. It reads from the ring and nothing else.
//
// `StreamRing` is the whole of their contract: single producer, single consumer, two monotonically
// increasing 64-bit counters with release/acquire, and an end-of-stream flag the producer raises
// after its last write. No lock, no allocation, no wait on either side. A consumer that finds fewer
// frames than it needs and no end-of-stream flag has UNDERRUN: it plays silence for the missing
// frames and counts them, and the game thread turns the count into a diagnostic.
//
// ================================================================================================
// WHY A LOOP IS SEAMLESS HERE
// ================================================================================================
//
// The source decodes up to the loop's end, seeks the decoder to the loop's start, and keeps going —
// so the frames on either side of the loop point arrive at the resampler, and then the ring, as one
// contiguous signal. Nothing on the audio thread knows a loop happened; there is no seam for it to
// get wrong. The loop points are frames of the DECODED track, exact, and the seek every decoder
// implements is sample-accurate (decoder.h).

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/servers/audio/buffer.h>
#include <cy/servers/audio/cooked_audio.h>
#include <cy/servers/audio/decoder.h>
#include <cy/servers/audio/resampler.h>

#include <atomic>

namespace cy::audio {

/// The shape of a gain ramp.
enum class FadeCurve : u8 {
    /// Gain moves in a straight line. A bus fade, a duck.
    Linear = 0,
    /// A quarter sine: rising as sin(t·π/2), falling as cos(t·π/2) toward the lower gain. Two
    /// streams crossfaded with it keep the sum of their squared gains constant, so uncorrelated
    /// material crossfades with no dip in loudness halfway — which is what a music change is.
    EqualPower = 1,
    Count = 2,
};

[[nodiscard]] const char* fade_curve_name(FadeCurve curve) noexcept;

/// The gain `t` of the way (0 to 1) along a ramp from `from` to `to`. What the mixer evaluates per
/// frame, public so a test states the expected curve with the same function's definition.
[[nodiscard]] f32 fade_curve_gain(FadeCurve curve, f32 from, f32 to, f32 t) noexcept;

/// A single-producer, single-consumer ring of interleaved float frames. See the file comment.
class StreamRing {
public:
    explicit StreamRing(Allocator& allocator) noexcept;

    StreamRing(const StreamRing&) = delete;
    StreamRing& operator=(const StreamRing&) = delete;
    StreamRing(StreamRing&&) = delete;
    StreamRing& operator=(StreamRing&&) = delete;

    /// Size the ring. Allocates; call it before either side touches the ring.
    [[nodiscard]] Status allocate(u32 capacity_frames, u32 channels) noexcept;

    [[nodiscard]] u32 capacity_frames() const noexcept { return capacity_; }
    [[nodiscard]] u32 channels() const noexcept { return channels_; }

    // --- Producer ----------------------------------------------------------------------------
    [[nodiscard]] u32 writable_frames() const noexcept;
    /// Copy up to `frames` frames in; returns how many fitted.
    u32 write(const f32* frames, u32 count) noexcept;
    /// The producer has written its last frame.
    void mark_end() noexcept { end_.store(1, std::memory_order_release); }

    // --- Consumer (the audio thread) ------------------------------------------------------------
    [[nodiscard]] u32 readable_frames() const noexcept;
    /// Copy up to `count` frames out; returns how many there were.
    u32 read(f32* out, u32 count) noexcept;
    /// Drop everything readable. A seek's flush.
    void discard() noexcept;
    /// Whether the producer has marked the end AND every frame has been read.
    [[nodiscard]] bool drained() const noexcept;
    /// Whether the producer has marked the end, whatever is still to be read.
    [[nodiscard]] bool ended() const noexcept { return end_.load(std::memory_order_acquire) != 0; }

    /// Forget everything, both sides. Only while neither side is running.
    void reset() noexcept;

    /// Frames written and read since the last reset. The consumer's count is the stream's play
    /// position; both are readable from any thread.
    [[nodiscard]] u64 frames_written() const noexcept {
        return written_.load(std::memory_order_acquire);
    }
    [[nodiscard]] u64 frames_read() const noexcept { return read_.load(std::memory_order_acquire); }

    [[nodiscard]] usize resident_bytes() const noexcept { return frames_.bytes(); }

private:
    AudioBuffer<f32> frames_;
    u32 capacity_ = 0;
    u32 channels_ = 0;
    std::atomic<u64> written_{0};
    std::atomic<u64> read_{0};
    std::atomic<u32> end_{0};
};

/// What a stream is converted to: the device's rate and layout, and how much it decodes at once.
struct StreamFormat {
    u32 sample_rate = 48000;
    u32 channels = 2;
    /// Source frames decoded per step of `fill`. The unit of work a refill does.
    u32 chunk_frames = 2048;
};

/// Where the played range and the loop are, in DECODED frames relative to the first played frame.
/// Built from a cooked header by `stream_timing`, or by hand for a raw decoder.
struct StreamTiming {
    /// The decoded frame the played range starts at.
    u64 play_start = 0;
    /// Frames in the played range. Zero: until the decoder ends.
    u64 frame_count = 0;
    bool looping = false;
    u64 loop_start = 0;
    /// Exclusive. Zero: the end of the played range.
    u64 loop_end = 0;
};

[[nodiscard]] StreamTiming stream_timing(const CookedAudioHeader& header) noexcept;

/// The producer half of a stream: a decoder, the conversions, the loop. See the file comment.
class StreamSource {
public:
    explicit StreamSource(Allocator& allocator) noexcept;

    StreamSource(const StreamSource&) = delete;
    StreamSource& operator=(const StreamSource&) = delete;
    StreamSource(StreamSource&&) = delete;
    StreamSource& operator=(StreamSource&&) = delete;

    /// Take `decoder` and size every buffer the source will use. Allocates; game thread. The
    /// decoder is positioned at `timing.play_start`.
    [[nodiscard]] Status open(DecoderPtr decoder, const StreamTiming& timing,
                              const StreamFormat& format) noexcept;
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept { return static_cast<bool>(decoder_); }

    /// PRODUCER. Decode and write until the ring is full, the track has ended, or `max_chunks`
    /// chunks have been decoded — the bound on one refill's work. Returns the frames written.
    /// Marks the ring's end once the last frame of a non-looping track is in it.
    [[nodiscard]] Expected<u32, Error> fill(StreamRing& ring, u32 max_chunks) noexcept;

    /// Position the next frame `fill` writes at played frame `frame` (relative to the played
    /// range's start). Neither side may be running; the caller flushes the ring.
    [[nodiscard]] Status seek(u64 frame) noexcept;

    /// The played frame the next decoded frame is, before conversion.
    [[nodiscard]] u64 position() const noexcept { return position_; }
    [[nodiscard]] bool ended() const noexcept { return ended_; }
    [[nodiscard]] const StreamFormat& format() const noexcept { return format_; }
    [[nodiscard]] DecodedFormat source_format() const noexcept { return source_; }
    [[nodiscard]] const StreamTiming& timing() const noexcept { return timing_; }

    /// Bytes this source holds: the decoder, the resampler, the staging buffers. Not the ring.
    [[nodiscard]] usize resident_bytes() const noexcept;

private:
    [[nodiscard]] u64 segment_end() const noexcept;
    [[nodiscard]] Expected<u32, Error> decode_step() noexcept;
    [[nodiscard]] Status wrap_or_end(u32 decoded) noexcept;
    u32 flush_pending(StreamRing& ring) noexcept;

    DecoderPtr decoder_;
    Resampler resampler_;
    StreamFormat format_;
    DecodedFormat source_;
    StreamTiming timing_;
    /// Decoded frames at the source's layout, then converted to the output layout in place.
    AudioBuffer<f32> decoded_;
    /// Converted frames waiting for room in the ring.
    AudioBuffer<f32> pending_;
    u32 pending_frames_ = 0;
    u32 pending_offset_ = 0;
    u64 position_ = 0;
    bool ended_ = false;
    bool drained_ = false;
};

}  // namespace cy::audio
