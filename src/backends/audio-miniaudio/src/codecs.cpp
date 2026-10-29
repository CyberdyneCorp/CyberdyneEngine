// SPDX-License-Identifier: MIT
// WAV, FLAC and MP3 through miniaudio's ma_decoder, the backend's codec table, and the loop
// metadata of each container. See cy/backends/audio/miniaudio_codecs.h.
//
// Like miniaudio_backend.cpp this includes miniaudio.h for its declarations only; the bodies are
// the `miniaudio` dependency target's.

#include <cy/backends/audio/miniaudio_codecs.h>

#include "codec_internal.h"

#include <cy/core/math/scalar.h>

#include <miniaudio.h>

#include <charconv>
#include <cstring>

namespace cy::audio {
namespace detail {
namespace {

[[nodiscard]] bool parse_u64(std::string_view text, u64& out) noexcept {
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const std::from_chars_result parsed = std::from_chars(first, last, out);
    return parsed.ec == std::errc{} && parsed.ptr == last && !text.empty();
}

[[nodiscard]] bool key_is(std::string_view comment, std::string_view key) noexcept {
    if (comment.size() <= key.size() || comment[key.size()] != '=') {
        return false;
    }
    for (usize index = 0; index < key.size(); ++index) {
        char c = comment[index];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        if (c != key[index]) {
            return false;
        }
    }
    return true;
}

}  // namespace

void LoopComments::observe(std::string_view comment) noexcept {
    const auto value = [&](std::string_view key) { return comment.substr(key.size() + 1); };
    if (key_is(comment, "LOOPSTART")) {
        has_start_ = parse_u64(value("LOOPSTART"), start_);
    } else if (key_is(comment, "LOOPLENGTH")) {
        has_length_ = parse_u64(value("LOOPLENGTH"), length_);
    } else if (key_is(comment, "LOOPEND")) {
        has_end_ = parse_u64(value("LOOPEND"), end_);
    }
}

SourceLoop LoopComments::loop() const noexcept {
    SourceLoop loop;
    if (!has_start_ || (!has_length_ && !has_end_)) {
        return loop;
    }
    const u64 end = has_length_ ? start_ + length_ : end_;
    if (end <= start_) {
        return loop;
    }
    loop.present = true;
    loop.start = start_;
    loop.end = end;
    return loop;
}

}  // namespace detail

namespace {

constexpr usize kHeapHeader = 16;

/// miniaudio's allocations, routed to an engine allocator and counted. miniaudio frees without a
/// size and the engine's allocators are sized-free, so each block carries its size in front.
struct Heap {
    Allocator* allocator = nullptr;
    usize live = 0;
};

void* heap_malloc(size_t size, void* user) {
    auto* heap = static_cast<Heap*>(user);
    auto* block = static_cast<u8*>(heap->allocator->allocate(size + kHeapHeader, kHeapHeader));
    if (block == nullptr) {
        return nullptr;
    }
    const usize stored = size;
    std::memcpy(block, &stored, sizeof(stored));
    heap->live += size + kHeapHeader;
    return block + kHeapHeader;
}

void heap_free(void* pointer, void* user) {
    if (pointer == nullptr) {
        return;
    }
    auto* heap = static_cast<Heap*>(user);
    u8* block = static_cast<u8*>(pointer) - kHeapHeader;
    usize size = 0;
    std::memcpy(&size, block, sizeof(size));
    heap->live -= size + kHeapHeader;
    heap->allocator->deallocate(block, size + kHeapHeader, kHeapHeader);
}

void* heap_realloc(void* pointer, size_t size, void* user) {
    if (pointer == nullptr) {
        return heap_malloc(size, user);
    }
    if (size == 0) {
        heap_free(pointer, user);
        return nullptr;
    }
    auto* heap = static_cast<Heap*>(user);
    u8* block = static_cast<u8*>(pointer) - kHeapHeader;
    usize old = 0;
    std::memcpy(&old, block, sizeof(old));
    auto* grown = static_cast<u8*>(
        heap->allocator->reallocate(block, old + kHeapHeader, size + kHeapHeader, kHeapHeader));
    if (grown == nullptr) {
        return nullptr;
    }
    const usize stored = size;
    std::memcpy(grown, &stored, sizeof(stored));
    heap->live = heap->live - old + size;
    return grown + kHeapHeader;
}

/// Reads a source through `AudioByteSource`, as ma_decoder's callbacks ask.
struct Cursor {
    AudioByteSource* source = nullptr;
    u64 position = 0;
};

ma_result cursor_read(ma_decoder* decoder, void* out, size_t bytes, size_t* read) {
    auto* cursor = static_cast<Cursor*>(decoder->pUserData);
    const Expected<usize, Error> got =
        cursor->source->read(cursor->position, Span<u8>(static_cast<u8*>(out), bytes));
    if (!got) {
        *read = 0;
        return MA_IO_ERROR;
    }
    cursor->position += *got;
    *read = *got;
    return (*got == 0 && bytes > 0) ? MA_AT_END : MA_SUCCESS;
}

ma_result cursor_seek(ma_decoder* decoder, ma_int64 offset, ma_seek_origin origin) {
    auto* cursor = static_cast<Cursor*>(decoder->pUserData);
    const i64 size = static_cast<i64>(cursor->source->size());
    i64 base = 0;
    if (origin == ma_seek_origin_current) {
        base = static_cast<i64>(cursor->position);
    } else if (origin == ma_seek_origin_end) {
        base = size;
    }
    const i64 target = base + offset;
    if (target < 0 || target > size) {
        return MA_INVALID_ARGS;
    }
    cursor->position = static_cast<u64>(target);
    return MA_SUCCESS;
}

[[nodiscard]] u32 read_le32(const u8* bytes) noexcept {
    return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8U) |
           (static_cast<u32>(bytes[2]) << 16U) | (static_cast<u32>(bytes[3]) << 24U);
}

/// The first forward loop of a WAV's `smpl` chunk, if it has one. Walks the RIFF chunks.
[[nodiscard]] SourceLoop riff_loop(AudioByteSource& source) noexcept {
    constexpr u32 kMaxChunks = 1024;
    u64 offset = 12;  // past "RIFF", the size and "WAVE"
    for (u32 chunk = 0; chunk < kMaxChunks && offset + 8 <= source.size(); ++chunk) {
        u8 header[8] = {};
        const Expected<usize, Error> got = source.read(offset, Span<u8>(header, sizeof(header)));
        if (!got || *got != sizeof(header)) {
            break;
        }
        const u32 size = read_le32(header + 4);
        if (std::memcmp(header, "smpl", 4) == 0 && size >= 36 + 24) {
            u8 body[36 + 24] = {};
            const Expected<usize, Error> read =
                source.read(offset + 8, Span<u8>(body, sizeof(body)));
            if (!read || *read != sizeof(body) || read_le32(body + 28) == 0) {
                break;
            }
            const u32 type = read_le32(body + 36 + 4);
            const u32 start = read_le32(body + 36 + 8);
            const u32 last = read_le32(body + 36 + 12);  // inclusive, the format's convention
            if (type != 0 || last < start) {
                break;  // only a forward loop is a loop the engine can play
            }
            SourceLoop loop;
            loop.present = true;
            loop.start = start;
            loop.end = static_cast<u64>(last) + 1U;
            return loop;
        }
        offset += 8ULL + size + (size & 1U);  // chunks are padded to an even size
    }
    return {};
}

/// A FLAC's loop comments, from its VORBIS_COMMENT metadata block.
[[nodiscard]] SourceLoop flac_loop(AudioByteSource& source, Allocator& allocator) noexcept {
    constexpr u32 kVorbisComment = 4;
    constexpr u32 kMaxBlock = 1U << 16U;
    u64 offset = 4;  // past "fLaC"
    for (u32 block = 0; block < 64; ++block) {
        u8 header[4] = {};
        const Expected<usize, Error> got = source.read(offset, Span<u8>(header, sizeof(header)));
        if (!got || *got != sizeof(header)) {
            break;
        }
        const bool last = (header[0] & 0x80U) != 0;
        const u32 type = header[0] & 0x7FU;
        const u32 length =
            (static_cast<u32>(header[1]) << 16U) | (static_cast<u32>(header[2]) << 8U) | header[3];
        if (type == kVorbisComment && length <= kMaxBlock) {
            Array<u8> body(allocator);
            if (!body.resize(length)) {
                break;
            }
            const Expected<usize, Error> read = source.read(offset + 4, body.span());
            if (!read || *read != length || length < 8) {
                break;
            }
            detail::LoopComments comments;
            u64 at = 4ULL + read_le32(body.data());  // past the vendor string
            if (at + 4 > length) {
                break;
            }
            const u32 count = read_le32(body.data() + at);
            at += 4;
            for (u32 index = 0; index < count && at + 4 <= length; ++index) {
                const u32 size = read_le32(body.data() + at);
                at += 4;
                if (at + size > length) {
                    break;
                }
                comments.observe(
                    std::string_view(reinterpret_cast<const char*>(body.data() + at), size));
                at += size;
            }
            return comments.loop();
        }
        if (last) {
            break;
        }
        offset += 4ULL + length;
    }
    return {};
}

/// A decoder over ma_decoder: WAV, FLAC and MP3.
class MaDecoder final : public AudioDecoder {
public:
    // `decoder_` is filled completely by ma_decoder_init and read only once `ready_` says it was;
    // `{}` would give its enum without a zero enumerator a value the enum does not have.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    MaDecoder(AudioByteSource& source, Allocator& allocator) noexcept : allocator_(&allocator) {
        heap_.allocator = &allocator;
        cursor_.source = &source;
    }

