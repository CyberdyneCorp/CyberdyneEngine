## 1. ABI 1.5

- [x] 1.1 Append the entries, structs, enums and vtable members to `cy_abi.h`; `CY_ABI_MINOR` 5; layout asserts; `just quality-abi` classifies it as appends; baseline, Swift overlay and Rust SDK regenerated.
- [x] 1.2 Thunks for systems, node paths, bodies and characters against abstract backends; `unit.abi` cases for each (`test_game_systems.cpp`, `test_game_scene.cpp`, `test_game_bodies.cpp`, `test_game_character.cpp`), the table shape and the 1.5 layouts.
- [x] 1.3 Host system registry and `cy::abi::ScriptSystems`; reload refusal for a changed declaration (`integration.abi_reload` over three C module builds).
- [x] 1.4 `BehaviourRuntime::tree_callback`, with the pre-1.5 vtable prefix rule.

## 2. Adapters

- [x] 2.1 `PhysicsBodyAdapter` (`integration.game_backend_bodies`).
- [x] 2.2 `CharacterAdapter` (`integration.game_backend_character`).
- [x] 2.3 `ScriptSceneBridge` and `BehaviourDesc::user` (`integration.game_backend_scene`).
- [x] 2.4 `onEnterTree` once per attachment, with a regression test in `unit.scene` (`test_lifecycle.cpp`).

## 3. CyberdyneKit

- [x] 3.1 Systems registered with the engine; `EngineChunkSource`; `GameModule.components` and `.systems`; `SystemRegistration` (`SystemEngineTests`).
- [x] 3.2 Tree callback thunks; `@Node` resolution at `ready`; `SceneTree.find` (`TreeCallbackTests`).
- [x] 3.3 `RigidBody` and `CharacterController` (`BodiesTests`).

## 4. Sample, docs, proofs

- [x] 4.1 `samples/13-rts-api`: a scheduled system, tree callbacks with `@Node`, a character and an impulse; `--no-systems`; the integration test and the controls.
- [x] 4.2 `docs/guides/swift.md`, `bindings/swift/README.md`, `src/abi/README.md`, `docs/guides/physics.md`, `requirements-coverage.toml`.
- [x] 4.3 Build, run the suites, and record mutation proofs in `evidence/falsification.md`.
