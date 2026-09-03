## MODIFIED Requirements

### Requirement: Persistence overlay
Runtime changes to the world SHALL be recorded in a **persistence overlay**, and cooked cells SHALL
remain immutable:

```
authored cells + persistence overlay = current world
```

The overlay SHALL record: entities created and removed, component values changed, layer states,
dynamic entity positions at checkpoints, and world state variables.

One overlay mechanism SHALL serve save games, dedicated server persistence, replays, and the
editor's play-mode changes. Its **encoding, journalling, atomicity, incremental writing, migration,
and storage backends** are defined in `save-and-persistence`; this capability defines the model the
world maintains.

The overlay SHALL be organised so that the persistent state of **unloaded regions** is available
without loading them, so that saving a world of which most is unloaded requires no additional
streaming.

Applying an overlay to authored cells SHALL be deterministic, and SHALL occur **during cell
activation** rather than by instantiating authored content and then correcting it.

An overlay SHALL declare the content version it was produced against so incompatibility is detected
rather than misapplied.

#### Scenario: A destroyed building stays destroyed
- **WHEN** a building is destroyed and the game is saved and reloaded
- **THEN** the authored cell SHALL be loaded unchanged and the overlay SHALL remove the building

#### Scenario: Content update after a save
- **WHEN** a save's content version does not match the installed content
- **THEN** the mismatch SHALL be detected and reported, not silently applied

#### Scenario: Saving does not stream the world
- **WHEN** a world with most regions unloaded is saved
- **THEN** the persistent state of unloaded regions SHALL be available without loading them
