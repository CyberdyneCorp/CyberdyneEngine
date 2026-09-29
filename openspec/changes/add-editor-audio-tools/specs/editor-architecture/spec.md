# editor-architecture Spec Delta

## ADDED Requirements

### Requirement: Audio buses and mixing editor
The audio buses and mixing editor SHALL edit the project's bus graph — each bus's output, gain,
mute, solo, bypass, sends and effect chain — as one undoable transaction per edit, and each edit
SHALL be an agent tool that does the same thing. The editor SHALL refuse an edit whose routing or
sends would form a cycle before saving it. A saved mixer SHALL be applied to the engine's audio
server, and the editor SHALL show each bus's gain, routing and level as the engine reports them
rather than as it sent them. The editor SHALL offer only the effect kinds the engine declares, and
SHALL refuse to open, naming the owning capability, until the engine has declared them.

#### Scenario: A gain change reaches the engine
- **WHEN** an author sets a bus's volume in the mixer editor or through its agent tool
- **THEN** the engine's bus SHALL hold that gain, the mix through it SHALL scale by it, and the voice playing through it SHALL continue

#### Scenario: A cycle is refused
- **WHEN** a send or route would make a bus feed itself
- **THEN** the edit SHALL be refused naming the cycle, and neither the saved mixer nor the engine's graph SHALL change

#### Scenario: Undo restores the engine's mix
- **WHEN** an author undoes a mixer edit
- **THEN** the mixer asset SHALL return to its prior text and the engine SHALL be sent that text

### Requirement: Audio cue preview
The editor SHALL author audio cues — a clip, its bus, gain, pitch, variation and looping — as
undoable project assets, and SHALL preview a cue by asking the engine to play it through the
project's mixer. The engine SHALL report whether the preview voice started, and, for a spatial
preview, the distance, attenuation gain and pan its own curves applied.

#### Scenario: A preview is an engine voice
- **WHEN** an author previews a cue
- **THEN** the engine SHALL start a voice for it on the cue's bus and report it playing

### Requirement: Audio during editor play
Entering play in the editor SHALL apply the project's mixer and start every enabled, autoplaying
audio source in the authored world, spatialised at its transform and heard from the camera play
renders from. Gameplay code SHALL reach the same audio server, the project's cues and its buses
during play. Pausing play SHALL hold those voices and stopping play SHALL end them. The play report
SHALL name the audio backend, so a mix on the null backend is not mistaken for a device.

#### Scenario: Play sounds the world
- **WHEN** play starts on a world with an autoplaying audio source
- **THEN** the engine SHALL be mixing that source, and stopping play SHALL end it

#### Scenario: Swift plays a project cue
- **WHEN** a Swift behaviour plays a cue by name during play
- **THEN** the cue SHALL resolve to the project's cue asset and play through the project's mixer
