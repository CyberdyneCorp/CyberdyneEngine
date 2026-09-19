# `platform/linux-native/` — the second implementation, and what it found

`core-platform-abstraction` has had **one** implementation of `Platform` and **one** of
`DisplayServer` since M0, and both are SDL3's. An interface implemented once is an interface nobody
has tested: every assumption its first implementation happens to satisfy is invisible until a second
one refuses it. This directory is the second one — **POSIX and Xlib/XRandR, with no SDL3 beneath
it** — and this file is the finding it produced, which is worth more than the code.

| File | What it implements |
|---|---|
| `src/linux_platform.cpp` | `cy::Platform` over POSIX and glibc: `fork`/`execvp` with pipes, `dlopen`, `clock_gettime`, `sigaction`, `/proc`, XDG directories |
| `src/x11_display_server.cpp` | `cy::DisplayServer` over Xlib, XRandR and EWMH, with the Vulkan surface reached through the loader by name |
| `src/x11_input_source.cpp` | The platform end of `input-and-actions`' boundary, over X11 keycodes as physical keys |

**SDL3 is not replaced and is not going to be.** `platform/desktop-sdl3/` stays, stays tested, and
stays the default. Two implementations is the proof; deleting one would leave the interface proved by
exactly as many as before.

## The exit criterion: the diff

> *"A port implements `Platform`, `DisplayServer`, an input backend, an audio backend and a graphics
> surface provider, with no changes required in `src/core/` or above."* — `core-platform-abstraction`
>
> *"if it does, the abstraction is wrong."* — `platform/README.md`

**It did not.** Nothing under `src/core/`, `src/ecs/`, `src/servers/` or `src/scene/` was touched to
make this backend exist. The check is mechanical and is `tools/ci/port_engine_layer_diff.py`, run
against the working tree while the port was being written and against the commits that carried it by
`m11d:port-touches-no-engine-layer`. The files this port added or changed are:

- `platform/linux-native/` and `platform/stub/` — new, layer 3;
- `platform/CMakeLists.txt`, `platform/README.md` — the directory's own index;
- `samples/00-empty/` — a `--platform` flag, layer 7, so that the M0 sample can be run on it;
- `tests/integration/test_stub_frame.cpp` — layer 7;
- `tools/layercheck/`, `tools/ci/`, `just/` — the gates and the recipes.

That is the whole of it. **The abstraction held.**

## What the interface got right, measured rather than assumed

- **`Platform::initialise()` needed no handshake.** This implementation's is a field assignment and
  nothing else: every service the interface promises is one the operating system already provides,
  and SDL3's `SDL_Init()` is there because SDL3 is a library, not because the interface demanded a
  context object or a subsystem mask. An interface shaped around its first implementation would have
  demanded one.
- **The buffer convention survived contact.** Every text-returning call takes a caller-owned buffer,
  never allocates, and answers `BufferTooSmall` rather than truncating. Reimplementing it over
  `getenv`, `readlink` and the XDG variables produced no case where the convention got in the way.
- **`has_feature()` did all the work it was designed for.** Six capabilities this backend genuinely
  cannot offer — exclusive fullscreen, per-screen DPI, mouse pass-through, adaptive and mailbox
  V-sync, and the whole system-integration group — are *reported* rather than faked or refused at
  window creation, and the window is still created. Every one of them is a `case` with its reason
  beside it in `x11_display_server.cpp`.
- **Separating `Platform` from `DisplayServer` paid for itself immediately.** A headless Linux run
  uses this `Platform` and `platform/headless/`'s display server; the X11 half is not compiled into
  the decision at all. Had they been one interface, a machine with no X server would have had no
  clock.
- **The surface seam held for Vulkan.** `create_surface(GraphicsApi::Vulkan)` is implemented here
  through `vkCreateXlibSurfaceKHR`, reached by name through the loader, and this file includes no
  Vulkan header — the same arrangement SDL3's backend has, which is what keeps `just quality-layers`'
  `gpuapi` rule true for `platform/` as well.

## Where it pushed back — four findings

