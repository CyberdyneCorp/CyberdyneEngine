#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Produce the audio the engine carries in content/audio/ — test fixtures and the RTS music.

--- WHAT THIS IS FOR -----------------------------------------------------------------------------

`audio` and `asset-import-pipeline` need real encoded files to be tested against: a FLAC, an Ogg
Vorbis and an MP3 that the miniaudio backend's decoders read, and whose decoded samples can be
compared with a reference nobody had to commit. So every fixture encodes ONE signal, `reference()`
below, defined in integer arithmetic only. The C++ suites compute the same integers
(src/backends/audio-miniaudio/tests/reference_signal.h), so a lossless decode is compared sample for
sample and a lossy one by its signal-to-noise ratio, on every platform, with no reference PCM file.

The signal is two parabolic "sines" per channel: an audible one (period 100 and 148 frames) and a
slow one whose period does not divide the audible one's, so the waveform does not repeat within a
fixture and a seek that lands on the wrong frame cannot look right by accident.

The music and the ambience are the project's own, synthesised below from fixed seeds as seamless
loops — a four-chord pad for the RTS theme, wind over grass with distant birds for the third-person
level — with LOOPSTART and LOOPLENGTH comments so the importer carries each loop through.

--- REPRODUCING IT --------------------------------------------------------------------------------

    python3 tools/content/make_audio.py content/audio

needs ffmpeg with libvorbis and libmp3lame on PATH, and numpy for the music. Every encode passes
ffmpeg's bit-exact flags, so the encoder's version string is not written into the files;
content/audio/PROVENANCE.md records the digest of every file this writes and the ffmpeg that wrote
them.
"""

from __future__ import annotations

import hashlib
import math
import pathlib
import struct
import subprocess
import sys
import tempfile

FIXTURE_RATE = 44100
# Audible and slow periods, in frames, per channel. Even, so each half-period is a whole number of
# frames. Mirrored in reference_signal.h — change both or neither.
PERIODS = ((100, 17642), (148, 26462))
AMPLITUDES = ((8000, 3000), (7000, 3000))

MUSIC_RATE = 48000
MUSIC_SEED = 0x5EED_A0D1
MUSIC_SECONDS_PER_CHORD = 8.0

AMBIENCE_SEED = 0x5EED_817D
AMBIENCE_SECONDS = 24


def hump(frame: int, period: int, amplitude: int) -> int:
    """A parabolic approximation of a sine, in integers: positive hump, then negative hump."""
    half = period // 2
    phase = frame % period
    sign = 1
    if phase >= half:
        phase -= half
        sign = -1
    return sign * ((4 * amplitude * phase * (half - phase)) // (half * half))


def reference(frame: int, channel: int) -> int:
    """The fixture signal's sample, as a signed 16-bit integer."""
    (fast, slow), (fast_amplitude, slow_amplitude) = PERIODS[channel], AMPLITUDES[channel]
    return hump(frame, fast, fast_amplitude) + hump(frame, slow, slow_amplitude)


def wav_bytes(samples: list[int], channels: int, rate: int,
              loop: tuple[int, int] | None = None) -> bytes:
    """A 16-bit PCM RIFF/WAVE, with a `smpl` chunk when `loop` is (first frame, last frame)."""
    data = struct.pack(f"<{len(samples)}h", *samples)
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * 2, channels * 2, 16)
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    if loop is not None:
        # The sampler chunk: nine header fields, then one loop of type 0 (forward), whose end is the
        # last frame played, inclusive — which is the format's own convention.
        smpl = struct.pack("<9I", 0, 0, 1_000_000_000 // rate, 60, 0, 0, 0, 1, 0)
        smpl += struct.pack("<6I", 0, 0, loop[0], loop[1], 0, 0)
        chunks += b"smpl" + struct.pack("<I", len(smpl)) + smpl
    chunks += b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks


def fixture_samples(frames: int) -> list[int]:
    out: list[int] = []
    for frame in range(frames):
        out.append(reference(frame, 0))
        out.append(reference(frame, 1))
    return out


def ffmpeg(source: pathlib.Path, target: pathlib.Path, *arguments: str) -> None:
    command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(source),
               "-fflags", "+bitexact", "-flags:a", "+bitexact", "-map_metadata", "-1",
               *arguments, str(target)]
    subprocess.run(command, check=True)


