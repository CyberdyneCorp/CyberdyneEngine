# native-abi Spec Delta

## ADDED Requirements

### Requirement: Game service interface
The append-only interface SHALL expose the gameplay-facing verbs of the input, camera, physics query,
navigation, audio and spawning subsystems, and the engine clock, as appended entries of one minor
version. Every entry SHALL return values, caller-owned structs or caller-supplied buffers; SHALL
report failure as a `CyResult` with the last-error message set; SHALL identify engine objects by
integer handles or entities, never by engine addresses; and SHALL document its permitted update
phases, its determinism, its ownership and its errors.

Variable-length results SHALL use the sizing pattern of `world_chunks`: the total is always
reported, a null buffer asks for it, and a buffer too small is filled with the first entries in the
stated order and answers `CY_RESULT_BUFFER_TOO_SMALL`.

The ABI layer SHALL reach each subsystem only through an abstract backend bound on the host, so that
the ABI module depends on no subsystem and an unbound service answers `CY_RESULT_UNAVAILABLE`.

#### Scenario: Selecting a unit from Swift
- **WHEN** a Swift behaviour in frame update reads the pointer, asks the active camera for the ray
  through it, and ray-casts that ray against the physics world
- **THEN** each call SHALL succeed through the interface table alone, and the hit SHALL name the
  entity owning the body that was hit

#### Scenario: Service not bound
- **WHEN** a module calls an audio entry on an engine with no audio backend bound
- **THEN** the call SHALL return `CY_RESULT_UNAVAILABLE` with a message naming the service, and
  SHALL NOT crash

#### Scenario: No engine address escapes
- **WHEN** any game-service entry returns
- **THEN** every value it wrote SHALL be a scalar, an entity, an integer handle, or a struct of
  those, and none SHALL be a pointer into engine memory

#### Scenario: Stale voice handle
- **WHEN** `audio_stop` is called with a voice that ended and whose slot a later voice now uses
- **THEN** it SHALL return `CY_RESULT_OK` and the later voice SHALL keep playing

#### Scenario: A queued path too long for the buffer is not lost
- **WHEN** `nav_poll_path` finds a READY path with more points than the caller's buffer holds
- **THEN** it SHALL return `CY_RESULT_BUFFER_TOO_SMALL` with the path's point count, write no
  points, and leave the query READY, so that a poll with a large enough buffer receives the path

#### Scenario: Arrival is an event for one tick
- **WHEN** a crowd agent sent with `nav_agent_move_to` reaches its target
- **THEN** `nav_agent_state` SHALL report ARRIVED with `CY_NAV_AGENT_EVENT` set until the next
  fixed step's navigation update, and ARRIVED without it afterwards

#### Scenario: Batch spawn is all or nothing
- **WHEN** `spawn_instantiate_many` fails while creating one of its instances
- **THEN** no instance of the batch SHALL remain, the caller's roots SHALL be untouched, and the
  world's structural epoch SHALL be unchanged

#### Scenario: Older module on a 1.3 engine
- **WHEN** a module compiled against ABI 1.2 loads into an engine exporting 1.3
- **THEN** its table prefix SHALL be unchanged, its behaviour vtable SHALL register with no frame
  callback, and it SHALL run unmodified

### Requirement: Update phases are enforced at the boundary
The engine SHALL track the update phase — none, fixed update, frame update — and every game-service
entry SHALL refuse a call made outside its permitted phases with `CY_RESULT_PERMISSION_DENIED` and a
message naming the entry and the phase, in every build configuration.

An entry permitted during fixed update SHALL answer from simulation state only, with every
multi-result list in a stated total order, so that the same world, tick and inputs produce
bit-identical answers. Device state and camera reads SHALL NOT be permitted during fixed update.
Presentation writes issued while the tick is a resimulation SHALL succeed and have no effect.

#### Scenario: Pointer read in a fixed step
- **WHEN** a behaviour's fixed update calls `input_pointer`
- **THEN** the call SHALL return `CY_RESULT_PERMISSION_DENIED` and leave its output untouched

#### Scenario: A fast press read in a fixed step replays
- **WHEN** a key bound to an action goes down and up inside one tick, and a fixed update reads the
  action with `input_action_state`
- **THEN** it SHALL read `press_count` 1 and `release_count` 1, and a replay that feeds the same
  events to a fresh input server SHALL read the same state

#### Scenario: Resimulated camera write
- **WHEN** `camera_set_target` is called during a fixed update whose tick is flagged as a
  resimulation
- **THEN** it SHALL return `CY_RESULT_OK` and the camera rig's target SHALL be unchanged

#### Scenario: Deterministic query order
- **WHEN** `physics_raycast_all` is called twice in fixed update over the same world and two hits
  are at equal distance
- **THEN** both calls SHALL return the same hits in the same order, ordered by entity value

#### Scenario: Deterministic path completion
- **WHEN** `nav_request_path` queues the same search on the same tick in two runs
- **THEN** `nav_poll_path` SHALL first report it READY on the same tick in both runs, with
  bit-identical points and result

#### Scenario: Order of a tick's move orders does not matter
- **WHEN** one fixed update's scripts send the same agents to the same targets in a different order
- **THEN** every agent's state SHALL be bit-identical on every later tick, because orders take effect
  in the navigation update, in entity order

#### Scenario: Resimulated tick plays no sound
- **WHEN** `audio_play` is called during a fixed update whose tick is flagged as a resimulation
- **THEN** it SHALL return `CY_RESULT_OK`, write a null voice, and start no voice

#### Scenario: Spawning refused in a frame update
- **WHEN** `spawn_instantiate`, `spawn_instantiate_many` or `spawn_destroy` is called during frame
  update
- **THEN** it SHALL return `CY_RESULT_PERMISSION_DENIED` and create or destroy nothing

#### Scenario: Deterministic spawning
- **WHEN** the same sequence of spawn calls runs in fixed update on two fresh worlds
- **THEN** both runs SHALL produce the same entities, in the same order

#### Scenario: Frame delta hidden from a fixed step
- **WHEN** `time_get` is called during fixed update
- **THEN** it SHALL report the tick being simulated and the fixed delta, and SHALL report the frame
  delta and interpolation as zero

### Requirement: Behaviour frame callback
`CyBehaviourVTable` SHALL carry a `frame_update` callback, appended after its existing members and
called once per frame during frame update with the frame's delta. A vtable that does not provide it
SHALL never be scheduled for frame updates.

#### Scenario: Behaviour without a frame callback
- **WHEN** a behaviour type registers with `frame_update` null, or with a `struct_size` that predates
  it
- **THEN** the engine SHALL not dispatch frame updates to its instances
