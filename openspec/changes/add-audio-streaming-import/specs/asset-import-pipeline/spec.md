## MODIFIED Requirements

### Requirement: Audio import
The audio importer SHALL produce cooked audio with: format selected by intended use (decoded PCM,
in-memory compressed, or streamed), sample rate conversion, channel handling (force mono for 3D
sources), loop point preservation, normalisation, and silence trimming.

It SHALL read WAV, Ogg Vorbis, FLAC and MP3 sources, MP3 with its gapless delay and padding removed.
A track cooked for memory-compressed or streamed use SHALL keep its source's compressed bitstream
unchanged, with where play starts, how many frames play and where it loops recorded beside it; a
WAV source, a decoded track, and a source that must be downmixed for a 3D emitter SHALL be cooked to
PCM. Loop points SHALL come from the importer's options or from the source's own metadata — a WAV
`smpl` loop, or `LOOPSTART` with `LOOPLENGTH` or `LOOPEND` comments. The importer SHALL measure the
track's ITU-R BS.1770 integrated loudness and sample peak and record them; normalisation SHALL be a
playback gain recorded beside them, capped so the normalised peak stays under a target peak, and
never a rewrite of the samples. Cooking SHALL be deterministic: one source and one set of options
produce the same bytes.

#### Scenario: 3D sound forced to mono
- **WHEN** a stereo file is imported for use as a 3D source
- **THEN** it SHALL be downmixed to mono, since stereo cannot be spatialised meaningfully

#### Scenario: A stereo WAV for a 3D source
- **WHEN** a one-second stereo 44.1 kHz WAV with a `smpl` loop is imported for a 3D source at 48 kHz
- **THEN** the cooked payload SHALL be mono, exactly 48 000 frames long, with its loop points
  converted by the same ratio

#### Scenario: A streamed track keeps its bitstream
- **WHEN** a long Ogg Vorbis track is imported for streaming
- **THEN** the cooked payload SHALL be its bitstream byte for byte, with its loop comments carried as
  the cooked loop

#### Scenario: Normalisation is a gain
- **WHEN** a track is normalised to a loudness its peak cannot take
- **THEN** the recorded gain SHALL stop at the target peak, the importer SHALL say so, and the
  payload SHALL be the same bytes as without normalisation
