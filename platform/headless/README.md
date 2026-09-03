# `platform/headless/` — layer 3

`DisplayServer` with no window system. Required by `core-platform-abstraction` — "A **headless**
implementation SHALL exist that satisfies the interface without a window system" — and by CI, where
there is no display to open a window on.

It is not a set of stubs that return errors. It is a display server whose desktop is **described**
rather than discovered: windows have positions, sizes and flags, screens have resolutions, refresh
rates and scale factors, and a window that moves between screens of different scale delivers a
DPI-change event. Everything the interface promises happens; only the pixels are missing.

## The modelled desktop

The default is **two** screens, side by side, with different scale factors:

| Screen | Position | Resolution | Refresh | Scale |
|---|---|---|---|---|
| `headless-1` | (0, 0) | 1920×1080 | 60 Hz | 1.0 |
| `headless-2` | (1920, 0) | 2560×1440 | 120 Hz | 2.0 |

Two, and mismatched, on purpose. `core-platform-abstraction`'s "DPI change while running" scenario —
a window moves to a screen with a different scale factor, and a DPI-change event is delivered — is
otherwise testable only on a machine with two mismatched monitors, which is not a machine CI has.
`HeadlessDesktop` lets a test describe any other desktop it needs.

## Headless-only controls

A window system delivers these; without one, a test does. They are on `HeadlessDisplayServer`
rather than on `DisplayServer`, so nothing written against the interface can reach them.

* `post_close_request(window)` — what a window manager's close button produces.
* `set_screen_scale(screen, scale)` — a screen changing its own scale under a stationary window, as
  distinct from a window moving to a different screen. Both produce `DpiChanged`; only a scale that
  actually changed does.

## What it reports and why

`has_feature()` answers `true` for the flags a described desktop can honour exactly — resizable,
borderless, always-on-top, no-focus, high DPI, per-screen scale, refresh rate — and `false` for
everything that needs a compositor, a pointer, a graphics API or a user. That makes it the backend
the "unsupported feature degrades" scenario is tested against: `WindowTransparency` is genuinely
absent here, so the drop-and-warn path is exercised rather than described.

`create_surface()` accepts only `GraphicsApi::None` and returns the window's id as an opaque,
never-dereferenced token — the same shape as the SDL3 seam, which is what task 3.3.5 asks for.

## There is no headless `Platform`

Process lifetime, arguments, environment, paths, clocks, subprocesses and CPU information do not
need a window system, so `Sdl3Platform` serves a headless run unchanged — it initialises SDL with no
subsystems. Only the display side has to be replaced, and only the display side is here.
