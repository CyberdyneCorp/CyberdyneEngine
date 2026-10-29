// SPDX-License-Identifier: MIT
#pragma once
// The cooked audio format: what `cy::import::AudioImporter` writes and the runtime reads. Issue
// #87.
//
// `audio` — "Audio assets and streaming": "Assets SHALL declare a load mode: **fully decoded in
// memory** (short sounds), **compressed in memory, decoded on demand** (medium), or **streamed from
// disk** (music and ambience)." The load mode, the codec, the loop points and the loudness the
// importer measured are all in the header, so a player decides how to play a track from the first
// `kCookedAudioHeaderBytes` bytes without reading the payload.
//
// ================================================================================================
// TWO KINDS OF PAYLOAD, AND WHY THE COMPRESSED ONE IS THE SOURCE'S OWN BITSTREAM
// ================================================================================================
//
// `Pcm16` is interleaved little-endian signed 16-bit samples the importer produced: decoded,
// converted to the project's rate and channel count, trimmed and normalised. A short sound is this.
//
// `Vorbis`, `Flac` and `Mp3` are the SOURCE FILE'S BITSTREAM, copied through unchanged. The engine
// carries decoders and no encoders (`thirdparty-dependencies` would have to admit libvorbis'
// encoder for a re-encode, and a re-encode of a lossy source is a second generation of loss). So a
// streamed track keeps its codec, and what the importer decided — where play starts, how many
// frames play, where the loop is — is metadata the stream applies. Rate and channel conversion of
// such a track happen as it is decoded, on the job that decodes it (stream.h), never on the audio
// thread.
//
// Every frame number in the header counts frames of the DECODED payload at `sample_rate`.
//
// ================================================================================================
// THE BYTES
// ================================================================================================
//
// Little-endian, fixed size, no padding left to the compiler:
//
//   0   u32  magic 'CYAU'          36  u64  loop_start
//   4   u16  version (1)           44  u64  loop_end    (0: the end of the played range)
//   6   u8   codec                 52  f32  peak_dbfs
//   7   u8   load_mode             56  f32  integrated_lufs
//   8   u16  channels              60  f32  gain_db
//   10  u16  flags (bit 0: loops)  64  u64  payload_offset
//   12  u32  sample_rate           72  u64  payload_size
//   16  u64  play_start            80  ---  payload
//   24  u64  frame_count
//   32  u32  reserved (0)

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

#include <string_view>

namespace cy::audio {

/// How the payload is encoded.
enum class AudioCodec : u8 {
    /// Interleaved signed 16-bit PCM, little-endian.
    Pcm16 = 0,
    /// An Ogg Vorbis bitstream, decoded by the backend's codecs.
    Vorbis = 1,
    /// A FLAC bitstream.
    Flac = 2,
    /// An MPEG-1/2 layer III bitstream, with its LAME gapless header if it had one.
    Mp3 = 3,
    Count = 4,
};

/// `audio`'s three load modes.
enum class AudioLoadMode : u8 {
    /// Decoded to float PCM once, at load. Short sounds.
    Decoded = 0,
    /// The payload stays in memory as it is and is decoded as it plays.
    Compressed = 1,
    /// Read from disk in chunks as it plays; only the stream's ring buffer is resident.
    Streamed = 2,
    Count = 3,
};

[[nodiscard]] const char* audio_codec_name(AudioCodec codec) noexcept;
[[nodiscard]] const char* audio_load_mode_name(AudioLoadMode mode) noexcept;
[[nodiscard]] Expected<AudioLoadMode, Error> audio_load_mode_from_name(
    std::string_view name) noexcept;

inline constexpr u32 kCookedAudioMagic = 0x55415943U;  // "CYAU", little-endian
inline constexpr u16 kCookedAudioVersion = 1;
inline constexpr usize kCookedAudioHeaderBytes = 80;
/// The loudness a track with no measurement reports: silence, rather than a plausible number.
inline constexpr f32 kAudioSilenceDb = -200.0F;

/// The header. See the file comment for the layout.
struct CookedAudioHeader {
    AudioCodec codec = AudioCodec::Pcm16;
    AudioLoadMode load_mode = AudioLoadMode::Decoded;
    u16 channels = 0;
    u32 sample_rate = 0;
    /// The first decoded frame that plays. Silence trimming, and a lossy codec's priming the
    /// decoder does not remove, move it past zero.
    u64 play_start = 0;
    /// How many frames play from `play_start`.
    u64 frame_count = 0;
    /// Whether the track loops, and where — relative to `play_start`. `loop_end` of zero is the end
    /// of the played range, so a header that only sets `looping` loops the whole track.
    bool looping = false;
    u64 loop_start = 0;
    u64 loop_end = 0;
    /// Sample peak, in dB relative to full scale, of the played range.
    f32 peak_dbfs = kAudioSilenceDb;
    /// ITU-R BS.1770 integrated loudness of the played range, in LUFS.
    f32 integrated_lufs = kAudioSilenceDb;
    /// The gain a player applies so the track meets the importer's normalisation target. Zero when
    /// the importer was asked not to normalise. Applied at playback, so normalising never clips a
    /// 16-bit payload and never re-encodes a compressed one.
    f32 gain_db = 0.0F;
    u64 payload_offset = kCookedAudioHeaderBytes;
    u64 payload_size = 0;

