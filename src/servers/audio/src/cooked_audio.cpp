// SPDX-License-Identifier: MIT
// The cooked audio format. See cy/servers/audio/cooked_audio.h.

#include <cy/servers/audio/cooked_audio.h>

#include <cmath>
#include <cstring>

namespace cy::audio {
namespace {

constexpr u16 kFlagLooping = 1U;

void put_u16(u8* out, u16 value) noexcept {
    out[0] = static_cast<u8>(value & 0xFFU);
    out[1] = static_cast<u8>((value >> 8U) & 0xFFU);
}

void put_u32(u8* out, u32 value) noexcept {
    for (u32 byte = 0; byte < 4; ++byte) {
        out[byte] = static_cast<u8>((value >> (8U * byte)) & 0xFFU);
    }
}

void put_u64(u8* out, u64 value) noexcept {
    for (u32 byte = 0; byte < 8; ++byte) {
        out[byte] = static_cast<u8>((value >> (8U * byte)) & 0xFFU);
    }
}

void put_f32(u8* out, f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits);
}

[[nodiscard]] u16 get_u16(const u8* in) noexcept {
    return static_cast<u16>(in[0] | (static_cast<u16>(in[1]) << 8U));
}

[[nodiscard]] u32 get_u32(const u8* in) noexcept {
    u32 value = 0;
    for (u32 byte = 0; byte < 4; ++byte) {
        value |= static_cast<u32>(in[byte]) << (8U * byte);
    }
    return value;
}

[[nodiscard]] u64 get_u64(const u8* in) noexcept {
    u64 value = 0;
    for (u32 byte = 0; byte < 8; ++byte) {
        value |= static_cast<u64>(in[byte]) << (8U * byte);
    }
    return value;
}

[[nodiscard]] f32 get_f32(const u8* in) noexcept {
    const u32 bits = get_u32(in);
    f32 value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void encode_header(const CookedAudioHeader& header, u8* out) noexcept {
    std::memset(out, 0, kCookedAudioHeaderBytes);
    put_u32(out + 0, kCookedAudioMagic);
    put_u16(out + 4, kCookedAudioVersion);
    out[6] = static_cast<u8>(header.codec);
    out[7] = static_cast<u8>(header.load_mode);
    put_u16(out + 8, header.channels);
    put_u16(out + 10, static_cast<u16>(header.looping ? kFlagLooping : 0U));
    put_u32(out + 12, header.sample_rate);
    put_u64(out + 16, header.play_start);
    put_u64(out + 24, header.frame_count);
    put_u64(out + 36, header.loop_start);
    put_u64(out + 44, header.loop_end);
    put_f32(out + 52, header.peak_dbfs);
    put_f32(out + 56, header.integrated_lufs);
    put_f32(out + 60, header.gain_db);
    put_u64(out + 64, header.payload_offset);
    put_u64(out + 72, header.payload_size);
}

[[nodiscard]] Status validate_loop(const CookedAudioHeader& header) noexcept {
    if (!header.looping) {
        return ok();
    }
    const u64 end = header.effective_loop_end();
    if (header.loop_start >= end) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked track's loop starts at or after where it ends");
    }
    return ok();
}

}  // namespace

const char* audio_codec_name(AudioCodec codec) noexcept {
    switch (codec) {
        case AudioCodec::Pcm16:
            return "pcm16";
        case AudioCodec::Vorbis:
            return "vorbis";
        case AudioCodec::Flac:
            return "flac";
        case AudioCodec::Mp3:
            return "mp3";
        case AudioCodec::Count:
            break;
    }
    return "unknown";
}

const char* audio_load_mode_name(AudioLoadMode mode) noexcept {
    switch (mode) {
        case AudioLoadMode::Decoded:
            return "decoded";
        case AudioLoadMode::Compressed:
            return "compressed";
        case AudioLoadMode::Streamed:
            return "streamed";
        case AudioLoadMode::Count:
            break;
    }
    return "unknown";
}

Expected<AudioLoadMode, Error> audio_load_mode_from_name(std::string_view name) noexcept {
    for (u32 index = 0; index < static_cast<u32>(AudioLoadMode::Count); ++index) {
        const auto mode = static_cast<AudioLoadMode>(index);
        if (name == audio_load_mode_name(mode)) {
            return mode;
        }
    }
    return fail(ErrorCode::InvalidArgument,
                "an audio load mode is one of 'decoded', 'compressed' or 'streamed'");
}

f32 CookedAudioHeader::gain_linear() const noexcept {
    return std::pow(10.0F, gain_db / 20.0F);
}

