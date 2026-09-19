# `platform/` — layer 3, the porting surface

Per-platform implementations of the engine-owned interfaces. One directory per platform backend;
platform-specific code lives here and **never behind an `#ifdef` in a shared file**. The rule is
checked by `just quality-layers`.

| Directory | Provides |
|---|---|
| `desktop-sdl3/` | `Platform`, `DisplayServer` and the input event source for Linux, Windows and macOS, over SDL3 |
| `headless/` | The same interfaces with no window system — required by the specification and by CI |
| `linux-native/` | The same interfaces **natively** on Linux — POSIX and Xlib/XRandR, with no SDL3 beneath them. M11.d's second implementation, which is what proves the abstraction carries no SDL assumption |
| `stub/` | The same interfaces sharing **no desktop assumption at all**: false to every `Feature`, one unresizable window, one writable directory, no command line, no subprocesses, no dynamic loader, one core |
| `host/` | Not a backend: the desktop host's `while (running)` that calls `Runtime::tick()` |

A port implements `Platform`, `DisplayServer`, an input backend, an audio backend, a graphics
surface provider per enabled RHI backend, and packaging support. It requires **no** change in
`src/core/`, `src/ecs/`, `src/servers/` or `src/scene/`; if it does, the abstraction is wrong.

No SDL type and no X11 type appears above this directory — `just quality-layers` refuses both, by the
same check and for the same reason. **The second implementation landed at M11.d** and the finding it
produced is in `linux-native/README.md`: what the port could do without changing a line above layer
3, and the three places the interface pushed back. SDL3 was **not** deleted and is not going to be;
two implementations is the proof, and one is the position this project was in for eleven
milestones.

**Governed by**: `core-platform-abstraction`, `build-system-and-platforms` (platform porting
surface).
