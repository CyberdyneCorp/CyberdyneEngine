// SPDX-License-Identifier: MIT
#ifndef CY_IMPORT_AUDIO_H
#define CY_IMPORT_AUDIO_H
// The audio importer: a WAV, Ogg Vorbis, FLAC or MP3 file in, a cooked track out. Issue #87.
//
// `asset-import-pipeline` — "Audio import": format by use, sample-rate conversion, mono downmix for
// 3D sources, loop points, normalisation and silence trimming. `audio` — "Audio assets and
// streaming": every asset declares one of three load modes. Each is an option below, and the cook
// writes the decision into the header `cy/servers/audio/cooked_audio.h` defines.
//
// --- FORMAT BY USE
// ---------------------------------------------------------------------------------
//
// `decoded` (a short effect) is cooked to 16-bit PCM at the project's rate and channel count: the
// source is decoded, downmixed if it is a 3D source, resampled, trimmed, and quantised once, here,
// so loading it is a copy. `compressed` and `streamed` (dialogue, music, ambience) keep the
// source's own Vorbis, FLAC or MP3 bitstream unchanged — the engine carries decoders and no
// encoders, and a re-encode of a lossy source would be a second generation of loss — and record
// where play starts, how long it lasts and where it loops, applied as it plays. A WAV is always
// cooked to PCM; a source that must be downmixed for a 3D emitter is too, with a note saying why
// its bitstream was not kept. `auto` picks `streamed` for anything longer than `stream-threshold`
// seconds and `decoded` below.
//
// --- WHAT IS MEASURED, AND WHAT IS APPLIED
// ---------------------------------------------------------
//
// Loudness (ITU-R BS.1770 integrated) and sample peak are measured over the played range of the
// final signal and written into the header. Normalisation is a GAIN in the header, applied at
// playback — it never clips a 16-bit payload and never touches a kept bitstream — and it is capped
// so the normalised peak stays at or below -1 dBFS, which is reported when it binds.
//
// --- WHAT IT NEEDS
// ---------------------------------------------------------------------------------
//
// The miniaudio backend's decoders, so `CY_AUDIO`. Built without it the importer is still
// registered — a `.wav` in a project is still claimed — and refuses each import naming the option.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/import/importer.h>
#include <cy/import/options.h>
#include <cy/servers/audio/cooked_audio.h>
#include <cy/servers/audio/decoder.h>

namespace cy::import {

enum class AudioChannels : u8 { Keep = 0, Mono, Stereo };
enum class AudioLoop : u8 {
    /// The source's own loop metadata: a WAV `smpl` chunk, `LOOPSTART`/`LOOPLENGTH` comments.
    Source = 0,
    Off,
    /// Loop `loop-start` to `loop-end`, or the whole track when they are unset.
    On,
};
enum class AudioNormalise : u8 { Off = 0, Peak, Loudness };

/// What one cook is asked to produce.
struct AudioCookOptions {
    /// `Count` is `auto`.
    audio::AudioLoadMode mode = audio::AudioLoadMode::Count;
    f32 stream_threshold_seconds = 10.0F;
    /// The rate a PCM cook is converted to; zero keeps the source's. A kept bitstream is converted
    /// as it plays.
    u32 sample_rate = 48000;
    AudioChannels channels = AudioChannels::Keep;
    /// A 3D emitter: the cook is mono whatever `channels` says, because the panner is what places
    /// it.
    bool spatial = false;
    AudioLoop loop = AudioLoop::Source;
    /// In SOURCE frames. Negative: unset.
    i64 loop_start = -1;
    i64 loop_end = -1;
    AudioNormalise normalise = AudioNormalise::Off;
    f32 target_lufs = -16.0F;
    f32 target_peak_db = -1.0F;
    bool trim_silence = false;
    f32 silence_threshold_db = -60.0F;
};

/// What a cook did.
struct AudioCookReport {
    audio::SourceFormat source = audio::SourceFormat::Unknown;
    u32 source_rate = 0;
    u32 source_channels = 0;
    u64 source_frames = 0;
    audio::CookedAudioHeader header;
    /// Frames trimmed from each end, in source frames.
    u64 trimmed_head = 0;
    u64 trimmed_tail = 0;
    bool loop_from_source = false;
    /// The source's bitstream could not be kept (a 3D downmix) and was cooked to PCM instead.
    bool converted_to_pcm = false;
    /// The normalisation gain was capped to keep the peak under the target peak.
    bool gain_capped = false;
};

/// Whether this build can cook audio — `CY_AUDIO` built the miniaudio backend's decoders.
[[nodiscard]] bool audio_cooking_available() noexcept;

/// Cook `source` as `options` asks into `out` (cleared first), the bytes
/// `audio::parse_cooked_audio` reads.
[[nodiscard]] Status cook_audio(Span<const u8> source, const AudioCookOptions& options,
                                Array<u8>& out, AudioCookReport& report) noexcept;

/// The option schema the audio importer declares.
[[nodiscard]] OptionsSchema audio_options() noexcept;

/// `.wav`, `.ogg`, `.oga`, `.flac` and `.mp3` to `AssetKind::Audio`. `.opus` is claimed too, so it
/// is refused with the reason (no Opus decoder is built) rather than ignored.
class AudioImporter final : public Importer {
public:
    [[nodiscard]] ImporterInfo info() const noexcept override;
    [[nodiscard]] OptionsSchema schema() const noexcept override;
    /// Read the options, cook, and add the cooked track as the one primary sub-asset, "audio".
    [[nodiscard]] Status import(const ImportRequest& request, ImportResult& out) noexcept override;
};

}  // namespace cy::import

#endif  // CY_IMPORT_AUDIO_H
