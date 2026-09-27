# Proposal

## Why

M12 is an RTS written in Swift against `CyberdyneKit`. Today the C interface table (ABI 1.2) exposes
only the engine-neutral core — diagnostics, values, entities, components, behaviours, the hierarchy,
chunks and editor services — and `samples/04-character/game/Contract.swift` records the consequence:
input, physics, cameras and audio "are appended by the subsystems that own them ... which has not
happened yet", so a Swift game reaches them only through components a C++ host carries across for
it. An RTS cannot be written that way. Selecting a unit is a pointer read, a camera ray and a
physics query in one frame; ordering it is a path request and a crowd move; building is a prefab
spawn; feedback is a positioned sound. Every one of those needs a C++ host written per game, which
is the thing Swift scripting exists to avoid.

## What Changes

- Append **ABI 1.3**: 38 entries at the end of `CyInterface` for time, input, camera, physics
  queries, navigation, audio and spawning, with their POD structs, handle typedefs and enums, and a
  `frame_update` callback appended to `CyBehaviourVTable`. Nothing existing is reordered, renamed or
  re-typed; the ABI gate accepts it as an append.
- Give every new entry an explicit **phase rule** (`N`one, `F`ixed update, frame `U`pdate), refused
  with `CY_RESULT_PERMISSION_DENIED` in every build, and a **determinism rule**: anything callable in
  a fixed step answers from simulation state with a total order on every list; device and
  presentation state are frame-update only.
- Introduce a **backend seam**: the thunks live in `cy_abi` and name no server; each service is an
  abstract `cy::abi::game::*Backend` bound on `cy::abi::Host::game`, implemented by adapters in a new
  `src/game_backend/` module over the existing servers.
- Regenerate the Swift overlay and the Rust SDK from the same description; add the Swift value types
  and the fake-engine test harness the `CyberdyneKit` facades share.
- Implement every entry: the thunks, one adapter per service in `src/game_backend/`, and the
  `CyberdyneKit` facades, on disjoint files per group as `design.md` sets out.
- Add `samples/13-rts-api`, an RTS unit written only in Swift over the new entries.

## Capabilities

### New Capabilities

None. The surfaces belong to existing capabilities.

### Modified Capabilities

- `native-abi`: the append-only interface gains the game-service entries, their update-phase rules,
  and the behaviour frame-update callback.
- `swift-scripting`: `CyberdyneKit` gains facades over those entries, tested through the package.

## Impact

- `src/abi/` (header, table, thunks, host), new `src/game_backend/`, `bindings/swift/`,
  `editor/crates/cy-editor-sdk/src/generated/`, `tools/gen/`.
- New `samples/13-rts-api/`: an RTS unit written only in Swift over the new entries, with a headless
  integration test driven by synthetic input — the end-to-end proof M12 builds on.
- A module compiled against 1.3 requires an engine exporting at least 1.3; modules compiled against
  1.0–1.2 are unaffected.
- `input-and-actions`, `physics`, `camera-system`, `navigation`, `audio`, `serialization-and-prefabs`
  and `simulation-and-determinism` are consumed as they stand; none of their requirements change.
