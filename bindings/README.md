# `bindings/` — generated language overlays

Language bindings generated from the ABI description in `src/abi/`.

- `swift/` — the generated Swift overlay and the `CyberdyneKit` package

The Swift overlay is **committed**, unlike other generated artefacts, so that a consumer needs no
generator to build against the engine. `just generate-check` asserts the committed overlay matches
what regeneration produces; a stale overlay is a build failure, not a runtime surprise.

**Governed by**: `swift-scripting`, `native-abi`, `build-system-and-platforms` (code generation).
Arrives at M4.
