# swift-scripting Spec Delta

## ADDED Requirements

### Requirement: Game service facades
`CyberdyneKit` SHALL provide hand-written facades over the ABI's game-service entries — `Input`,
`Camera`, `Physics`, `Navigation`, `Audio`, `Spawn` and `Time` — taking and returning Swift values
(`Vec3`, `Pose`, `Ray`, `Entity`, optionals and arrays) rather than C structs and out-parameters.

A facade SHALL surface an ABI failure, including a phase refusal and an unbound service, as a thrown
`CyberdyneError` carrying the status and the engine's message, and SHALL throw rather than trap when
no engine is bound. Facades SHALL be tested through `CyberdyneKit` against an interface table the
test constructs, so that the package's own tests exercise them without an engine.

#### Scenario: A pick in a few lines
- **WHEN** a behaviour's frame update picks the unit under the pointer
- **THEN** it SHALL be expressible as a pointer read, a camera ray and a physics ray cast returning an
  optional hit, with no C types in the game's code

#### Scenario: Refused phase becomes a Swift error
- **WHEN** a facade call is refused by the engine because of the update phase
- **THEN** the facade SHALL throw `CyberdyneError.status(.permissionDenied, message:)` with the
  engine's message

#### Scenario: Facade without an engine
- **WHEN** a facade is called in a process where no engine has bound the module
- **THEN** it SHALL throw `CyberdyneError.status(.unavailable, message:)` rather than trap

#### Scenario: An RTS unit written only in Swift
- **WHEN** a game module's behaviour pans a camera from keys and the pointer, selects the unit
  under a click, sends it to a clicked ground point, plays a cue when it arrives and spawns a unit
  from a prefab on a key, and a host that binds the game-service backends drives it with synthetic
  input
- **THEN** every one of those outcomes SHALL be produced by the behaviour's facade calls, with no
  host code reading or writing a gameplay value, and the same host with the behaviour absent SHALL
  produce none of them

### Requirement: Frame update reaches Swift behaviours
A Swift behaviour that overrides `onUpdate` SHALL receive it once per frame, during frame update,
through the behaviour vtable's frame callback; one that does not override it SHALL register no frame
callback.

#### Scenario: onUpdate is dispatched
- **WHEN** a behaviour overrides `onUpdate` and the engine runs a frame
- **THEN** `onUpdate` SHALL be called with the frame's delta while the engine reports the frame
  update phase
