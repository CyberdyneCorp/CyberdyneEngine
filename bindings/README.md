# `bindings/` — generated language overlays

Language bindings generated from the ABI description in `src/abi/`.

- `swift/` — the generated Swift overlay and the `CyberdyneKit` package

The Swift overlay is **committed**, unlike other generated artefacts, so that a consumer needs no
generator to build against the engine. `just generate-swift --check` asserts the committed overlay
matches what regeneration produces, and it also runs as `integration.swift_overlay` under
`just test-all` — on every machine, with or without a Swift toolchain, because a stale overlay is
everybody's problem and not only that of the machines that can build Swift.

It has its own recipe rather than joining `generate-check` because it needs no configured build
directory: `cy_features.h` and `cy_modules.h` are a function of the *configuration*, and the overlay
is a function of `src/abi/include/cy/abi/cy_abi.h` alone.

**Governed by**: `swift-scripting`, `native-abi`, `build-system-and-platforms` (code generation).
Landed at M4.
