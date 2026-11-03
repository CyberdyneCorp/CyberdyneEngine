// SPDX-License-Identifier: MIT
// The audio importer. See cy/import/audio.h.

#include <cy/import/audio.h>

#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/audio/loudness.h>
#include <cy/servers/audio/resampler.h>

#if defined(CY_IMPORT_AUDIO)
#    include <cy/backends/audio/miniaudio_codecs.h>
#endif

#include <cmath>
#include <cstdio>
#include <utility>

namespace cy::import {
namespace {

using audio::AudioLoadMode;
using audio::CookedAudioHeader;
using audio::SourceFormat;

constexpr std::string_view kExtensions[] = {".wav", ".ogg", ".oga", ".flac", ".mp3", ".opus"};
constexpr assets::AssetKind kProduces[] = {assets::AssetKind::Audio};
constexpr std::string_view kModes[] = {"auto", "decoded", "compressed", "streamed"};
constexpr std::string_view kChannels[] = {"keep", "mono", "stereo"};
constexpr std::string_view kLoops[] = {"source", "off", "on"};
constexpr std::string_view kNormalise[] = {"off", "peak", "loudness"};

constexpr OptionSpec kOptions[] = {
    {"mode", OptionType::Enumeration, OptionValue::of_enumeration("auto"),
     "How the track loads: decoded to PCM in memory (short sounds), its compressed bitstream kept "
     "in memory, or streamed from disk (music, ambience). Auto streams anything longer than "
     "stream-threshold.",
     Span<const std::string_view>(kModes), 0.0, 0.0},
    {"stream-threshold",
     OptionType::Float,
     OptionValue::of_float(10.0),
     "For mode auto: the length in seconds above which a track is streamed.",
     {},
     0.0,
     3600.0},
    {"sample-rate",
     OptionType::Int,
     OptionValue::of_int(48000),
     "The rate a PCM cook is converted to, in hertz; 0 keeps the source's. A kept bitstream is "
     "converted as it plays.",
     {},
     0.0,
     192000.0},
    {"channels", OptionType::Enumeration, OptionValue::of_enumeration("keep"),
     "The channel count of a PCM cook: the source's, mono (averaged) or stereo.",
     Span<const std::string_view>(kChannels), 0.0, 0.0},
    {"spatial",
     OptionType::Bool,
     OptionValue::of_bool(false),
     "A 3D emitter: cook mono whatever channels says, since the panner places it.",
     {},
     0.0,
     0.0},
    {"loop", OptionType::Enumeration, OptionValue::of_enumeration("source"),
     "Whether the track loops: as the source's own loop metadata says, never, or always (from "
     "loop-start to loop-end, or the whole track).",
     Span<const std::string_view>(kLoops), 0.0, 0.0},
    {"loop-start",
     OptionType::Int,
     OptionValue::of_int(-1),
     "With loop on: the first looped frame, in source frames; -1 is the track's start.",
     {},
     -1.0,
     4.0e12},
    {"loop-end",
     OptionType::Int,
     OptionValue::of_int(-1),
     "With loop on: the frame after the last looped one, in source frames; -1 is the track's end.",
     {},
     -1.0,
     4.0e12},
    {"normalise", OptionType::Enumeration, OptionValue::of_enumeration("off"),
     "Write a playback gain that brings the track to target-lufs (loudness) or target-peak "
     "(peak). The samples are not rewritten.",
     Span<const std::string_view>(kNormalise), 0.0, 0.0},
    {"target-lufs",
     OptionType::Float,
     OptionValue::of_float(-16.0),
     "The integrated loudness, in LUFS, loudness normalisation aims for.",
     {},
     -70.0,
     0.0},
    {"target-peak",
     OptionType::Float,
     OptionValue::of_float(-1.0),
     "The sample peak, in dBFS, peak normalisation aims for and every normalisation stays under.",
     {},
     -60.0,
     0.0},
    {"trim-silence",
     OptionType::Bool,
     OptionValue::of_bool(false),
     "Drop leading and trailing frames quieter than silence-threshold. Never trims into a loop.",
     {},
     0.0,
     0.0},
    {"silence-threshold",
     OptionType::Float,
     OptionValue::of_float(-60.0),
     "The level, in dBFS, below which a frame counts as silence for trimming.",
     {},
     -120.0,
     0.0},
};

[[nodiscard]] Allocator& cook_allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

[[nodiscard]] audio::AudioCodecs& source_codecs() noexcept {
#if defined(CY_IMPORT_AUDIO)
    return audio::miniaudio_audio_codecs();
#else
    return audio::builtin_audio_codecs();
#endif
}

/// The decoded source: interleaved floats at its own rate and layout, and what it says of itself.
struct DecodedSource {
    explicit DecodedSource(Allocator& allocator) noexcept : samples(allocator) {}
    Array<f32> samples;
    audio::DecodedFormat format;
    audio::SourceLoop loop;
    SourceFormat container = SourceFormat::Unknown;

