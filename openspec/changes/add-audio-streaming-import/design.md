# Design: audio import and streaming

## Where each part lives, and why

| Part | Module | Why there |
|---|---|---|
| `CookedAudioHeader`, `AudioCodecs`, `AudioDecoder`, PCM decoder, resampler, loudness, `StreamRing`, `StreamSource`, server streams | `src/servers/audio` (layer 2) | The format and the mixer side are engine policy every build has, `CY_AUDIO` or not |
| WAV/FLAC/MP3 (`ma_decoder`), Vorbis (stb_vorbis push API) | `src/backends/audio-miniaudio` (layer 3) | `audio`: decoding is a backend capability; the only directory that may name `ma_` or `stb_vorbis` |
| `StreamPlayer`, `RefillExecutor`s, `FileByteSource` | `src/audio` (layer 4) | Job scheduling and file reads are above the server |
| `AudioImporter`, `cy_audio_cook` | `tools/import` (layer 7) | Source parsing is tool-time |
| ABI 1.18 thunks / adapter | `src/abi`, `src/game_backend` | As every ABI group |

## The cooked bitstream is the source's

The engine carries decoders and no encoders. A compressed or streamed cook therefore keeps the
source's Vorbis, FLAC or MP3 bitstream and records the played range, the loop and the gain beside
it; rate and channel conversion happen in the refill job as it plays. A decoded cook is PCM at the
project rate. A 3D source must be mono, and a bitstream cannot be downmixed without re-encoding, so
such a source is cooked to PCM with a note.

## Threads

Game thread: `pump()` reaps, completes seeks, reports underruns and schedules refills — never
decodes, never reads a file. Refill job: opens the decoder (a Vorbis open reads headers and scans
the last page), seeks, decodes, converts, writes the ring; one in flight per stream; storage reads
run on `AsyncService` (the one thread allowed to block). Audio thread: drains each ring through its
envelope; no allocation, no lock.

The ring is SPSC with two monotonic 64-bit counters and an end flag. Priming (waiting for the ring to
hold half its length) is silence and not an underrun; a short ring with no end flag is an underrun:
silence, counted, and logged by the game thread with the track's name.

## Seeks and crossfades are sample-accurate by construction

A crossfade defers the incoming stream's start until its ring is primed and then issues both
envelopes in one command batch, so they start in the same mixer block and the per-frame curves are
cos/sin of the same t. A seek is a flush (fade out one block, drop the ring, prime again, ramp in
one block) and a decoder seek in the next refill.

Vorbis has no seek in stb's push API, only `flush`. The decoder bisects the file's Ogg pages
(checksums verified, read through a 4 KiB stack window) for the last page at least two long blocks
before the target, flushes, feeds from it, discards packets until stb reports positions and then
frames until the target; on overshoot it backs off four times further; near the start it reopens.
The integration suite compares every seek with a decode from the start, bit for bit.

dr_mp3 (inside miniaudio 0.11.24) seeks forward by subtracting a position that counts the LAME
delay from a target that does not, landing `delay` frames early; the engine seeks MP3 itself by
decoding forward (or from the start when the target is behind). A streamed MP3 that loops to a
non-zero frame therefore re-decodes its intro every lap, which the importer warns about.

## Memory is counted, not estimated

Every buffer on the stream path is an `AudioBuffer` of exactly the size it needs (an `Array` would
round a 200 KiB arena to 256 KiB). miniaudio's allocations are routed to the stream's allocator with
a size header; stb_vorbis gets one arena, grown by doubling until the headers fit and then cut to
what stb reports needing. `StreamReport` and `AudioDecoder::resident_bytes` report the result, and the
tests bound it with a `TrackingAllocator`.
