// SPDX-License-Identifier: MIT
// integration.import_audio: the audio importer over WAVs it writes and the fixtures in
// content/audio/. Issue #87.
//
// `asset-import-pipeline` — "Audio import": format by use, sample-rate conversion, mono downmix for
// 3D sources, loop points, normalisation and silence trimming. The exemption this suite replaces
// named its own case: a stereo WAV imported with the 3D-source option cooks to a mono payload of
// the expected sample count with its loop points carried through. That is the first case.

#include <cy/import/audio.h>
#include <cy/import/pipeline.h>
#include <cy/servers/audio/cooked_audio.h>
#include <cy/test/audio_signal.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace cy;
using namespace cy::import;
namespace signal = cy::test::audio_signal;

namespace {

[[nodiscard]] std::vector<u8> fixture(const char* name) {
    const std::string path = std::string(CY_AUDIO_CONTENT_DIR) + "/" + name;
    std::vector<u8> bytes = signal::read_file(path);
    CY_REQUIRE_MESSAGE(!bytes.empty(), "missing fixture " << path);
    return bytes;
}

[[nodiscard]] ImportResult import_audio(const std::vector<u8>& bytes, const ImportOptions* options,
                                        std::string_view path) {
    AudioImporter importer;
    ImportRequest request;
    request.source = assets::VirtualPath::normalise(path).value();
    request.bytes = Span<const u8>(bytes.data(), bytes.size());
    request.options = options;
    ImportResult result;
    CY_REQUIRE(importer.import(request, result).has_value());
    return result;
}

[[nodiscard]] audio::CookedAudioView cooked(const ImportResult& result) {
    CY_REQUIRE_FALSE(result.has_errors());
    const SubAsset* primary = result.primary();
    CY_REQUIRE(primary != nullptr);
    CY_CHECK(primary->kind == assets::AssetKind::Audio);
    CY_CHECK_EQ(primary->view(), "audio");
    const Expected<audio::CookedAudioView, Error> view =
        audio::parse_cooked_audio(Span<const u8>(primary->payload.data(), primary->payload.size()));
    CY_REQUIRE(view.has_value());
    return *view;
}

[[nodiscard]] bool reported(const ImportResult& result, std::string_view code) {
    return std::ranges::any_of(result.diagnostics(), [code](const ImportDiagnostic& diagnostic) {
        return code == diagnostic.code;
    });
}

void set(ImportOptions& options, std::string_view name, const OptionValue& value) {
    CY_REQUIRE(options.set(audio_options(), name, value).has_value());
}

[[nodiscard]] i16 sample_at(const audio::CookedAudioView& view, u64 index) {
    const auto low = static_cast<u16>(view.payload[index * 2]);
    const auto high = static_cast<u16>(view.payload[(index * 2) + 1]);
    return static_cast<i16>(static_cast<u16>(low | (high << 8U)));
}

}  // namespace

#if defined(CY_IMPORT_AUDIO)

CY_TEST_CASE("audio: a stereo WAV for a 3D source cooks mono, of the expected length, loop kept") {
    // One second at 44.1 kHz with a smpl loop over frames 4410..39689 inclusive, cooked for a 3D
    // emitter at 48 kHz: mono, ceil(44100 * 48000 / 44100) = 48000 frames, and the loop converted
    // by the same ratio — 4800 to 43200.
    const std::vector<u8> wav = signal::wav_bytes(signal::reference_samples(44100), 2,
                                                  signal::kFixtureRate, true, 4410, 39689);
    ImportOptions options;
    set(options, "spatial", OptionValue::of_bool(true));
    const ImportResult result = import_audio(wav, &options, "sfx/engine.wav");
    const audio::CookedAudioView view = cooked(result);
    CY_CHECK(view.header.codec == audio::AudioCodec::Pcm16);
    CY_CHECK(view.header.load_mode == audio::AudioLoadMode::Decoded);
    CY_CHECK_EQ(view.header.channels, 1);
    CY_CHECK_EQ(view.header.sample_rate, 48000U);
    CY_CHECK_EQ(view.header.frame_count, 48000U);
    CY_CHECK_EQ(view.payload.size(), 48000U * sizeof(i16));
    CY_CHECK(view.header.looping);
    CY_CHECK_EQ(view.header.loop_start, 4800U);
    CY_CHECK_EQ(view.header.loop_end, 43200U);

    // The mono payload is the average of the two channels, resampled: at an output frame that
    // lands exactly on an input frame (every 160th output is every 147th input), it is that
    // input's average within the resampler's error and the quantisation step.
    for (u64 output = 1600; output < 46000; output += 1600) {
        const u64 input = (output * 147) / 160;
        const f32 expected =
            0.5F * (signal::reference_float(input, 0) + signal::reference_float(input, 1));
        const f32 got = static_cast<f32>(sample_at(view, output)) / 32767.0F;
        CY_CHECK_NEAR(got, expected, 2e-3F);
    }
    CY_CHECK_GT(view.header.integrated_lufs, -40.0F);
    CY_CHECK_LT(view.header.peak_dbfs, 0.0F);
    CY_CHECK_EQ(view.header.gain_db, 0.0F);  // normalisation is off unless asked for
}