    [[nodiscard]] u64 frames() const noexcept {
        return (format.channels == 0) ? 0 : samples.size() / format.channels;
    }
};

[[nodiscard]] Status decode_source(Span<const u8> bytes, DecodedSource& out) noexcept {
    audio::MemoryByteSource source(bytes);
    Expected<audio::OpenedSource, Error> opened =
        source_codecs().open_source(source, cook_allocator());
    if (!opened) {
        return make_unexpected(opened.error());
    }
    out.container = opened->format;
    out.format = opened->decoder->format();
    out.loop = opened->decoder->source_loop();
    if (Status decoded = audio::decode_all(*opened->decoder, out.samples); !decoded) {
        return decoded;
    }
    if (out.frames() == 0) {
        return fail(ErrorCode::InvalidArgument, "the source decodes to no audio at all");
    }
    return ok();
}

/// The played range and the loop, in source frames, before any trimming or conversion.
struct Range {
    u64 start = 0;
    u64 end = 0;
    bool looping = false;
    u64 loop_start = 0;
    u64 loop_end = 0;
};

[[nodiscard]] Status choose_loop(const AudioCookOptions& options, const DecodedSource& source,
                                 Range& range, AudioCookReport& report) noexcept {
    const u64 frames = source.frames();
    range.end = frames;
    if (options.loop == AudioLoop::Off) {
        return ok();
    }
    if (options.loop == AudioLoop::Source) {
        if (!source.loop.present) {
            return ok();
        }
        range.looping = true;
        range.loop_start = source.loop.start;
        range.loop_end = math::min(source.loop.end, frames);
        report.loop_from_source = true;
    } else {
        range.looping = true;
        range.loop_start = (options.loop_start < 0) ? 0 : static_cast<u64>(options.loop_start);
        range.loop_end = (options.loop_end < 0) ? frames : static_cast<u64>(options.loop_end);
    }
    if (range.loop_start >= range.loop_end || range.loop_end > frames) {
        return fail(ErrorCode::InvalidArgument,
                    "the loop is empty, inverted, or ends past the end of the source");
    }
    return ok();
}

/// Trim silence from either end, never into the loop.
void trim(const AudioCookOptions& options, const DecodedSource& source, Range& range) noexcept {
    if (!options.trim_silence) {
        return;
    }
    const f32 threshold = std::pow(10.0F, options.silence_threshold_db / 20.0F);
    const u32 channels = source.format.channels;
    const auto loud = [&](u64 frame) {
        for (u32 channel = 0; channel < channels; ++channel) {
            if (std::fabs(source.samples[(frame * channels) + channel]) > threshold) {
                return true;
            }
        }
        return false;
    };
    const u64 head_limit = range.looping ? range.loop_start : range.end;
    while (range.start < head_limit && !loud(range.start)) {
        ++range.start;
    }
    const u64 tail_limit = range.looping ? range.loop_end : range.start + 1;
    while (range.end > tail_limit && !loud(range.end - 1)) {
        --range.end;
    }
}

[[nodiscard]] AudioLoadMode choose_mode(const AudioCookOptions& options,
                                        const DecodedSource& source) noexcept {
    if (options.mode != AudioLoadMode::Count) {
        return options.mode;
    }
    const f64 seconds = static_cast<f64>(source.frames()) /
                        static_cast<f64>(math::max(source.format.sample_rate, 1U));
    return (seconds > static_cast<f64>(options.stream_threshold_seconds)) ? AudioLoadMode::Streamed
                                                                          : AudioLoadMode::Decoded;
}

[[nodiscard]] u32 target_channels(const AudioCookOptions& options, u32 source) noexcept {
    if (options.spatial || options.channels == AudioChannels::Mono) {
        return 1;
    }
    return (options.channels == AudioChannels::Stereo) ? 2 : source;
}

/// The PCM cook: the played range, converted, resampled and quantised.
[[nodiscard]] Status cook_pcm(const AudioCookOptions& options, const DecodedSource& source,
                              const Range& range, CookedAudioHeader& header, Array<f32>& pcm,
                              Array<u8>& payload) noexcept {
    const u32 channels = target_channels(options, source.format.channels);
    const u32 rate = (options.sample_rate == 0) ? source.format.sample_rate : options.sample_rate;
    const u64 frames = range.end - range.start;

    Array<f32> converted(cook_allocator());
    if (Status sized = converted.resize(static_cast<usize>(frames) * channels); !sized) {
        return sized;
    }
    audio::convert_channels(source.samples.data() + (range.start * source.format.channels),
                            source.format.channels, converted.data(), channels,
                            static_cast<u32>(frames));

    audio::Resampler resampler(cook_allocator());
    if (Status configured = resampler.configure(source.format.sample_rate, rate, channels);
        !configured) {
        return configured;
    }
    const u64 most = resampler.max_output(static_cast<u32>(frames)) +
                     resampler.max_output(audio::Resampler::kHalfTaps * 2U);
    if (Status sized = pcm.resize(static_cast<usize>(most) * channels); !sized) {
        return sized;
    }
    u64 written = resampler.process(converted.data(), static_cast<u32>(frames), pcm.data());
    written += resampler.drain(pcm.data() + (written * channels));
    if (Status trimmed = pcm.resize(static_cast<usize>(written) * channels); !trimmed) {
        return trimmed;
    }

    if (Status sized = payload.resize(pcm.size() * sizeof(i16)); !sized) {
        return sized;
    }
    for (usize index = 0; index < pcm.size(); ++index) {
        const f32 clamped = math::clamp(pcm[index], -1.0F, 1.0F);
        const auto value = static_cast<i16>(std::lround(clamped * 32767.0F));
        const auto bits = static_cast<u16>(value);
        payload[index * 2] = static_cast<u8>(bits & 0xFFU);
        payload[(index * 2) + 1] = static_cast<u8>(bits >> 8U);
    }

    const u32 from = source.format.sample_rate;
    header.codec = audio::AudioCodec::Pcm16;
    header.channels = static_cast<u16>(channels);
    header.sample_rate = rate;
    header.play_start = 0;
    header.frame_count = written;
    header.looping = range.looping;
    if (range.looping) {
        header.loop_start =
            audio::Resampler::convert_frame(range.loop_start - range.start, from, rate);
        header.loop_end = math::min<u64>(
            audio::Resampler::convert_frame(range.loop_end - range.start, from, rate), written);
    }
    return ok();
}

/// The kept bitstream: the source's bytes, and the range and loop as metadata.
[[nodiscard]] Status keep_bitstream(Span<const u8> bytes, const DecodedSource& source,
                                    const Range& range, CookedAudioHeader& header,
                                    Array<u8>& payload) noexcept {
    header.codec = audio::passthrough_codec(source.container);
    header.channels = static_cast<u16>(source.format.channels);
    header.sample_rate = source.format.sample_rate;
    header.play_start = range.start;
    header.frame_count = range.end - range.start;
    header.looping = range.looping;
    if (range.looping) {
        header.loop_start = range.loop_start - range.start;
        header.loop_end = range.loop_end - range.start;
    }
    return payload.append(bytes);
}

[[nodiscard]] Status measure(const AudioCookOptions& options, const f32* frames, u64 count,
                             u32 channels, u32 rate, CookedAudioHeader& header,
                             AudioCookReport& report) noexcept {
    Expected<audio::LoudnessMeasurement, Error> measured =
        audio::measure_loudness(frames, count, channels, rate, cook_allocator());
    if (!measured) {
        return make_unexpected(measured.error());
    }
    header.integrated_lufs = measured->integrated_lufs;
    header.peak_dbfs = measured->peak_dbfs;
    header.gain_db = 0.0F;
    if (options.normalise == AudioNormalise::Off || measured->peak_dbfs <= audio::kAudioSilenceDb) {
        return ok();
    }
    f32 gain = (options.normalise == AudioNormalise::Peak)
                   ? options.target_peak_db - measured->peak_dbfs
                   : options.target_lufs - measured->integrated_lufs;
    const f32 headroom = options.target_peak_db - measured->peak_dbfs;
    if (gain > headroom) {
        gain = headroom;
        report.gain_capped = options.normalise == AudioNormalise::Loudness;
    }
    header.gain_db = gain;
    return ok();
}

[[nodiscard]] Status cook(Span<const u8> bytes, const AudioCookOptions& options,
                          const DecodedSource& source, Array<u8>& out,
                          AudioCookReport& report) noexcept {
    Range range;
    if (Status chosen = choose_loop(options, source, range, report); !chosen) {
        return chosen;
    }
    trim(options, source, range);
    report.trimmed_head = range.start;
    report.trimmed_tail = source.frames() - range.end;

    CookedAudioHeader header;
    header.load_mode = choose_mode(options, source);
    const bool downmix = target_channels(options, source.format.channels) != source.format.channels;
    const bool keep = header.load_mode != AudioLoadMode::Decoded &&
                      source.container != SourceFormat::Wav && !downmix;
    report.converted_to_pcm = header.load_mode != AudioLoadMode::Decoded &&
                              source.container != SourceFormat::Wav && downmix;

    Array<u8> payload(cook_allocator());
    Array<f32> pcm(cook_allocator());
    Status built = ok();
    if (keep) {
        built = keep_bitstream(bytes, source, range, header, payload);
        if (built) {
            built = measure(options, source.samples.data() + (range.start * source.format.channels),
                            range.end - range.start, source.format.channels,
                            source.format.sample_rate, header, report);
        }
    } else {
        built = cook_pcm(options, source, range, header, pcm, payload);
        if (built) {
            built = measure(options, pcm.data(), header.frame_count, header.channels,
                            header.sample_rate, header, report);
        }
    }
    if (!built) {
        return built;
    }
    report.header = header;
    return audio::write_cooked_audio(header, payload.span(), out);
}

[[nodiscard]] AudioLoadMode mode_of(std::string_view name) noexcept {
    if (name == "auto") {
        return AudioLoadMode::Count;
    }
    const Expected<AudioLoadMode, Error> mode = audio::audio_load_mode_from_name(name);
    return mode ? *mode : AudioLoadMode::Count;
}

template <class Enum>
[[nodiscard]] Enum enum_of(std::string_view name, Span<const std::string_view> names) noexcept {
    for (usize index = 0; index < names.size(); ++index) {
        if (names[index] == name) {
            return static_cast<Enum>(index);
        }
    }
    return static_cast<Enum>(0);
}

[[nodiscard]] AudioCookOptions gather(const ImportRequest& request,
                                      const OptionsSchema& schema) noexcept {
    const auto value = [&](std::string_view name) {
        Expected<OptionValue, Error> found = request.option(schema, name);
        return found ? *found : OptionValue{};
    };
    AudioCookOptions options;
    options.mode = mode_of(value("mode").as_text());
    options.stream_threshold_seconds = static_cast<f32>(value("stream-threshold").as_float());
    options.sample_rate = static_cast<u32>(value("sample-rate").as_int());
    options.channels = enum_of<AudioChannels>(value("channels").as_text(),
                                              Span<const std::string_view>(kChannels));
    options.spatial = value("spatial").as_bool();
    options.loop =
        enum_of<AudioLoop>(value("loop").as_text(), Span<const std::string_view>(kLoops));
    options.loop_start = value("loop-start").as_int();
    options.loop_end = value("loop-end").as_int();
    options.normalise = enum_of<AudioNormalise>(value("normalise").as_text(),
                                                Span<const std::string_view>(kNormalise));
    options.target_lufs = static_cast<f32>(value("target-lufs").as_float());
    options.target_peak_db = static_cast<f32>(value("target-peak").as_float());
    options.trim_silence = value("trim-silence").as_bool();
    options.silence_threshold_db = static_cast<f32>(value("silence-threshold").as_float());
    return options;
}

[[nodiscard]] Status note(ImportResult& out, const ImportRequest& request, ImportSeverity severity,
                          const char* code, std::string_view detail) noexcept {
    return out.report(severity, code, detail, request.source.view());
}

[[nodiscard]] Status report_cook(const AudioCookReport& report, const ImportRequest& request,
                                 ImportResult& out) noexcept {
    if (report.converted_to_pcm) {
        if (Status noted = note(out, request, ImportSeverity::Info, "audio-downmixed-to-pcm",
                                "a 3D source must be mono and a compressed bitstream cannot be "
                                "downmixed without re-encoding it, so this track is cooked as PCM");
            !noted) {
            return noted;
        }
    }
    if (report.gain_capped) {
        if (Status noted = note(out, request, ImportSeverity::Info, "audio-gain-capped",
                                "the loudness target would push the peak past target-peak; the "
                                "gain stops there");
            !noted) {
            return noted;
        }
    }
    const CookedAudioHeader& header = report.header;
    if (header.codec == audio::AudioCodec::Mp3 && header.looping && header.loop_start > 0 &&
        header.load_mode == AudioLoadMode::Streamed) {
        // An MP3 seeks by decoding from its start (see
        // src/backends/audio-miniaudio/src/codecs.cpp), so a loop that does not return to frame
        // zero costs a decode of the intro every lap.
        return note(out, request, ImportSeverity::Warning, "audio-mp3-loop-seek",
                    "a streamed MP3 whose loop starts after its first frame re-decodes the intro "
                    "on every lap; Ogg Vorbis or FLAC seek by page and do not");
    }
    return ok();
}

}  // namespace

bool audio_cooking_available() noexcept {
#if defined(CY_IMPORT_AUDIO)
    return true;
#else
    return false;
#endif
}

Status cook_audio(Span<const u8> source, const AudioCookOptions& options, Array<u8>& out,
                  AudioCookReport& report) noexcept {
    report = AudioCookReport{};
    out.clear();
    DecodedSource decoded(cook_allocator());
    if (Status read = decode_source(source, decoded); !read) {
        return read;
    }
    if (decoded.format.channels == 0 || decoded.format.channels > 2) {
        return fail(ErrorCode::Unsupported, "the engine plays mono and stereo sources");
    }
    report.source = decoded.container;
    report.source_rate = decoded.format.sample_rate;
    report.source_channels = decoded.format.channels;
    report.source_frames = decoded.frames();
    return cook(source, options, decoded, out, report);
}

OptionsSchema audio_options() noexcept {
    return OptionsSchema(Span<const OptionSpec>(kOptions));
}

ImporterInfo AudioImporter::info() const noexcept {
    ImporterInfo result;
    result.name = "audio";
    result.version = 1;
    result.extensions = Span<const std::string_view>(kExtensions);
    result.produces = Span<const assets::AssetKind>(kProduces);
    result.description =
        "Cooks a WAV, Ogg Vorbis, FLAC or MP3 file into a track: decoded to PCM at the project's "
        "rate for a short sound, or its own bitstream kept for a compressed or streamed one, with "
        "a mono downmix for 3D sources, loop points, silence trimming, and measured loudness with "
        "an optional normalisation gain.";
    return result;
}

OptionsSchema AudioImporter::schema() const noexcept {
    return audio_options();
}

Status AudioImporter::import(const ImportRequest& request, ImportResult& out) noexcept {
    if (!audio_cooking_available()) {
        return note(out, request, ImportSeverity::Error, "audio-backend-missing",
                    "this build has no audio decoders to import with; configure with "
                    "-D CY_AUDIO=ON");
    }
    const AudioCookOptions options = gather(request, schema());
    Array<u8> payload;
    AudioCookReport report;
    if (Status cooked = cook_audio(request.bytes, options, payload, report); !cooked) {
        const char* code =
            (cooked.error().code == ErrorCode::Unsupported) ? "audio-unsupported" : "audio-cook";
        return note(out, request, ImportSeverity::Error, code, cooked.error().message);
    }
    if (Status reported = report_cook(report, request, out); !reported) {
        return reported;
    }
    return out.add(assets::AssetKind::Audio, "audio", std::move(payload), true);
}

}  // namespace cy::import
