# Tasks

- [x] The cooked track format (`cooked_audio.h`), the decoder interface and the built-in PCM codec, the resampler, BS.1770 loudness, the stream ring and source; `integration.audio_stream`.
- [x] Server streams: priming, underrun counting and its diagnostic, per-frame envelopes, flush with declick, natural ends; `integration.audio_mixer`'s stream cases.
- [x] The miniaudio backend's codecs: WAV/FLAC/MP3 through `ma_decoder` with counted allocations, Ogg Vorbis through stb_vorbis's push API with page-bisection seeks, loop metadata; the MP3 forward-seek workaround; `integration.audio_codecs` and `smoke.audio_stream_long`.
- [x] stb_vorbis pinned in `deps/manifest.toml`, provided by `cy__provide_stb_vorbis`, attributed in `THIRD_PARTY.md`, and held to the backend by the layer checker.
- [x] `StreamPlayer` with inline and job executors and `FileByteSource`; `integration.audio_streaming`.
- [x] The audio importer, its registration, `cy_audio_cook`; `integration.import_audio` and the build-graph case in `integration.build_content`.
- [x] ABI 1.18, its thunks and the adapter; `unit.abi` and `integration.game_backend_audio`; the baseline, the Swift overlay and the Rust SDK regenerated; CyberdyneKit's stream API and `AudioStreamTests`.
- [x] samples/14-third-person streams its ambience from Swift; its digest unchanged.
- [x] The `TrackingAllocator` address-reuse defect and its regression case in `unit.memory`.
- [x] Prove the new cases red by mutation, recorded in `evidence/falsification.txt`.
- [x] READMEs, `docs/guides/audio-streaming.md`, the requirements map, the specifications.
