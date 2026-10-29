# `platform/desktop-sdl3/` — layer 3

`Platform` and `DisplayServer` for **Linux, Windows and macOS**, over SDL3.

Governed by `core-platform-abstraction`; the choice of SDL3 is `design.md` §4, and the dependency is
pinned in `deps/manifest.toml`. A native per-platform backend is a scheduled **M11** task, not a
hope: an abstraction with exactly one implementation is a guess, and this is the implementation that
gets checked against a second one.

## The two rules this directory exists to keep

**No SDL type appears above `platform/`.** Not in a header, not in a signature, not in a type alias.
The window handles in `cy/platform/sdl3_display_server.h` are `void*` for exactly that reason, and
`tools/layercheck/` fails the build on an `SDL` include outside this tree.

**No `#ifdef` selects platform code.** Everything SDL3 does not provide is one file per operating
system under `src/host/`, and `CMakeLists.txt` picks the one matching `CMAKE_SYSTEM_NAME`. That is
task 3.2.4. A fourth desktop platform is a fourth file and one branch in `CMakeLists.txt`.

## Layout

| Path | What it is |
|---|---|
| `include/cy/platform/sdl3_platform.h` | `Sdl3Platform` — the `Platform` implementation |
| `include/cy/platform/sdl3_display_server.h` | `Sdl3DisplayServer` — the `DisplayServer` implementation |
| `include/cy/platform/sdl3_input_source.h` | `Sdl3InputSource` — SDL events as `cy::input::DeviceEvent`s (M4) |
| `include/cy/platform/sdl3_event_injector.h` | `Sdl3EventInjector` — synthetic input as REAL SDL events on SDL's own queue: keys, the mouse, a window's focus, and a virtual gamepad through SDL's virtual joystick. What a played game's tests and captures push their hands through (`samples/play`) |
| `src/sdl3_platform.cpp` | every SDL call for the process services |
| `src/sdl3_display_server.cpp` | every SDL call for windows, screens and events |
| `src/sdl3_input_source.cpp` | every SDL call for keyboards, mice and gamepads, and the window-to-view pointer mapping |
| `src/sdl3_event_injector.cpp` | the injector: the input source's translations run backwards (`src/sdl3_controls.h`), so an injected key is the scancode a person's press of it carries; and `Sdl3EventSystem`, SDL's event subsystem alone |
| `src/host/host.h` | the four host services SDL3 has no API for |
| `src/host/host_linux.cpp` | `/proc/self/exe`, `/proc/meminfo`, the X11 and Wayland handles |
| `src/host/host_macos.cpp` | `_NSGetExecutablePath`, mach memory statistics, the `NSWindow` — **unverified** |
| `src/host/host_windows.cpp` | `GetModuleFileNameW`, `GlobalMemoryStatusEx`, the `HWND` — **unverified** |
| `src/host/host_signals_posix.cpp` | `sigaction` for the fatal signals; Linux and macOS |
| `src/host/host_signals_windows.cpp` | `SetUnhandledExceptionFilter` — **unverified** |

Linux was verified by running: a real X11 window opens, the native handles come back, the clocks are
independent sources, subprocesses and dynamic libraries work. Windows and macOS were written against
the documented APIs and reviewed; the first CI job on each (task 2.4.1) is what makes them facts.

## Holding the mouse

`set_window_relative_mouse` — `Feature::RelativeMouse` — is `SDL_SetWindowRelativeMouseMode`: the
cursor hidden and held in the window, its motion delivered as raw counts with no system acceleration
(SDL3's default unless `SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE` asks for it). A first-person look and
an orbiting camera read those counts. The dummy and offscreen drivers have no pointer to hold and
answer false. `DisplayServer`'s default refuses it, so the native and stub ports say so without
restating it.

## Where the abstraction degrades

`design.md` §4 predicted it and `has_feature()` is the answer. Each `case` in
`Sdl3DisplayServer::has_feature()` carries its reason; the ones that matter today:

* **`WindowTransparency`** — honoured by the Cocoa, Wayland and Windows backends. Under X11 it needs
  a compositing manager, whose presence SDL does not expose, so the answer there is `false`.
* **`MousePassthrough`, `WindowPopup`** — SDL3 has neither as a `SDL_CreateWindow` flag. Requesting
  them warns and the window is created without them.
* **`VSyncAdaptive`, `VSyncMailbox`, and every surface API** — V-sync and surfaces belong to whatever
  presents, and nothing does until the RHI lands at **M3**. `set_window_vsync()` records the mode so
  the window carries its own intent; `create_surface()` with `GraphicsApi::None` returns the native
  window a real surface would be built from, which is the seam task 3.3.5 asks to be exercised.
* **Clipboard, dialogs, cursors, IME, tray** — SDL3 provides all of these and this `DisplayServer`
  does not expose them yet. `has_feature()` answers `false` for a call that does not exist; each
  becomes `true` in the change that adds its call.

## What is not here

**Input landed at M4 and this section used to say it was coming at M2.** It is
`Sdl3InputSource`, above: SDL keyboard, mouse and gamepad events become timestamped
`cy::input::DeviceEvent`s and nothing is coalesced, which is `core-platform-abstraction`'s
"Fixed-step input handling" requirement — the platform layer's whole obligation is to deliver every
transition with its timestamp, and `src/servers/input/` resolves them per tick.
`integration.sdl3_input` drives it headlessly with synthetic SDL events.

**A view of another size.** A host that draws a fixed-size view scaled and letterboxed into a
window of any size sets `Sdl3InputSource::set_pointer_mapping` (`cy::PointerMapping`, `view =
(window - offset) * scale`), and every pointer position and motion forwarded from then on is in the
view's pixels — converted once, where the coordinate is first read. The identity by default.

**A hand with no display.** `Sdl3EventSystem` brings SDL's event subsystem up alone, and
`Sdl3EventInjector::mouse_wheel` and `drain` let a host with no display server push a wheel's turn
and empty the queue itself: `samples/13-rts-api --play --headless --input` plays a whole skirmish
that way, and `integration.sdl3_input` covers both.

What is genuinely still absent is the rest of `core-platform-abstraction`'s "Clipboard, dialogs, and
system integration": clipboard text and images, native file and message dialogs, cursors, IME and
on-screen keyboards, orientation and keep-awake, and tray items. SDL3 provides all of them; this
`DisplayServer` exposes none, `has_feature()` answers `false` for each, and each becomes `true` in
the change that adds its call. `Accessibility hooks` — an accessibility tree the OS screen reader
can read — is likewise not started; `InputServer`'s accessibility settings are a different
requirement in a different capability.
