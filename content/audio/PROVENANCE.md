# `content/audio/` — licence and provenance

## What is here

| file | what it is | used by |
|---|---|---|
| `fixtures/tone.flac` | one second of the reference signal, stereo 44.1 kHz FLAC, with `LOOPSTART=4410` and `LOOPLENGTH=22050` comments | `integration.audio_codecs`, `integration.import_audio` |
| `fixtures/tone.ogg` | six seconds of the reference signal, Ogg Vorbis q5, with `LOOPSTART=44100` and `LOOPLENGTH=176400` | `integration.audio_codecs`, `smoke.audio_stream_long`, `integration.import_audio` |
| `fixtures/tone.mp3` | the same six seconds, MP3 192 kbit/s with a LAME gapless header | `integration.audio_codecs`, `integration.import_audio` |
| `music/frontier_theme.ogg` | a 32-second four-chord pad, a seamless loop, Ogg Vorbis q3 | `integration.import_audio`, `docs/guides/audio-streaming.md` |
| `ambience/hillside_wind.ogg` | 24 seconds of wind over grass and distant birds, a seamless loop, Ogg Vorbis q1 | `samples/14-third-person`'s streamed ambience |

## Licence

**The same licence as the rest of this repository** — see [`LICENSE`](../../LICENSE). Every file is
synthesised by `tools/content/make_audio.py` from integer arithmetic and fixed seeds and encoded with
ffmpeg; nothing was recorded, downloaded or derived from anything outside this tree, so
`THIRD_PARTY.md` lists none of it.

The reference signal is two parabolic "sines" per channel in integer arithmetic
(`tests/harness/include/cy/test/audio_signal.h` computes the same integers), so a lossless decode is
compared sample for sample and a lossy one by its signal-to-noise ratio, with no reference PCM
committed.

## Reproducing it

```sh
python3 tools/content/make_audio.py content/audio
```

needs ffmpeg with libvorbis and libmp3lame, and numpy. Every encode passes ffmpeg's bit-exact flags,
so two runs write the same bytes. ffmpeg 6.1.1 (Ubuntu 24.04) wrote the committed files:

| file | bytes | sha256 |
|---|---:|---|
| `ambience/hillside_wind.ogg` | 214 542 | `028f78521e32d6544d2e16b18ae86eb6c5f31301f9b8b9ba3179150c19491203` |
| `fixtures/tone.flac` | 50 152 | `78ed4fb85e39a176d1daa466150731fcca25f1b431e63057f74ca6bc05776eb7` |
| `fixtures/tone.mp3` | 145 468 | `0af3f620c3c3d4c6cb629c359f05ff01c363db3f1295a74267cd413ae04c2e5e` |
| `fixtures/tone.ogg` | 80 950 | `6180d3d564b74cc30234175a6116dd13a5dd1530aa4208711660c1292cbdffbd` |
| `music/frontier_theme.ogg` | 141 657 | `cf309db251ec4f18a329e61c244b05f13ab22adffb87b1deab89e48c202f44da` |

A different ffmpeg or libvorbis may encode different bytes; the tests compare decoded samples with
the reference signal, not the files' digests, so they hold for any faithful re-encode.
