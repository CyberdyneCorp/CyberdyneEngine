// SPDX-License-Identifier: MIT
// Byte sources, the built-in PCM decoder, and the helpers over any decoder. See
// cy/servers/audio/decoder.h.

#include <cy/servers/audio/decoder.h>

#include <cy/core/math/scalar.h>

#include <cstring>

namespace cy::audio {
namespace {

/// Interleaved little-endian signed 16-bit PCM. Reads through a small fixed buffer, so it holds the
/// same few kilobytes whether the payload is a second or an hour.
class Pcm16Decoder final : public AudioDecoder {
public:
    static constexpr u32 kChunkFrames = 1024;

    Pcm16Decoder(AudioByteSource& source, const DecodedFormat& format,
                 Allocator& allocator) noexcept
        : source_(&source), format_(format), allocator_(&allocator) {
        const u64 frame_bytes = static_cast<u64>(format.channels) * sizeof(i16);
        format_.frame_count = (frame_bytes == 0) ? 0 : source.size() / frame_bytes;
    }

    [[nodiscard]] DecodedFormat format() const noexcept override { return format_; }

    [[nodiscard]] Expected<u32, Error> decode(f32* out, u32 frames) noexcept override {
        u32 produced = 0;
        const usize frame_bytes = static_cast<usize>(format_.channels) * sizeof(i16);
        while (produced < frames && position_ < format_.frame_count) {
            const u64 left = format_.frame_count - position_;
            const u32 want = static_cast<u32>(
                math::min<u64>(math::min<u64>(frames - produced, kChunkFrames), left));
            const Expected<usize, Error> read = source_->read(
                position_ * frame_bytes, Span<u8>(buffer_, static_cast<usize>(want) * frame_bytes));
            if (!read) {
                return make_unexpected(read.error());
            }
            const u32 got = static_cast<u32>(*read / frame_bytes);
            if (got == 0) {
                break;
            }
            convert(out + (static_cast<usize>(produced) * format_.channels),
                    static_cast<usize>(got) * format_.channels);
            produced += got;
            position_ += got;
        }
        return produced;
    }

    [[nodiscard]] Status seek(u64 frame) noexcept override {
        if (frame > format_.frame_count) {
            return fail(ErrorCode::OutOfRange, "a PCM seek past the end of the track");
        }
        position_ = frame;
        return ok();
    }

    [[nodiscard]] usize resident_bytes() const noexcept override { return sizeof(*this); }

    void destroy() noexcept override {
        Allocator* allocator = allocator_;
        this->~Pcm16Decoder();
        allocator->deallocate(this, sizeof(Pcm16Decoder), alignof(Pcm16Decoder));
    }

private:
    void convert(f32* out, usize samples) const noexcept {
        for (usize index = 0; index < samples; ++index) {
            const auto low = static_cast<u16>(buffer_[index * 2]);
            const auto high = static_cast<u16>(buffer_[(index * 2) + 1]);
            const auto value = static_cast<i16>(static_cast<u16>(low | (high << 8U)));
            out[index] = static_cast<f32>(value) * (1.0F / 32768.0F);
        }
    }

