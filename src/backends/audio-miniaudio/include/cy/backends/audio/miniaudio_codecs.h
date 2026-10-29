// SPDX-License-Identifier: MIT
#pragma once
// The miniaudio backend's decoders, behind `cy::audio::AudioCodecs`. Issue #87.
//
// `audio`: miniaudio is the default backend for "device I/O, conversion, decoding, and streaming".
// This is the decoding: WAV, FLAC and MP3 through miniaudio's `ma_decoder` (its bundled dr_wav,
// dr_flac and dr_mp3), and Ogg Vorbis through stb_vorbis's push API, which miniaudio's own Vorbis
// path is built on and which this backend drives itself so that a stream from any
// `AudioByteSource` seeks sample-accurately by bisecting Ogg pages rather than by decoding from the
// start. No miniaudio or stb_vorbis type appears here; src/codecs.cpp and src/vorbis_decoder.cpp
// are the translation units that name them.
//
// MEMORY. Every byte a decoder holds comes from the allocator it was opened with: miniaudio's
// allocation callbacks are routed to it, and stb_vorbis is handed one arena from it. So a stream's
// `resident_bytes()` is the decoder's real footprint, and a `TrackingAllocator` sees all of it.

#include <cy/servers/audio/decoder.h>

namespace cy::audio {

/// The codecs this backend decodes: `Pcm16`, `Vorbis`, `Flac` and `Mp3`, plus WAV sources for the
/// importer through `open_source`.
[[nodiscard]] AudioCodecs& miniaudio_audio_codecs() noexcept;

}  // namespace cy::audio
