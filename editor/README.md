# `editor/` — layer 7

The editor application, built on Scene + Runtime, tools builds only. With `CY_BUILD_EDITOR=OFF` this
directory is excluded from the build and `CY_EDITOR` is undefined, so code guarded by `#if CY_EDITOR`
is absent from the binary.

The editor application is a **Rust** workspace driving the engine through its C ABI; Cargo owns its
compilation and the engine build does not reimplement it.

**Governed by**: `editor-architecture`, `editor-rust-application`, `editor-documents-and-transactions`,
`editor-ui-ux`, `editor-viewport-and-gizmos`. Arrives at M5; the layer slot is reserved now.