def music_samples() -> list[float]:
    """The RTS theme: Am, F, C, G as slow pads with a soft arpeggio, looping seamlessly."""
    import numpy as np

    rng = np.random.default_rng(MUSIC_SEED)
    chords = ((57, 60, 64, 69), (53, 57, 60, 65), (48, 55, 60, 64), (55, 59, 62, 67))
    chord_frames = int(MUSIC_SECONDS_PER_CHORD * MUSIC_RATE)
    total = chord_frames * len(chords)
    time = np.arange(total) / MUSIC_RATE
    left = np.zeros(total)
    right = np.zeros(total)

    def hz(note: int) -> float:
        return 440.0 * 2.0 ** ((note - 69) / 12.0)

    for index, chord in enumerate(chords):
        # A raised-cosine window two chords wide, centred on this chord and wrapped around the
        # loop, so neighbouring chords cross-fade and the last one fades into the first.
        centre = (index + 0.5) * chord_frames
        distance = (np.arange(total) - centre + total / 2) % total - total / 2
        window = np.where(np.abs(distance) < chord_frames,
                          0.5 + 0.5 * np.cos(np.pi * distance / chord_frames), 0.0)
        for voice, note in enumerate(chord):
            # Whole cycles per loop keep the wrap seamless: round each partial's frequency so the
            # loop holds an integer number of its periods.
            for detune, pan in ((-0.07, 0.25), (0.07, 0.75)):
                frequency = round(hz(note + detune) * total / MUSIC_RATE) * MUSIC_RATE / total
                phase = rng.uniform(0.0, 2.0 * math.pi)
                tone = np.sin(2.0 * math.pi * frequency * time + phase)
                tone += 0.25 * np.sin(4.0 * math.pi * frequency * time + phase)
                gain = window * (0.11 if voice else 0.14)
                left += tone * gain * math.cos(pan * math.pi / 2)
                right += tone * gain * math.sin(pan * math.pi / 2)
        # An arpeggio: eight plucks per chord, each a decaying partial, also wrapped.
        for step in range(8):
            note = chord[step % 4] + 12
            frequency = round(hz(note) * total / MUSIC_RATE) * MUSIC_RATE / total
            start = index * chord_frames + step * chord_frames // 8
            age = ((np.arange(total) - start) % total) / MUSIC_RATE
            envelope = np.where(age < 1.5, np.exp(-age * 3.0) * (1.0 - np.exp(-age * 200.0)), 0.0)
            pluck = np.sin(2.0 * math.pi * frequency * time) * envelope * 0.08
            pan = 0.3 + 0.4 * ((step * 3) % 8) / 7.0
            left += pluck * math.cos(pan * math.pi / 2)
            right += pluck * math.sin(pan * math.pi / 2)

    peak = max(np.max(np.abs(left)), np.max(np.abs(right)))
    scale = 0.5 / peak  # -6 dBFS peak: the importer measures loudness, this only leaves headroom
    interleaved = np.empty(total * 2)
    interleaved[0::2] = left * scale
    interleaved[1::2] = right * scale
    return [int(round(value * 32767.0)) for value in interleaved]


