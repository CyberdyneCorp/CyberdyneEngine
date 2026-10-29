# Add audio import and streaming: decoders, a cooked track format, and streamed music

## Why

Issue #87's audio half. The engine mixed float PCM it was handed and nothing else: no decoder, no
importer, no load modes and no streaming (`requirements-coverage.toml` exempted both "Audio assets
and streaming" and "Audio import" for exactly that). A game could not play a music track at all
without decoding it to memory by hand, and a ten-minute track decoded to memory is a hundred
megabytes. A Swift game had cues and bus volumes and no music verbs.

## What changes

- **Decoders behind the backend.** `cy::audio::AudioCodecs` (servers/audio) is the engine-owned
  decoding interface; the built-in codecs decode the engine's own PCM in every build, and the
  miniaudio backend's decode WAV, FLAC and MP3 through miniaudio's `ma_decoder` and Ogg Vorbis
  through stb_vorbis's push API (a new pinned dependency), all over an engine `AudioByteSource` so a
  package range, a payload in memory and a file are one path. Vorbis seeks sample-accurately by
  bisecting Ogg pages; MP3 seeks are done by the engine because dr_mp3's forward seek lands one
  encoder delay early.
- **The cooked track** (`cooked_audio.h`): codec, load mode, rate, channels, played range, loop and
  measured loudness in an 80-byte header, then PCM or the source's own bitstream.
- **Streaming** (`stream.h`, server streams, `cy::audio::StreamPlayer`): a lock-free ring per
  stream that refill jobs fill — storage reads on the async service — with rate and channel
  conversion in the job; per-frame gain envelopes on the audio thread for sample-aligned
  equal-power crossfades; priming, flush-and-reprime seeks with declicking ramps; underruns as
  silence, counted, and reported as diagnostics.
- **The audio importer** (`tools/import`): WAV, Ogg Vorbis, FLAC and MP3 in; format by use, mono for
  3D sources, sample-rate conversion, loop points from options or the source's metadata, silence
  trimming, BS.1770 loudness and a capped normalisation gain; a build-graph node like every importer,
  and `cy_audio_cook` for a sample's build.
- **ABI 1.18**: `audio_find_track` and `audio_stream_{play,stop,seek,set_volume,state}`, with
  CyberdyneKit's `Audio.track`, `Audio.stream`, `Audio.crossfade` and `AudioStream`; the Swift
  overlay and the Rust SDK regenerated.
- **samples/14-third-person** streams its hillside wind as a cooked Ogg Vorbis bed, started from
  Swift; its digest is unchanged.
- **A defect in `TrackingAllocator`** found by the stream memory tests: a free of an address the
  heap had handed out again was counted as a double free and never released.

## Impact

- Specs: `audio` (Audio assets and streaming), `asset-import-pipeline` (Audio import), `native-abi`
  (Streamed music and ambience through the ABI), `core-memory-and-containers` (Memory diagnostics).
- Dependencies: stb_vorbis (MIT or public domain, taken under MIT), optional with `CY_AUDIO`.
- Not built here: Opus and ADPCM decoding, an editor audio-import panel (the editor lists importers
  from the registry, so `.wav`/`.ogg`/`.flac`/`.mp3` are claimed there by name), runtime loading of
  cooked tracks through the asset system's packages (the player reads any `AudioByteSource`).