**1. Xlib's macros collide with the interface's own vocabulary, and one of them is `Status`.**
`X11/X.h` defines `None` and `Always`, and **`X11/Xlib.h:83` is `#define Status int`** — which is
`cy::Status`, the engine's universal return type. Measured rather than predicted: the first compile
of this backend produced **nine** *"no declaration matches `int cy::X11DisplayServer::set_window_*`"*
errors, because every `Status`-returning member in the implementation had silently become
`int`-returning and therefore matched no declaration. They are macros, so namespace scoping cannot
save them; the fix is four `#undef`s after the X11 includes, with the engine headers included first.

Two things follow, and the second matters more than the first:

- The cost is a dozen lines in three files **only because `DisplayServer`'s vocabulary is scoped
  enumerations** — `GraphicsApi::None`, not `None`. An interface that had spelled its constants at
  namespace scope would have been impossible to implement over X11 without renaming the interface.
- **`cy::Status` is not scoped and cannot be**, so every future backend over a C library that
  `#define`s a common word pays this cost again. The diagnostic is far from the cause, which is why
  it is written at the top of `x11_display_server.cpp` rather than left for the next port to
  rediscover.

**2. V-sync is not a window-system property, and the interface says it is.** `set_window_vsync()`
sits on `DisplayServer`, but on X11 the presentation interval belongs to the graphics API — a Vulkan
present mode — and the window system has no say in it. This backend **records** the request so the
RHI can read it back through the interface rather than through a channel of its own, and refuses
`Adaptive` and `Mailbox`, which `has_feature()` already answers false for. SDL3 hides this because
SDL owns the swapchain; a native backend cannot. **The honest shape would be a request on
`DisplayServer` and an answer on the swapchain**, and the interface currently has only the first.

**3. `input-and-actions` cannot be fully satisfied through a window system, and the interface has no
way to say so.** Two halves:

- **Key repeat.** X11 delivers auto-repeat as an ordinary press/release pair with no marker. SDL3
  marks it and `Sdl3InputSource` drops it, because *"a repeat is not a transition"*. This backend
  **cannot** distinguish one without peeking at the next event, which an observer that must not
  consume from the queue may not do. So repeats reach the action layer here and do not on SDL3 —
  **the two backends cannot agree, and the interface carries no "this was a repeat" bit** that would
  let them.
- **Gamepads.** There are none here, and that is a finding rather than an omission: a gamepad on
  Linux is a `/dev/input/event*` node found through udev, and it has nothing to do with the X server
  — which is why a headless Linux game can read a controller and cannot open a window. SDL3 bundles
  the two behind one library. **On this backend, `input-and-actions`' gamepad requirements are met by
  no implementation until an evdev source is written**, and that source belongs beside this directory
  rather than inside it.

**4. Timestamp resolution is the protocol's, not the engine's.** `input-and-actions` requires *"a
high-resolution timestamp"*. X stamps input events with the server's time **in milliseconds**; SDL3
reports nanoseconds. This backend converts rather than carrying two clocks in one stream, and the
millisecond granularity is the ceiling on what any X11 input source can report. An evdev source would
do better. Latency analysis across the two backends is therefore not comparable at sub-millisecond
resolution, and nothing in the interface records which resolution a source has.

## The residual this backend does not cover

A Linux native backend proves the abstraction carries no **SDL** assumption. It does **not** prove
the abstraction against a window system unlike X11's, and it does not prove it against a target that
is not a desktop — X11 is the window system SDL3 is best tested on, so the two share more than either
shares with Wayland, Cocoa or a console.

`platform/stub/` is the deliberate cover for exactly that, and it is the cheaper and stricter of the
two proofs: it shares **no** desktop assumption at all rather than a different set of them. See
`platform/stub/include/cy/platform/stub_platform.h`, and `tests/integration/test_stub_frame.cpp`,
which runs the engine's frames on it.

**Wayland is the third implementation this interface still wants**, and the reason it is not here is
measured rather than preferred: `XDG_SESSION_TYPE=x11` and no compositor on the machine this was
written on, so a Wayland backend could have been compiled here and never once run — which is the
exact failure mode M11.d moved Metal and D3D12 out for.

**Governed by**: `core-platform-abstraction`, `build-system-and-platforms` (platform porting
surface), `input-and-actions` (the platform boundary).
