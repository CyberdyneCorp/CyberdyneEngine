# Proposal: the Swift gaps M12 needs — scheduled systems, tree callbacks, bodies and characters

## Why

M12 is an RTS written in Swift. `docs/guides/swift.md` ("What is not available yet") lists what a
game cannot plan around at ABI 1.4, and three of those items are things an RTS cannot do without:

- **Engine-scheduled Swift systems.** `@System` derives an access declaration from its signature,
  but nothing hands the declaration to the engine: there is no `register_system` entry, and
  `ChunkSource` has no conformance over `world_chunks`. Bulk unit logic either runs per instance in
  a behaviour or not at all.
- **The tree callbacks.** `onEnterTree`, `onReady`, `onEnable`, `onDisable` and `onExitTree` are
  declared and recorded by the `@Behaviour` macro, and never called; `@Node(path)` is always nil.
- **Forces, impulses and a character controller.** Physics is queries only; a Swift game cannot
  push a body or walk a character, though the engine has both (`PhysicsServer::add_force` and
  friends, `cy::physics::CharacterController`).

## What Changes

- **ABI 1.5**, appended: `register_system` with `CySystemDesc` / `CySystemAccess` / `CyAccessMode`;
  `node_find`; `physics_apply_force`, `physics_apply_impulse`, `physics_apply_torque`,
  `physics_set_velocity`, `physics_get_velocity`; `character_create`, `character_destroy`,
  `character_move`, `character_state` with `CyCharacterDesc` / `CyCharacterInput` /
  `CyCharacterState` / `CyGroundState`; and `enter_tree`, `ready`, `enable`, `disable`, `exit_tree`
  appended to `CyBehaviourVTable`. `CY_ABI_MINOR` 5. Each entry states its phases and determinism.
- **The engine side**: thunks against new abstract backends (`SceneBackend`, `PhysicsBodyBackend`,
  `CharacterBackend`) on the host; a system registry on the host and `cy::abi::ScriptSystems`, which
  installs each registered system into an `ecs::Schedule` stage and runs a stage under its phase;
  `BehaviourRuntime::tree_callback`; a reload that changes a scheduled system's stage or access is
  refused (`ReloadFailure::SystemChanged`).
- **Adapters** in `cy::game-backend`: `PhysicsBodyAdapter`, `CharacterAdapter`, and
  `ScriptSceneBridge`, which makes every script behaviour type a scene behaviour of the same name so
  the scene tree's pump drives the tree callbacks, and answers `node_find` against the tree.
- **The scene tree** delivers `onEnterTree` once per attachment: a node created under a parent
  attached in the same frame was entered twice (a regression test proves it).
- **CyberdyneKit**: `@System` functions listed in `GameModule.systems` are registered with the
  engine and run over `EngineChunkSource` (a `world_chunks` join by archetype); `GameModule.components`
  are registered first; the bridge registers and dispatches the tree callbacks; `@Node` resolves at
  `ready` through `node_find`; `SceneTree.find`, `RigidBody` and `CharacterController` facades.
- **`samples/13-rts-api`** uses a scheduled system, the tree callbacks with `@Node`, a character
  controller and an impulse, with its integration test, the existing negative control extended, and
  a new `--no-systems` control.
- The Swift overlay, the Rust SDK and the ABI baseline are regenerated.

## Capabilities

### Modified Capabilities

- `native-abi`: scheduled module systems; node paths; rigid-body writes; character controllers;
  the tree callbacks in the behaviour vtable.
- `swift-scripting`: engine-scheduled systems; tree callbacks driven and `@Node` resolved; bodies
  and characters from Swift.

## Impact

- `src/abi/` (header, thunks, host, systems, module), `src/game_backend/`, `src/scene/` (the enter
  fix), `bindings/swift/`, the generated Swift overlay and Rust SDK, `samples/13-rts-api`, and the
  guides. Append-only: `just quality-abi` classifies every change as an append.