CY_TEST_CASE("audio: an Ogg Vorbis track streams with its bitstream kept and its loop carried") {
    const std::vector<u8> ogg = fixture("music/frontier_theme.ogg");
    const ImportResult result = import_audio(ogg, nullptr, "music/frontier_theme.ogg");
    const audio::CookedAudioView view = cooked(result);
    // Thirty-two seconds is past the ten-second threshold, so `auto` streams it.
    CY_CHECK(view.header.load_mode == audio::AudioLoadMode::Streamed);
    CY_CHECK(view.header.codec == audio::AudioCodec::Vorbis);
    CY_CHECK_EQ(view.header.sample_rate, 48000U);
    CY_CHECK_EQ(view.header.channels, 2);
    CY_CHECK_EQ(view.header.frame_count, 1536000U);
    CY_CHECK(view.header.looping);  // from its LOOPSTART and LOOPLENGTH comments
    CY_CHECK_EQ(view.header.loop_start, 0U);
    CY_CHECK_EQ(view.header.effective_loop_end(), 1536000U);
    CY_REQUIRE_EQ(view.payload.size(), ogg.size());
    CY_CHECK(std::equal(view.payload.begin(), view.payload.end(), ogg.begin()));
}

CY_TEST_CASE("audio: the mode by use — a short MP3 decodes to PCM, a FLAC kept compressed") {
    ImportOptions decoded;
    set(decoded, "mode", OptionValue::of_enumeration("decoded"));
    set(decoded, "sample-rate", OptionValue::of_int(0));
    const audio::CookedAudioView mp3 =
        cooked(import_audio(fixture("fixtures/tone.mp3"), &decoded, "sfx/tone.mp3"));
    CY_CHECK(mp3.header.codec == audio::AudioCodec::Pcm16);
    CY_CHECK_EQ(mp3.header.sample_rate, signal::kFixtureRate);
    CY_CHECK_EQ(mp3.header.frame_count, signal::kLongFrames);  // gapless: no delay, no padding

    ImportOptions compressed;
    set(compressed, "mode", OptionValue::of_enumeration("compressed"));
    const audio::CookedAudioView flac =
        cooked(import_audio(fixture("fixtures/tone.flac"), &compressed, "vo/tone.flac"));
    CY_CHECK(flac.header.codec == audio::AudioCodec::Flac);
    CY_CHECK(flac.header.load_mode == audio::AudioLoadMode::Compressed);
    CY_CHECK_EQ(flac.header.frame_count, signal::kShortFrames);
    CY_CHECK(flac.header.looping);
    CY_CHECK_EQ(flac.header.loop_start, 4410U);
    CY_CHECK_EQ(flac.header.loop_end, 26460U);
}

CY_TEST_CASE("audio: a 3D source cannot keep a stereo bitstream, so it cooks to PCM and says so") {
    ImportOptions options;
    set(options, "mode", OptionValue::of_enumeration("streamed"));
    set(options, "spatial", OptionValue::of_bool(true));
    const ImportResult result =
        import_audio(fixture("fixtures/tone.ogg"), &options, "amb/tone.ogg");
    const audio::CookedAudioView view = cooked(result);
    CY_CHECK(view.header.codec == audio::AudioCodec::Pcm16);
    CY_CHECK(view.header.load_mode == audio::AudioLoadMode::Streamed);
    CY_CHECK_EQ(view.header.channels, 1);
    CY_CHECK(reported(result, "audio-downmixed-to-pcm"));
}

CY_TEST_CASE("audio: loudness is measured, and normalising writes a gain capped at the peak") {
    // The reference signal at half amplitude, quiet enough that -16 LUFS needs a gain its peak
    // cannot take without passing -1 dBFS.
    std::vector<i16> quiet = signal::reference_samples(44100);
    for (i16& sample : quiet) {
        sample = static_cast<i16>(sample / 2);
    }
    const std::vector<u8> wav = signal::wav_bytes(quiet, 2, signal::kFixtureRate);
    const audio::CookedAudioView plain = cooked(import_audio(wav, nullptr, "sfx/quiet.wav"));
    CY_CHECK_EQ(plain.header.gain_db, 0.0F);

    ImportOptions loudness;
    set(loudness, "normalise", OptionValue::of_enumeration("loudness"));
    set(loudness, "target-lufs", OptionValue::of_float(-2.0));
    const ImportResult capped = import_audio(wav, &loudness, "sfx/quiet.wav");
    const audio::CookedAudioView view = cooked(capped);
    CY_CHECK_NEAR(view.header.integrated_lufs, plain.header.integrated_lufs, 1e-4F);
    CY_CHECK_NEAR(view.header.peak_dbfs + view.header.gain_db, -1.0F, 1e-4F);
    CY_CHECK(reported(capped, "audio-gain-capped"));

    ImportOptions gentle;
    set(gentle, "normalise", OptionValue::of_enumeration("loudness"));
    set(gentle, "target-lufs", OptionValue::of_float(-30.0));
    const audio::CookedAudioView quieter = cooked(import_audio(wav, &gentle, "sfx/quiet.wav"));
    CY_CHECK_NEAR(quieter.header.integrated_lufs + quieter.header.gain_db, -30.0F, 1e-4F);
    // The samples themselves were not touched: the gain is applied at playback.
    CY_CHECK(std::equal(quieter.payload.begin(), quieter.payload.end(), plain.payload.begin()));
}