Status validate_cooked_audio_header(const CookedAudioHeader& header) noexcept {
    if (header.codec >= AudioCodec::Count || header.load_mode >= AudioLoadMode::Count) {
        return fail(ErrorCode::InvalidArgument, "a cooked track names an unknown codec or mode");
    }
    if (header.channels == 0 || header.channels > 2) {
        return fail(ErrorCode::Unsupported, "a cooked track is mono or stereo");
    }
    if (header.sample_rate == 0 || header.frame_count == 0) {
        return fail(ErrorCode::InvalidArgument, "a cooked track has a sample rate and frames");
    }
    if (header.codec == AudioCodec::Pcm16) {
        if (header.play_start != 0 ||
            header.payload_size != header.frame_count * header.channels * sizeof(i16)) {
            return fail(ErrorCode::InvalidArgument,
                        "a PCM track's payload is exactly its frames, from the first");
        }
    }
    if (header.load_mode == AudioLoadMode::Decoded && header.codec != AudioCodec::Pcm16) {
        return fail(ErrorCode::InvalidArgument,
                    "a track decoded in memory is cooked as PCM; only a compressed or streamed "
                    "track keeps its source's bitstream");
    }
    return validate_loop(header);
}

Status write_cooked_audio(const CookedAudioHeader& header, Span<const u8> payload,
                          Array<u8>& out) noexcept {
    CookedAudioHeader written = header;
    written.payload_offset = kCookedAudioHeaderBytes;
    written.payload_size = payload.size();
    if (Status valid = validate_cooked_audio_header(written); !valid) {
        return valid;
    }
    out.clear();
    if (Status sized = out.resize(kCookedAudioHeaderBytes + payload.size()); !sized) {
        return sized;
    }
    encode_header(written, out.data());
    if (!payload.empty()) {
        std::memcpy(out.data() + kCookedAudioHeaderBytes, payload.data(), payload.size());
    }
    return ok();
}

Status write_cooked_audio_header(const CookedAudioHeader& header, Span<u8> out) noexcept {
    if (out.size() < kCookedAudioHeaderBytes) {
        return fail(ErrorCode::BufferTooSmall, "a cooked track's header is 80 bytes");
    }
    if (header.payload_offset != kCookedAudioHeaderBytes) {
        return fail(ErrorCode::InvalidArgument, "a cooked track's payload follows its header");
    }
    if (Status valid = validate_cooked_audio_header(header); !valid) {
        return valid;
    }
    encode_header(header, out.data());
    return ok();
}

Expected<CookedAudioHeader, Error> parse_cooked_audio_header(Span<const u8> bytes) noexcept {
    if (bytes.size() < kCookedAudioHeaderBytes) {
        return fail(ErrorCode::InvalidArgument, "a cooked track is shorter than its header");
    }
    const u8* in = bytes.data();
    if (get_u32(in) != kCookedAudioMagic) {
        return fail(ErrorCode::InvalidArgument, "not a cooked track: the magic is not 'CYAU'");
    }
    if (get_u16(in + 4) != kCookedAudioVersion) {
        return fail(ErrorCode::Unsupported, "a cooked track of a version this build cannot read");
    }
    CookedAudioHeader header;
    header.codec = static_cast<AudioCodec>(in[6]);
    header.load_mode = static_cast<AudioLoadMode>(in[7]);
    header.channels = get_u16(in + 8);
    header.looping = (get_u16(in + 10) & kFlagLooping) != 0;
    header.sample_rate = get_u32(in + 12);
    header.play_start = get_u64(in + 16);
    header.frame_count = get_u64(in + 24);
    header.loop_start = get_u64(in + 36);
    header.loop_end = get_u64(in + 44);
    header.peak_dbfs = get_f32(in + 52);
    header.integrated_lufs = get_f32(in + 56);
    header.gain_db = get_f32(in + 60);
    header.payload_offset = get_u64(in + 64);
    header.payload_size = get_u64(in + 72);
    if (header.payload_offset < kCookedAudioHeaderBytes) {
        return fail(ErrorCode::InvalidArgument, "a cooked track's payload overlaps its header");
    }
    if (Status valid = validate_cooked_audio_header(header); !valid) {
        return make_unexpected(valid.error());
    }
    return header;
}

Expected<CookedAudioView, Error> parse_cooked_audio(Span<const u8> bytes) noexcept {
    Expected<CookedAudioHeader, Error> header = parse_cooked_audio_header(bytes);
    if (!header) {
        return make_unexpected(header.error());
    }
    if (header->payload_offset > bytes.size() ||
        header->payload_size > bytes.size() - header->payload_offset) {
        return fail(ErrorCode::OutOfRange, "a cooked track's payload runs past its bytes");
    }
    CookedAudioView view;
    view.header = *header;
    view.payload = bytes.subspan(static_cast<usize>(header->payload_offset),
                                 static_cast<usize>(header->payload_size));
    return view;
}

}  // namespace cy::audio
