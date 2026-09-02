# core-platform-abstraction Specification

## Purpose

Defines the surface every supported operating system must implement: process and filesystem
services, windowing and display, input, clipboard and dialogs, and the platform hooks the
renderer and audio backends need. Getting this boundary right is what makes porting a bounded
task rather than an open-ended one.

## Requirements

### Requirement: Platform services
`Platform` SHALL abstract, per operating system: process lifetime and exit code, command-line
arguments, environment variables, standard output and error, the user data / config / cache
directories, the executable path, dynamic library loading and symbol resolution, subprocess
creation and control, monotonic and wall clocks, locale, CPU count and features, memory
statistics, and crash handler installation.

Time SHALL be exposed as a monotonic clock in nanoseconds for frame timing, separate from wall
clock time.

#### Scenario: Monotonic clock is unaffected by system time
- **WHEN** the system clock is adjusted while the game runs
- **THEN** frame timing SHALL be unaffected

#### Scenario: Crash produces a report
- **WHEN** the process crashes with the handler installed
- **THEN** a report SHALL be written to the user mount containing the signal or exception, a
  symbolised backtrace where available, the engine version, and the last logged frame number

### Requirement: DisplayServer
`DisplayServer` SHALL abstract windows and screens: creation and destruction, position, size,
minimum and maximum size, title, icon, mode (windowed, minimised, maximised, fullscreen,
exclusive fullscreen), flags (resizable, borderless, always-on-top, transparent, no-focus,
popup, mouse pass-through), DPI scale, screen enumeration with resolution and refresh rate,
V-sync mode, and native surface creation for the active graphics backend.

Capabilities SHALL be queryable rather than assumed, through
`DisplayServer::has_feature(Feature)`.

A **headless** implementation SHALL exist that satisfies the interface without a window system.

#### Scenario: Unsupported feature degrades
- **WHEN** a platform lacks transparent windows
- **THEN** `has_feature(WindowTransparency)` SHALL return false and requesting transparency SHALL
  be ignored with a warning, not fail window creation

#### Scenario: DPI change while running
- **WHEN** a window moves to a screen with a different scale factor
- **THEN** a DPI-change event SHALL be delivered and the UI SHALL re-layout at the new scale

#### Scenario: Surface for the graphics backend
- **WHEN** the Vulkan backend initialises
- **THEN** `DisplayServer` SHALL provide the platform surface (VkSurfaceKHR, CAMetalLayer, or
  equivalent) without the backend containing platform `#ifdef`s

### Requirement: Input
The input pipeline SHALL be: platform event → `InputServer` normalisation → per-frame input state
snapshot → consumers (UI, then gameplay actions, then raw handlers).

Event types SHALL cover: key press and release with scancode, keycode, modifiers and repeat flag;
text input as UTF-8 (separate from key events); mouse motion (absolute and relative), buttons,
and wheel; touch begin, move, and end with per-touch ids; pen input with pressure and tilt;
gamepad buttons, axes, and connection changes; and gestures where the platform provides them.

Gamepad support SHALL use SDL3 as the backend, giving controller database mappings, rumble, and
hot-plug across platforms.

#### Scenario: Text input is separate from keys
- **WHEN** the user types a character with a compose key or IME
- **THEN** a text-input event with the composed UTF-8 SHALL be delivered, distinct from the raw
  key events

#### Scenario: Relative mouse for camera control
- **WHEN** the mouse is captured
- **THEN** relative motion SHALL be delivered without being clamped by screen edges

#### Scenario: Controller hot-plug
- **WHEN** a gamepad is connected mid-session
- **THEN** a connection event SHALL be delivered with a stable device id and its mapping resolved
  from the controller database

### Requirement: Input actions
`InputServer` SHALL provide an action mapping layer: named actions bound to one or more inputs,
with per-binding deadzone, inversion, and scale; and named axes and 2D vectors composed from
bindings.

Action state SHALL be queryable as pressed, just-pressed, just-released, and analogue value,
sampled against a per-frame snapshot so all consumers in a frame observe the same state.

Binding sets SHALL be loadable from configuration and rebindable at runtime.

#### Scenario: Same action, keyboard and gamepad
- **WHEN** "move" is bound to WASD and to the left stick
- **THEN** `get_vector("move")` SHALL return a normalised vector from whichever device is active,
  with the stick's deadzone applied

#### Scenario: Consistent state within a frame
- **WHEN** two systems query the same action in one frame
- **THEN** both SHALL observe identical state regardless of when the platform event arrived

### Requirement: Fixed-step input handling
Input consumed by fixed-step simulation SHALL be accumulated between ticks so that no press is
lost when a frame contains zero or multiple ticks.

#### Scenario: Button pressed and released within one frame
- **WHEN** a button is pressed and released between two simulation ticks
- **THEN** the tick SHALL still observe a just-pressed and just-released event, rather than
  missing the input entirely

### Requirement: Clipboard, dialogs, and system integration
`DisplayServer` SHALL expose, gated by feature queries: clipboard read and write (text and
image), native file dialogs, native message dialogs, the system cursor shape and custom cursors,
IME positioning and composition state, on-screen keyboards, screen orientation and keep-awake
control, and system tray or status items.

#### Scenario: Feature absent
- **WHEN** native file dialogs are unavailable
- **THEN** the engine SHALL fall back to its own in-engine file dialog

### Requirement: Accessibility hooks
The platform layer SHALL expose an accessibility tree interface so UI elements can publish role,
label, value, and state to the OS screen reader where the platform supports it.

Accessibility SHALL be optional at build time and SHALL not impose cost when disabled.

#### Scenario: Screen reader reads a button
- **WHEN** accessibility is available and focus moves to a button
- **THEN** the button's role and label SHALL be published so the screen reader announces it

### Requirement: Supported platforms
The initial supported platform set SHALL be **Linux**, **Windows**, and **macOS** on x86-64 and
ARM64.

**iOS**, **Android**, and **visionOS** SHALL be planned targets whose requirements the platform
abstraction must not preclude — specifically: no assumption of a mouse, no assumption of a
resizable window, no assumption of a filesystem writable outside the user mount, and no
assumption that the process controls its own main loop.

Console platforms are explicitly **out of scope**.

#### Scenario: Platform does not own the main loop
- **WHEN** a platform requires the OS to drive frames (mobile, web)
- **THEN** the runtime SHALL expose a `tick()` entry point the platform layer can call, rather
  than owning a `while (running)` loop internally

#### Scenario: New platform port
- **WHEN** a new platform is added
- **THEN** it SHALL implement `Platform`, `DisplayServer`, an input backend, an audio backend, and
  a graphics surface provider, with no changes required in `src/core/` or above