    /// The loop's end, resolved: `loop_end`, or the end of the played range when it is zero.
    [[nodiscard]] u64 effective_loop_end() const noexcept {
        return (loop_end == 0 || loop_end > frame_count) ? frame_count : loop_end;
    }
    /// `gain_db` as a linear factor.
    [[nodiscard]] f32 gain_linear() const noexcept;
    /// Duration of the played range in seconds.
    [[nodiscard]] f64 seconds() const noexcept {
        return (sample_rate == 0) ? 0.0
                                  : static_cast<f64>(frame_count) / static_cast<f64>(sample_rate);
    }
};

/// A parsed cooked asset: the header and a view of its payload inside the bytes it was parsed from.
struct CookedAudioView {
    CookedAudioHeader header;
    Span<const u8> payload;
};

/// Write `header` and `payload` as one cooked asset into `out` (cleared first). `payload_offset`
/// and `payload_size` are filled in from `payload`; every other field is checked by
/// `validate_cooked_audio_header` first, so an inconsistent header is refused at cook time rather
/// than discovered by a player.
[[nodiscard]] Status write_cooked_audio(const CookedAudioHeader& header, Span<const u8> payload,
                                        Array<u8>& out) noexcept;

/// Write `header` alone into the first `kCookedAudioHeaderBytes` of `out`, for a writer that
/// streams its payload after it. `payload_offset` must be `kCookedAudioHeaderBytes` and
/// `payload_size` the payload that will follow; the header is validated as `write_cooked_audio`
/// validates it.
[[nodiscard]] Status write_cooked_audio_header(const CookedAudioHeader& header,
                                               Span<u8> out) noexcept;

/// Read the header from the first `kCookedAudioHeaderBytes` of an asset. What a streamed player
/// calls, having read only that much.
[[nodiscard]] Expected<CookedAudioHeader, Error> parse_cooked_audio_header(
    Span<const u8> bytes) noexcept;

/// Parse a whole asset held in memory, checking the payload lies inside `bytes`.
[[nodiscard]] Expected<CookedAudioView, Error> parse_cooked_audio(Span<const u8> bytes) noexcept;

/// The rules every header obeys: a known codec and mode, one or two channels, a rate, a played
/// range, a loop inside it, and a PCM payload whose size is exactly the frames it claims.
[[nodiscard]] Status validate_cooked_audio_header(const CookedAudioHeader& header) noexcept;

}  // namespace cy::audio