    MaDecoder(const MaDecoder&) = delete;
    MaDecoder& operator=(const MaDecoder&) = delete;
    MaDecoder(MaDecoder&&) = delete;
    MaDecoder& operator=(MaDecoder&&) = delete;

    ~MaDecoder() override {
        if (ready_) {
            ma_decoder_uninit(&decoder_);
        }
    }

    [[nodiscard]] Status initialize(ma_encoding_format encoding, bool exact_length) noexcept {
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        config.encodingFormat = encoding;
        config.allocationCallbacks.pUserData = &heap_;
        config.allocationCallbacks.onMalloc = &heap_malloc;
        config.allocationCallbacks.onRealloc = &heap_realloc;
        config.allocationCallbacks.onFree = &heap_free;
        if (ma_decoder_init(&cursor_read, &cursor_seek, &cursor_, &config, &decoder_) !=
            MA_SUCCESS) {
            return fail(ErrorCode::InvalidArgument,
                        "miniaudio could not open the stream as the format its header names");
        }
        ready_ = true;
        format_.sample_rate = decoder_.outputSampleRate;
        format_.channels = decoder_.outputChannels;
        if (format_.channels == 0 || format_.channels > 2) {
            return fail(ErrorCode::Unsupported,
                        "the engine plays mono and stereo; this source has more channels");
        }
        // DR_MP3 SEEKS FORWARD ONE ENCODER DELAY SHORT. Its forward seek subtracts a position that
        // counts the gapless delay from a target that does not, so every forward seek in a file
        // with a LAME header lands `delay` frames early (1105 for the fixture). A backward seek
        // restarts and is right. So MP3 seeks here are done by this class: back to the start when
        // the target is behind, then decode and discard. integration.audio_codecs proves it.
        decode_to_seek_ = (encoding == ma_encoding_format_mp3);
        if (exact_length) {
            ma_uint64 length = 0;
            if (ma_decoder_get_length_in_pcm_frames(&decoder_, &length) == MA_SUCCESS) {
                format_.frame_count = length;
            }
            // Reading the length may have moved the stream: start again.
            if (ma_decoder_seek_to_pcm_frame(&decoder_, 0) != MA_SUCCESS) {
                return fail(ErrorCode::Io, "miniaudio could not return to the stream's start");
            }
        }
        return ok();
    }