def ambience_samples() -> list[int]:
    """The third-person level's ambience: wind over a hillside and a few distant birds.

    Built circularly so it loops without a seam: the wind is noise shaped in the frequency domain
    over the whole loop (so its inverse transform is periodic), its gusts are whole cycles of slow
    sines, and a chirp near the end wraps round to the start.
    """
    import numpy as np

    rng = np.random.default_rng(AMBIENCE_SEED)
    total = AMBIENCE_SECONDS * MUSIC_RATE
    frequencies = np.fft.rfftfreq(total, 1.0 / MUSIC_RATE)

    def wind() -> "np.ndarray":
        spectrum = rng.normal(size=frequencies.size) + 1j * rng.normal(size=frequencies.size)
        # A broad hump around 400 Hz falling off both ways: the hiss of air through grass.
        shape = np.exp(-((np.log(np.maximum(frequencies, 1.0) / 400.0)) ** 2) / 1.2)
        shape[frequencies < 40.0] = 0.0
        return np.fft.irfft(spectrum * shape, n=total)

    time = np.arange(total) / MUSIC_RATE
    gust = 0.55 + 0.25 * np.sin(2 * math.pi * 3 * time / AMBIENCE_SECONDS) \
        + 0.20 * np.sin(2 * math.pi * 7 * time / AMBIENCE_SECONDS + 1.3)
    left = wind() * gust
    right = wind() * np.roll(gust, MUSIC_RATE // 3)
    left /= np.max(np.abs(left))
    right /= np.max(np.abs(right))
    left *= 0.35
    right *= 0.35

    # Birds: short rising-then-falling chirps, a few per loop, panned.
    for _ in range(9):
        start = int(rng.uniform(0, total))
        length = int(0.09 * MUSIC_RATE)
        age = np.arange(length) / MUSIC_RATE
        pitch = rng.uniform(2800.0, 4200.0)
        sweep = pitch * (1.0 + 0.25 * np.sin(math.pi * age / age[-1]))
        phase = 2 * math.pi * np.cumsum(sweep) / MUSIC_RATE
        chirp = np.sin(phase) * np.sin(math.pi * age / age[-1]) ** 2 * 0.12
        pan = rng.uniform(0.15, 0.85)
        for repeat in range(int(rng.integers(2, 4))):
            at = (start + repeat * int(0.14 * MUSIC_RATE) + np.arange(length)) % total
            left[at] += chirp * math.cos(pan * math.pi / 2)
            right[at] += chirp * math.sin(pan * math.pi / 2)

    interleaved = np.empty(total * 2)
    interleaved[0::2] = left
    interleaved[1::2] = right
    return [int(round(float(value) * 32767.0)) for value in np.clip(interleaved, -1.0, 1.0)]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = pathlib.Path(sys.argv[1])
    (out / "fixtures").mkdir(parents=True, exist_ok=True)
    (out / "music").mkdir(parents=True, exist_ok=True)
    (out / "ambience").mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as scratch:
        scratch_dir = pathlib.Path(scratch)
        short = scratch_dir / "short.wav"
        long = scratch_dir / "long.wav"
        short.write_bytes(wav_bytes(fixture_samples(FIXTURE_RATE), 2, FIXTURE_RATE))
        long.write_bytes(wav_bytes(fixture_samples(6 * FIXTURE_RATE), 2, FIXTURE_RATE))

        ffmpeg(short, out / "fixtures/tone.flac", "-c:a", "flac", "-compression_level", "8",
               "-metadata", "LOOPSTART=4410", "-metadata", "LOOPLENGTH=22050")
        ffmpeg(long, out / "fixtures/tone.ogg", "-c:a", "libvorbis", "-q:a", "5",
               "-metadata", "LOOPSTART=44100", "-metadata", "LOOPLENGTH=176400")
        ffmpeg(long, out / "fixtures/tone.mp3", "-c:a", "libmp3lame", "-b:a", "192k")

        music = scratch_dir / "theme.wav"
        samples = music_samples()
        music.write_bytes(wav_bytes(samples, 2, MUSIC_RATE))
        ffmpeg(music, out / "music/frontier_theme.ogg", "-c:a", "libvorbis", "-q:a", "3",
               "-metadata", "LOOPSTART=0", "-metadata", f"LOOPLENGTH={len(samples) // 2}")

        (out / "ambience").mkdir(parents=True, exist_ok=True)
        wind = scratch_dir / "wind.wav"
        samples = ambience_samples()
        wind.write_bytes(wav_bytes(samples, 2, MUSIC_RATE))
        ffmpeg(wind, out / "ambience/hillside_wind.ogg", "-c:a", "libvorbis", "-q:a", "1",
               "-metadata", "LOOPSTART=0", "-metadata", f"LOOPLENGTH={len(samples) // 2}")

    for path in sorted(out.rglob("*")):
        if path.is_file() and path.suffix in {".flac", ".ogg", ".mp3"}:
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            print(f"{path.relative_to(out)}  {path.stat().st_size:>8}  sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
