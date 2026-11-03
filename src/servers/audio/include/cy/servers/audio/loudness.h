// SPDX-License-Identifier: MIT
#pragma once
// Loudness measurement: ITU-R BS.1770-4 integrated loudness and sample peak. Issue #87.
//
// The importer measures every track once, at cook time, and writes the result into the cooked
// header (`CookedAudioHeader::integrated_lufs`, `peak_dbfs`). A project that normalises its music
// to -16 LUFS gets a playback gain per track computed from that measurement, so two tracks mastered
// ten decibels apart play at one level without anyone touching a fader — and without the samples
// being rewritten.
//
// THE ALGORITHM IS THE STANDARD'S, NOT AN APPROXIMATION OF IT: the two-stage K-weighting filter
// (its coefficients derived for the actual sample rate, not only the 48 kHz table), mean square per
// 400 ms block with 75 % overlap, an absolute gate at -70 LUFS and a relative gate 10 LU below the
// absolutely-gated mean. Channel weights are 1.0 for the front pair, which is everything the cooked
// format holds. EBU Tech 3341's reference — a stereo 1 kHz sine at -23 dBFS reads -23 LUFS — is a
// unit case.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>

namespace cy::audio {

struct LoudnessMeasurement {
    /// Integrated loudness, LUFS. `kAudioSilenceDb` when no block passes the absolute gate.
    f32 integrated_lufs = -200.0F;
    /// Largest absolute sample, in dBFS.
    f32 peak_dbfs = -200.0F;
};

/// Measure interleaved float frames at `sample_rate`. One or two channels. Allocates one `f64` per
/// 100 ms of audio from `allocator`, for the gating's second pass.
[[nodiscard]] Expected<LoudnessMeasurement, Error> measure_loudness(const f32* frames,
                                                                    u64 frame_count, u32 channels,
                                                                    u32 sample_rate,
                                                                    Allocator& allocator) noexcept;

}  // namespace cy::audio