    void set_loop(const SourceLoop& loop) noexcept { loop_ = loop; }

    [[nodiscard]] DecodedFormat format() const noexcept override { return format_; }
    [[nodiscard]] SourceLoop source_loop() const noexcept override { return loop_; }

    [[nodiscard]] Expected<u32, Error> decode(f32* out, u32 frames) noexcept override {
        ma_uint64 read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder_, out, frames, &read);
        if (result != MA_SUCCESS && result != MA_AT_END) {
            return fail(ErrorCode::Io, "miniaudio failed decoding the stream");
        }
        position_ += read;
        return static_cast<u32>(read);
    }

    [[nodiscard]] Status seek(u64 frame) noexcept override {
        if (!decode_to_seek_) {
            if (ma_decoder_seek_to_pcm_frame(&decoder_, frame) != MA_SUCCESS) {
                return fail(ErrorCode::OutOfRange, "miniaudio could not seek the stream");
            }
            position_ = frame;
            return ok();
        }
        if (frame < position_) {
            if (ma_decoder_seek_to_pcm_frame(&decoder_, 0) != MA_SUCCESS) {
                return fail(ErrorCode::Io, "miniaudio could not return to the stream's start");
            }
            position_ = 0;
        }
        f32 discard[1024 * 2];
        while (position_ < frame) {
            const auto want = static_cast<u32>(math::min<u64>(frame - position_, 1024));
            const Expected<u32, Error> got = decode(discard, want);
            if (!got) {
                return make_unexpected(got.error());
            }
            if (*got == 0) {
                return fail(ErrorCode::OutOfRange, "an MP3 seek past the end of the stream");
            }
        }
        return ok();
    }

    [[nodiscard]] usize resident_bytes() const noexcept override {
        return sizeof(*this) + heap_.live;
    }

    void destroy() noexcept override {
        Allocator* allocator = allocator_;
        this->~MaDecoder();
        allocator->deallocate(this, sizeof(MaDecoder), alignof(MaDecoder));
    }

