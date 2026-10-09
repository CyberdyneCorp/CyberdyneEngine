# animation-and-skinning Spec Delta

## ADDED Requirements

### Requirement: Authored events reach a game's clips
A clip's events SHALL be replaceable without changing its tracks or keys, so an event authored outside
the source asset can be cooked into the clip a game loads. A clip cooked with authored events SHALL
emit them, in time order, on the tick that crosses each.

#### Scenario: Cleared events fire nothing
- **WHEN** a clip's events are cleared
- **THEN** its tracks and keys SHALL be unchanged and playback SHALL emit no event until one is added

#### Scenario: A baked clip emits the authored events
- **WHEN** a clip baked with an event at 0.26 s is advanced in sixtieths of a second
- **THEN** the event SHALL be emitted on the sixteenth tick