    AudioByteSource* source_;
    DecodedFormat format_;
    Allocator* allocator_;
    u64 position_ = 0;
    u8 buffer_[static_cast<usize>(kChunkFrames) * 2 * sizeof(i16)] = {};
};

class BuiltinCodecs final : public AudioCodecs {
public:
    [[nodiscard]] const char* name() const noexcept override { return "builtin"; }
    [[nodiscard]] bool supports(AudioCodec codec) const noexcept override {
        return codec == AudioCodec::Pcm16;
    }
    [[nodiscard]] Expected<DecoderPtr, Error> open(AudioCodec codec, AudioByteSource& source,
                                                   const DecodedFormat& format,
                                                   Allocator& allocator) noexcept override {
        if (codec != AudioCodec::Pcm16) {
            return fail(ErrorCode::Unavailable,
                        "this build decodes only PCM; Vorbis, FLAC and MP3 need the miniaudio "
                        "backend's codecs (CY_AUDIO)");
        }
        if (format.channels == 0 || format.channels > 2 || format.sample_rate == 0) {
            return fail(ErrorCode::InvalidArgument,
                        "a PCM decoder needs a rate and 1 or 2 channels");
        }
        void* storage = allocator.allocate(sizeof(Pcm16Decoder), alignof(Pcm16Decoder));
        if (storage == nullptr) {
            return fail(ErrorCode::OutOfMemory, "no memory for a PCM decoder");
        }
        return DecoderPtr(construct_at<Pcm16Decoder>(storage, source, format, allocator));
    }
};

[[nodiscard]] bool starts_with(Span<const u8> bytes, usize offset, const char* text) noexcept {
    for (usize index = 0; text[index] != '\0'; ++index) {
        if (offset + index >= bytes.size() ||
            bytes[offset + index] != static_cast<u8>(text[index])) {
            return false;
        }
    }
    return true;
}

}  // namespace

const char* source_format_name(SourceFormat format) noexcept {
    switch (format) {
        case SourceFormat::Unknown:
            return "unknown";
        case SourceFormat::Wav:
            return "wav";
        case SourceFormat::OggVorbis:
            return "ogg-vorbis";
        case SourceFormat::Flac:
            return "flac";
        case SourceFormat::Mp3:
            return "mp3";
        case SourceFormat::Count:
            break;
    }
    return "unknown";
}

SourceFormat sniff_source_format(Span<const u8> head) noexcept {
    if (starts_with(head, 0, "RIFF") && starts_with(head, 8, "WAVE")) {
        return SourceFormat::Wav;
    }
    if (starts_with(head, 0, "fLaC")) {
        return SourceFormat::Flac;
    }
    if (starts_with(head, 0, "OggS")) {
        // The first page carries the identification header: packet type 1, then "vorbis". Its
        // offset is the page header (27 bytes) plus the segment table.
        if (head.size() > 26) {
            const usize packet = 27U + head[26];
            if (packet < head.size() && head[packet] == 1 &&
                starts_with(head, packet + 1, "vorbis")) {
                return SourceFormat::OggVorbis;
            }
        }
        return SourceFormat::Unknown;  // an Ogg of something else: Opus, Theora, FLAC-in-Ogg
    }
    if (starts_with(head, 0, "ID3")) {
        return SourceFormat::Mp3;
    }
    // An MPEG audio frame: eleven sync bits, a layer field of 01 (layer III).
    if (head.size() >= 2 && head[0] == 0xFF && (head[1] & 0xE0U) == 0xE0U &&
        ((head[1] >> 1U) & 0x3U) == 0x1U) {
        return SourceFormat::Mp3;
    }
    return SourceFormat::Unknown;
}

AudioCodec passthrough_codec(SourceFormat format) noexcept {
    switch (format) {
        case SourceFormat::OggVorbis:
            return AudioCodec::Vorbis;
        case SourceFormat::Flac:
            return AudioCodec::Flac;
        case SourceFormat::Mp3:
            return AudioCodec::Mp3;
        case SourceFormat::Unknown:
        case SourceFormat::Wav:
        case SourceFormat::Count:
            break;
    }
    return AudioCodec::Pcm16;
}

Expected<OpenedSource, Error> AudioCodecs::open_source(AudioByteSource& source,
                                                       Allocator& allocator) noexcept {
    (void)source;
    (void)allocator;
    return fail(ErrorCode::Unavailable,
                "this build has no source decoders; importing audio needs the miniaudio backend "
                "(CY_AUDIO)");
}

Expected<usize, Error> MemoryByteSource::read(u64 offset, Span<u8> destination) noexcept {
    if (offset >= bytes_.size()) {
        return usize{0};
    }
    const auto count =
        math::min<usize>(destination.size(), bytes_.size() - static_cast<usize>(offset));
    if (count > 0) {
        std::memcpy(destination.data(), bytes_.data() + offset, count);
    }
    return count;
}

Expected<usize, Error> RangeByteSource::read(u64 offset, Span<u8> destination) noexcept {
    if (parent_ == nullptr || offset >= size_) {
        return usize{0};
    }
    const auto count = static_cast<usize>(math::min<u64>(destination.size(), size_ - offset));
    return parent_->read(offset_ + offset, destination.first(count));
}

AudioCodecs& builtin_audio_codecs() noexcept {
    static BuiltinCodecs codecs;
    return codecs;
}

Expected<DecoderPtr, Error> open_cooked_audio(AudioCodecs& codecs, const CookedAudioHeader& header,
                                              AudioByteSource& asset, RangeByteSource& payload,
                                              Allocator& allocator) noexcept {
    if (header.payload_offset > asset.size() ||
        header.payload_size > asset.size() - header.payload_offset) {
        return fail(ErrorCode::OutOfRange, "a cooked track's payload runs past its source");
    }
    payload.reset(asset, header.payload_offset, header.payload_size);
    DecodedFormat format;
    format.sample_rate = header.sample_rate;
    format.channels = header.channels;
    Expected<DecoderPtr, Error> decoder = codecs.open(header.codec, payload, format, allocator);
    if (!decoder) {
        return decoder;
    }
    const DecodedFormat decoded = (*decoder)->format();
    if (decoded.sample_rate != header.sample_rate || decoded.channels != header.channels) {
        return fail(
            ErrorCode::InvalidArgument,
            "a cooked track's payload decodes at a rate or layout its header does not name");
    }
    if (header.play_start != 0) {
        if (Status sought = (*decoder)->seek(header.play_start); !sought) {
            return make_unexpected(sought.error());
        }
    }
    return decoder;
}

Status decode_all(AudioDecoder& decoder, Array<f32>& out) noexcept {
    out.clear();
    const u32 channels = decoder.format().channels;
    if (channels == 0) {
        return fail(ErrorCode::InvalidArgument, "a decoder with no channels");
    }
    constexpr u32 kChunk = 4096;
    if (const u64 known = decoder.format().frame_count; known > 0) {
        if (Status reserved = out.reserve(static_cast<usize>(known) * channels); !reserved) {
            return reserved;
        }
    }
    for (;;) {
        const usize at = out.size();
        if (Status grown = out.resize(at + (static_cast<usize>(kChunk) * channels)); !grown) {
            return grown;
        }
        const Expected<u32, Error> got = decoder.decode(out.data() + at, kChunk);
        if (!got) {
            return make_unexpected(got.error());
        }
        if (Status trimmed = out.resize(at + (static_cast<usize>(*got) * channels)); !trimmed) {
            return trimmed;
        }
        if (*got == 0) {
            return ok();
        }
    }
}

}  // namespace cy::audio