private:
    Allocator* allocator_;
    Heap heap_;
    Cursor cursor_;
    // Filled completely by ma_decoder_init, and read only once `ready_` says it succeeded.
    ma_decoder decoder_;  // NOLINT(cppcoreguidelines-pro-type-member-init)
    bool ready_ = false;
    bool decode_to_seek_ = false;
    DecodedFormat format_;
    SourceLoop loop_;
    u64 position_ = 0;
};

[[nodiscard]] Expected<DecoderPtr, Error> open_ma(AudioByteSource& source, Allocator& allocator,
                                                  ma_encoding_format encoding, bool exact_length,
                                                  SourceLoop loop) noexcept {
    void* storage = allocator.allocate(sizeof(MaDecoder), alignof(MaDecoder));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "no memory for a decoder");
    }
    auto* decoder = construct_at<MaDecoder>(storage, source, allocator);
    DecoderPtr owned(decoder);
    if (Status started = decoder->initialize(encoding, exact_length); !started) {
        return make_unexpected(started.error());
    }
    decoder->set_loop(loop);
    return owned;
}

class MiniaudioCodecs final : public AudioCodecs {
public:
    [[nodiscard]] const char* name() const noexcept override { return "miniaudio"; }

    [[nodiscard]] bool supports(AudioCodec codec) const noexcept override {
        return codec < AudioCodec::Count;
    }

    [[nodiscard]] Expected<DecoderPtr, Error> open(AudioCodec codec, AudioByteSource& source,
                                                   const DecodedFormat& format,
                                                   Allocator& allocator) noexcept override {
        switch (codec) {
            case AudioCodec::Pcm16:
                return builtin_audio_codecs().open(codec, source, format, allocator);
            case AudioCodec::Vorbis:
                return detail::open_vorbis(source, allocator);
            case AudioCodec::Flac:
                return open_ma(source, allocator, ma_encoding_format_flac, false, {});
            case AudioCodec::Mp3:
                return open_ma(source, allocator, ma_encoding_format_mp3, false, {});
            case AudioCodec::Count:
                break;
        }
        return fail(ErrorCode::InvalidArgument, "an unknown codec");
    }

    [[nodiscard]] Expected<OpenedSource, Error> open_source(
        AudioByteSource& source, Allocator& allocator) noexcept override {
        u8 head[64] = {};
        const Expected<usize, Error> got = source.read(0, Span<u8>(head, sizeof(head)));
        if (!got) {
            return make_unexpected(got.error());
        }
        OpenedSource opened;
        opened.format = sniff_source_format(Span<const u8>(head, *got));
        Expected<DecoderPtr, Error> decoder = open_for(opened.format, source, allocator);
        if (!decoder) {
            return make_unexpected(decoder.error());
        }
        opened.decoder = static_cast<DecoderPtr&&>(*decoder);
        return opened;
    }

private:
    [[nodiscard]] static Expected<DecoderPtr, Error> open_for(SourceFormat format,
                                                              AudioByteSource& source,
                                                              Allocator& allocator) noexcept {
        switch (format) {
            case SourceFormat::Wav:
                return open_ma(source, allocator, ma_encoding_format_wav, true, riff_loop(source));
            case SourceFormat::Flac:
                return open_ma(source, allocator, ma_encoding_format_flac, true,
                               flac_loop(source, allocator));
            case SourceFormat::Mp3:
                return open_ma(source, allocator, ma_encoding_format_mp3, true, {});
            case SourceFormat::OggVorbis:
                return detail::open_vorbis(source, allocator);
            case SourceFormat::Unknown:
            case SourceFormat::Count:
                break;
        }
        return fail(ErrorCode::Unsupported,
                    "not a WAV, Ogg Vorbis, FLAC or MP3 file (an Ogg holding Opus is not decoded "
                    "by this build)");
    }
};

}  // namespace

AudioCodecs& miniaudio_audio_codecs() noexcept {
    static MiniaudioCodecs codecs;
    return codecs;
}

}  // namespace cy::audio
