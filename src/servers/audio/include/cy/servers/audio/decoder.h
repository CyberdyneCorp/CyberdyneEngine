// SPDX-License-Identifier: MIT
#pragma once
// Decoding: where a stream's bytes come from, what turns them into float frames, and who supplies
// the decoders. Issue #87.
//
// `audio` — "Engine-owned audio architecture": third-party libraries sit behind engine-owned
// interfaces, and miniaudio is the default `AudioBackend` for "device I/O, conversion, decoding,
// and streaming". The decoders are therefore a BACKEND CAPABILITY (`AudioCodecs`), and this layer
// knows only the interface. Everything a decoder reads, it reads through `AudioByteSource`, so a
// stream from a package on disk, a payload in memory and a test's buffer are one code path.
//
// What this layer implements itself is the one codec that needs no library: `Pcm16`, the format the
// importer writes for decoded sounds. `builtin_audio_codecs()` answers for it in every build, so a
// build with `CY_AUDIO` off still streams the engine's own PCM; the miniaudio backend's
// `miniaudio_audio_codecs()` answers for all four (src/backends/audio-miniaudio/).
//
// THREADING. A decoder is used by one thread at a time — the job that refills its stream — and is
// opened and destroyed on the game thread while no refill is in flight. Nothing here is touched by
// the audio callback.
//
// MEMORY. A decoder allocates only from the allocator it was opened with and reports what it holds
// through `resident_bytes()`, so "only the ring buffer is resident" is a number a test reads rather
// than a claim.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/audio/cooked_audio.h>

namespace cy::audio {

/// Bytes a decoder reads, by offset. Reads may be short at the end and nowhere else.
class AudioByteSource {
public:
    AudioByteSource() = default;
    virtual ~AudioByteSource() = default;

    AudioByteSource(const AudioByteSource&) = delete;
    AudioByteSource& operator=(const AudioByteSource&) = delete;
    AudioByteSource(AudioByteSource&&) = delete;
    AudioByteSource& operator=(AudioByteSource&&) = delete;

    [[nodiscard]] virtual u64 size() const noexcept = 0;
    /// Copy up to `destination.size()` bytes from `offset`. Returns how many were copied: fewer
    /// only at the end, zero at or past it.
    [[nodiscard]] virtual Expected<usize, Error> read(u64 offset,
                                                      Span<u8> destination) noexcept = 0;
    /// Bytes this source keeps resident for itself — a read buffer, a copy. Zero for a view.
    [[nodiscard]] virtual usize resident_bytes() const noexcept { return 0; }
};

/// A view of bytes someone else owns: a payload in memory (`AudioLoadMode::Compressed`) or a test's
/// buffer. Holds nothing of its own.
class MemoryByteSource final : public AudioByteSource {
public:
    MemoryByteSource() noexcept = default;
    explicit MemoryByteSource(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    void reset(Span<const u8> bytes) noexcept { bytes_ = bytes; }

    [[nodiscard]] u64 size() const noexcept override { return bytes_.size(); }
    [[nodiscard]] Expected<usize, Error> read(u64 offset, Span<u8> destination) noexcept override;

private:
    Span<const u8> bytes_;
};

/// A window of another source: a cooked asset's payload inside the file or package that holds it.
class RangeByteSource final : public AudioByteSource {
public:
    RangeByteSource() noexcept = default;
    RangeByteSource(AudioByteSource& parent, u64 offset, u64 size) noexcept
        : parent_(&parent), offset_(offset), size_(size) {}

    void reset(AudioByteSource& parent, u64 offset, u64 size) noexcept {
        parent_ = &parent;
        offset_ = offset;
        size_ = size;
    }

    [[nodiscard]] u64 size() const noexcept override { return size_; }
    [[nodiscard]] Expected<usize, Error> read(u64 offset, Span<u8> destination) noexcept override;

private:
    AudioByteSource* parent_ = nullptr;
    u64 offset_ = 0;
    u64 size_ = 0;
};

/// What a decoder produces: interleaved float frames at the source's own rate and channel count.
struct DecodedFormat {
    u32 sample_rate = 0;
    u32 channels = 0;
    /// Total frames, when the container says. Zero when it cannot be known without decoding.
    u64 frame_count = 0;
};

/// A loop the SOURCE FILE declares — a WAV `smpl` chunk, `LOOPSTART`/`LOOPLENGTH` comments. In
/// decoded frames; `end` is exclusive.
struct SourceLoop {
    bool present = false;
    u64 start = 0;
    u64 end = 0;
};

/// One open decoder. See the file comment for threading and memory.
class AudioDecoder {
public:
    AudioDecoder() = default;
    virtual ~AudioDecoder() = default;

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;
    AudioDecoder(AudioDecoder&&) = delete;
    AudioDecoder& operator=(AudioDecoder&&) = delete;

    [[nodiscard]] virtual DecodedFormat format() const noexcept = 0;
    /// The loop the source declares, if it declares one.
    [[nodiscard]] virtual SourceLoop source_loop() const noexcept { return {}; }

    /// Decode up to `frames` frames into `out`, which holds `frames * format().channels` floats.
    /// Returns how many were decoded; zero means the end.
    [[nodiscard]] virtual Expected<u32, Error> decode(f32* out, u32 frames) noexcept = 0;

