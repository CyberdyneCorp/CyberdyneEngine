## MODIFIED Requirements

### Requirement: Audio assets and streaming
The engine SHALL support: uncompressed PCM, and compressed **Vorbis** and **Opus** for streamed
music, plus a lightweight ADPCM or similar for short effects. Cooked tracks SHALL also carry FLAC and
MP3 bitstreams kept from their sources, so a streamed track is never re-encoded at cook time.

Assets SHALL declare a load mode: **fully decoded in memory** (short sounds), **compressed in
memory, decoded on demand** (medium), or **streamed from disk** (music and ambience).

Streaming SHALL decode ahead into a ring buffer on the asset thread, with underrun producing
silence and a diagnostic rather than a stall. A stream's decoding, its rate and channel conversion,
and every read of its source SHALL run as jobs — reads that block on storage on the asynchronous
service — and never on the audio thread; the audio thread SHALL only drain the ring. A refill SHALL
fill the ring to capacity, so a stream SHALL NOT underrun while no refill is delayed by more than the
ring's length less one mixer block.

A stream's gain SHALL be an envelope the audio thread evaluates per frame, so a crossfade between two
tracks is equal-power and both of its ramps start on the same sample. A seek SHALL fade the old
position out over one mixer block, refill the ring from the new position, and ramp the new position
in once the ring is primed.

#### Scenario: Music streams
- **WHEN** a long music track plays
- **THEN** it SHALL be streamed and decoded ahead, with only the ring buffer resident

#### Scenario: Seamless loop
- **WHEN** a track declares loop points
- **THEN** looping SHALL be sample-accurate with no gap or click

#### Scenario: A starved job system within the buffer
- **WHEN** every refill of a stream is delayed by up to the ring's length less one mixer block
- **THEN** the stream SHALL play every frame in order with no underrun

#### Scenario: Underrun is silence and a diagnostic
- **WHEN** a stream's refills are delayed past the ring's length
- **THEN** the missing frames SHALL play as silence, the underrun SHALL be counted and reported as a
  diagnostic naming the track, and the stream SHALL continue once refills arrive

#### Scenario: Seek is sample-accurate
- **WHEN** a stream seeks to a position in its track
- **THEN** the frames it plays after the seek's ramp SHALL be the track's frames from that position,
  and a decoder's seek SHALL deliver the same samples a decode from the start delivers there

#### Scenario: Crossfade is equal-power
- **WHEN** one track crossfades into another
- **THEN** the outgoing gain SHALL fall as cos(t·π/2) and the incoming rise as sin(t·π/2), starting on
  the same sample, so the sum of their squares stays one, and the outgoing stream SHALL stop itself
  at the end of its ramp