CY_TEST_CASE("audio: silence is trimmed from both ends, never into the loop") {
    std::vector<i16> padded(usize{2000} * 2, 0);
    const std::vector<i16> tone = signal::reference_samples(10000);
    padded.insert(padded.end(), tone.begin(), tone.end());
    padded.insert(padded.end(), usize{3000} * 2, 0);
    AudioCookReport report;
    Array<u8> out;
    std::vector<u8> wav = signal::wav_bytes(padded, 2, signal::kFixtureRate);
    AudioCookOptions cook_options;
    cook_options.trim_silence = true;
    cook_options.sample_rate = 0;
    CY_REQUIRE(
        cook_audio(Span<const u8>(wav.data(), wav.size()), cook_options, out, report).has_value());
    // The reference starts at zero on both channels, so its own first frame is trimmed too.
    CY_CHECK_EQ(report.trimmed_head, 2001U);
    CY_CHECK_GE(report.trimmed_tail, 3000U);
    CY_CHECK_EQ(report.header.frame_count, 15000U - report.trimmed_head - report.trimmed_tail);

    // A loop that begins inside the leading silence stops the trim at the loop's start.
    cook_options.loop = AudioLoop::On;
    cook_options.loop_start = 1000;
    cook_options.loop_end = 9000;
    wav = signal::wav_bytes(padded, 2, signal::kFixtureRate);
    CY_REQUIRE(
        cook_audio(Span<const u8>(wav.data(), wav.size()), cook_options, out, report).has_value());
    CY_CHECK_EQ(report.trimmed_head, 1000U);
    CY_CHECK_EQ(report.header.loop_start, 0U);
    CY_CHECK_EQ(report.header.loop_end, 8000U);
}

CY_TEST_CASE("audio: two cooks of one source are byte-identical") {
    const std::vector<u8> wav = signal::wav_bytes(signal::reference_samples(22050), 2, 22050);
    ImportOptions options;
    set(options, "spatial", OptionValue::of_bool(true));
    set(options, "normalise", OptionValue::of_enumeration("loudness"));
    const ImportResult first = import_audio(wav, &options, "sfx/a.wav");
    const ImportResult second = import_audio(wav, &options, "sfx/a.wav");
    const SubAsset* a = first.primary();
    const SubAsset* b = second.primary();
    CY_REQUIRE(a != nullptr);
    CY_REQUIRE(b != nullptr);
    CY_REQUIRE_EQ(a->payload.size(), b->payload.size());
    CY_CHECK(std::equal(a->payload.begin(), a->payload.end(), b->payload.begin()));
}

CY_TEST_CASE("audio: Opus and garbage are refused with the reason, and the registry routes here") {
    std::vector<u8> opus(64, 0);
    const char head[] = "OggS";
    std::copy(head, head + 4, opus.begin());
    opus[26] = 1;
    const char tag[] = "OpusHead";
    std::copy(tag, tag + 8, opus.begin() + 28);
    const ImportResult refused = import_audio(opus, nullptr, "music/theme.opus");
    CY_CHECK(refused.has_errors());
    CY_CHECK(reported(refused, "audio-unsupported"));
    CY_CHECK(refused.assets().empty());

    ImporterRegistry registry;
    CY_REQUIRE(register_builtin_importers(registry).has_value());
    for (const std::string_view path : {"a.wav", "b.ogg", "c.oga", "d.flac", "e.mp3", "f.opus"}) {
        const Importer* importer =
            registry.find_for_source(assets::VirtualPath::normalise(path).value());
        CY_REQUIRE(importer != nullptr);
        CY_CHECK(importer->info().name == "audio");
    }
}

#else

CY_TEST_CASE("audio: without the miniaudio backend a source is refused naming the option") {
    CY_CHECK_FALSE(audio_cooking_available());
    const std::vector<u8> wav = signal::wav_bytes(signal::reference_samples(100), 2, 44100);
    const ImportResult result = import_audio(wav, nullptr, "sfx/a.wav");
    CY_CHECK(result.has_errors());
    CY_CHECK(reported(result, "audio-backend-missing"));
    CY_CHECK(result.assets().empty());
}

#endif
