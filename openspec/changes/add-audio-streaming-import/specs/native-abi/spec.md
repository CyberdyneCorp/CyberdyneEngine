## ADDED Requirements

### Requirement: Streamed music and ambience through the ABI
ABI 1.18 SHALL append `audio_find_track`, `audio_stream_play`, `audio_stream_stop`,
`audio_stream_seek`, `audio_stream_set_volume` and `audio_stream_state`, with `CyAudioTrack`,
`CyAudioStream`, `CyAudioStreamPlay` and `CyAudioStreamState`. A track SHALL be one the host
registered by name; `audio_stream_play` SHALL start it, and with `crossfade_from` naming a playing
stream SHALL fade that stream out over the same length as the new one fades in. Every entry SHALL be
allowed in every phase, as audio is presentation, and while the tick is a resimulation the writes
SHALL answer `CY_RESULT_OK` and do nothing, `audio_stream_play` handing back a null stream. A stream
handle SHALL never name a later stream once its own has ended. A backend with no stream player SHALL
answer `CY_RESULT_UNAVAILABLE`. Bus volumes SHALL remain ABI 1.3's `audio_set_bus_volume`. The Swift
overlay SHALL expose the entries through CyberdyneKit's `Audio.track`, `Audio.stream`,
`Audio.crossfade` and `AudioStream`.

#### Scenario: A crossfade from Swift
- **WHEN** a game streams one track and then plays another with `crossfade_from` naming the first
- **THEN** the second SHALL fade in while the first fades out, and the first SHALL end at the end of
  the fade, `audio_stream_state` then answering `CY_RESULT_NOT_FOUND` for it

#### Scenario: A resimulated tick starts no music
- **WHEN** `audio_stream_play` is called while the tick is being resimulated
- **THEN** it SHALL answer `CY_RESULT_OK` with a null stream and start nothing
