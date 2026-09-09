## ADDED Requirements

### Requirement: Play sessions
Entering play SHALL build a simulation from the authored world — an entity per authored node, its
placement, and the physics components the world declares — and SHALL report what it built: how many
entities, how many bodies, and how many declared components this build did not understand.

A play session SHALL step at the fixed simulation rate, in the `Physics` stage, and SHALL make the
simulated placement of each node visible to whatever presents the world, so that a viewport drawing
the authored world draws the simulation without knowing a session exists.

Leaving play SHALL restore the authored world **exactly**, and the restoration SHALL be VERIFIED
rather than asserted: the session SHALL compare the world's serialized bytes against the bytes it
held before play and SHALL report whether they are identical. A session that cannot restore exactly
SHALL restore the whole snapshot and SHALL say that it had to.

Entering play twice, pausing what is not playing, resuming what is not paused, and stopping what is
not running SHALL each be answered by name; stopping a session that is not running SHALL succeed,
because a host's teardown calls it unconditionally.

A session SHALL NOT create the physics backend it simulates in. Which backend a project uses is the
host's decision.

#### Scenario: The world the editor authored is the world that simulates
- **WHEN** play is entered over a world holding a static box and a dynamic sphere above it
- **THEN** the session SHALL report two entities and two bodies, and the sphere SHALL come to rest on
  the box

#### Scenario: Stopping leaves no residue
- **WHEN** a play session runs and is then stopped
- **THEN** the authored world's bytes SHALL be identical to what they were before play, and a second
  session over the same world SHALL reproduce the first exactly

#### Scenario: A play state this build does not know is refused by name
- **WHEN** a caller asks for a play state that does not exist
- **THEN** the session SHALL stay where it was and SHALL answer with the state actually in force