    /// Position the next `decode` at `frame`, sample-accurately.
    [[nodiscard]] virtual Status seek(u64 frame) noexcept = 0;

    /// Bytes this decoder holds: its state, its buffers, its tables.
    [[nodiscard]] virtual usize resident_bytes() const noexcept = 0;

    /// Release the decoder and the memory it was constructed in. Every implementation knows its own
    /// size and allocator; `DecoderPtr` calls this, so nothing outside does.
    virtual void destroy() noexcept = 0;
};

/// Sole ownership of a decoder. Move-only; destroys through `AudioDecoder::destroy`.
class DecoderPtr {
public:
    DecoderPtr() noexcept = default;
    explicit DecoderPtr(AudioDecoder* decoder) noexcept : decoder_(decoder) {}
    ~DecoderPtr() { reset(); }

    DecoderPtr(const DecoderPtr&) = delete;
    DecoderPtr& operator=(const DecoderPtr&) = delete;
    DecoderPtr(DecoderPtr&& other) noexcept : decoder_(other.decoder_) { other.decoder_ = nullptr; }
    DecoderPtr& operator=(DecoderPtr&& other) noexcept {
        if (this != &other) {
            reset();
            decoder_ = other.decoder_;
            other.decoder_ = nullptr;
        }
        return *this;
    }

    void reset() noexcept {
        if (decoder_ != nullptr) {
            decoder_->destroy();
            decoder_ = nullptr;
        }
    }

    [[nodiscard]] AudioDecoder* get() const noexcept { return decoder_; }
    [[nodiscard]] AudioDecoder* operator->() const noexcept { return decoder_; }
    [[nodiscard]] AudioDecoder& operator*() const noexcept { return *decoder_; }
    explicit operator bool() const noexcept { return decoder_ != nullptr; }

private:
    AudioDecoder* decoder_ = nullptr;
};

/// The container a SOURCE file is — what the importer reads, before anything is cooked.
enum class SourceFormat : u8 {
    Unknown = 0,
    /// RIFF/WAVE: PCM of any width, IEEE float, or ADPCM.
    Wav,
    OggVorbis,
    Flac,
    Mp3,
    Count,
};

[[nodiscard]] const char* source_format_name(SourceFormat format) noexcept;

/// Recognise a source by its first bytes: `RIFF....WAVE`, `OggS` with a Vorbis identification
/// header, `fLaC`, or an ID3 tag or MPEG audio frame sync. At least 64 bytes give a firm answer.
[[nodiscard]] SourceFormat sniff_source_format(Span<const u8> head) noexcept;

/// The codec a streamed or compressed cook keeps for a source: its own bitstream, or PCM for WAV.
[[nodiscard]] AudioCodec passthrough_codec(SourceFormat format) noexcept;

/// A decoder over a source file, and what the file was.
struct OpenedSource {
    DecoderPtr decoder;
    SourceFormat format = SourceFormat::Unknown;
};

/// Who can decode what. A backend capability; see the file comment.
class AudioCodecs {
public:
    AudioCodecs() = default;
    virtual ~AudioCodecs() = default;

    AudioCodecs(const AudioCodecs&) = delete;
    AudioCodecs& operator=(const AudioCodecs&) = delete;
    AudioCodecs(AudioCodecs&&) = delete;
    AudioCodecs& operator=(AudioCodecs&&) = delete;

    /// "builtin", "miniaudio".
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    [[nodiscard]] virtual bool supports(AudioCodec codec) const noexcept = 0;

    /// Open a decoder over `source`, which must outlive it. `format` describes a raw codec that
    /// carries no header of its own — `Pcm16` — and is ignored by every other codec.
    [[nodiscard]] virtual Expected<DecoderPtr, Error> open(AudioCodec codec,
                                                           AudioByteSource& source,
                                                           const DecodedFormat& format,
                                                           Allocator& allocator) noexcept = 0;

    /// Open a decoder over a whole SOURCE file — WAV, Ogg Vorbis, FLAC or MP3 — recognised by
    /// `sniff_source_format`. Its `format().frame_count` is exact and its `source_loop()` carries
    /// the file's loop metadata. What the importer calls; the default refuses.
    [[nodiscard]] virtual Expected<OpenedSource, Error> open_source(AudioByteSource& source,
                                                                    Allocator& allocator) noexcept;
};

/// The codecs this layer implements itself: `Pcm16`. Every build has them.
[[nodiscard]] AudioCodecs& builtin_audio_codecs() noexcept;

/// Open a cooked track's decoder: the payload of `asset` (read through `asset`) with the codec and
/// format its header names, positioned at the first played frame. `payload` is a caller-owned
/// window the decoder reads through, and must outlive the decoder like `asset`.
[[nodiscard]] Expected<DecoderPtr, Error> open_cooked_audio(AudioCodecs& codecs,
                                                            const CookedAudioHeader& header,
                                                            AudioByteSource& asset,
                                                            RangeByteSource& payload,
                                                            Allocator& allocator) noexcept;

/// Decode a whole source into interleaved floats at its own rate and layout. What the importer and
/// a fully decoded load use; never what a stream uses.
[[nodiscard]] Status decode_all(AudioDecoder& decoder, Array<f32>& out) noexcept;

}  // namespace cy::audio
