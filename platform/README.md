# `platform/` — layer 3, the porting surface

Per-platform implementations of the engine-owned interfaces. One directory per platform backend;
platform-specific code lives here and **never behind an `#ifdef` in a shared file**. The rule is
checked by `just quality-layers`.

| Directory | Provides |
|---|---|
| `desktop-sdl3/` | `Platform`, `DisplayServer` and the input event source for Linux, Windows and macOS, over SDL3 |
| `headless/` | The same interfaces with no window system — required by the specification and by CI |

A port implements `Platform`, `DisplayServer`, an input backend, an audio backend, a graphics
surface provider per enabled RHI backend, and packaging support. It requires **no** change in
`src/core/`, `src/ecs/`, `src/servers/` or `src/scene/`; if it does, the abstraction is wrong.

No SDL type appears above this directory. Native per-platform backends are scheduled at M11, so the
abstraction is validated against a second implementation before 1.0 — see `design.md` §4.

**Governed by**: `core-platform-abstraction`, `build-system-and-platforms` (platform porting
surface).
