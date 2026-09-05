# `src/servers/input/` — CyberInput

Layer 2. Turns normalised, timestamped device events into semantic actions and per-tick command
frames. Section 4.1 of the M4 tasks; the governing specification is `input-and-actions`.

## The boundaries are the specification

```
platform/desktop-sdl3/  ──►  src/servers/input/  ──►  src/gameplay/
   timestamped events          users, contexts,          commands
   (no coalescing)             bindings, actions
```

The platform layer ends at normalised, timestamped device events. This layer owns users, contexts,
bindings, processors, triggers and actions. Gameplay owns commands. *"A gameplay system that reads a
key code has crossed a line, and so has an input layer that writes a transform."*

Both halves of that sentence are enforced by the build rather than by review:

* This module is layer 2, so it cannot include `cy/backends/**`, `cy/rendering/**`, `cy/scene/**`,
  `cy/runtime/**` or an ECS world. There is no transform reachable from here, and
  `set_reference_frame()` takes three vectors precisely so that a camera-relative binding needs no
  camera type.
* `src/gameplay/` declares **no dependency on this module**, so a gameplay translation unit cannot
  include these headers at all. See `src/gameplay/README.md` and `design.md` §3.

## The file to read first

`src/user.cpp`, and the test that drove it: `tests/test_fixed_tick.cpp`.

`design.md` §5 names the defect this milestone exists to avoid: *"the implementation most likely to
be written is 'sample the current state each tick', which works in every manual test and loses
inputs precisely when the frame rate is uneven — which is when players notice."*

The resolution is therefore three phases and not one:

1. `begin_tick()` clears the per-tick half of every action record — the flags and the two transition
   counts — and keeps the level half.
2. `observe()` is called once per accumulated event, **in `(timestamp, sequence)` order**, and
   re-evaluates only the bindings that read the control the event touched. Every rising edge
   increments `press_count`; every falling edge increments `release_count`.
3. `finish_tick()` advances the triggers that depend on elapsed time rather than on an event — a
   hold reaching its duration, a pulse repeating — and builds the command frame.

A press and a release inside one window therefore leave `press_count == 1` and `release_count == 1`
behind, with the *level* back where it started. Both facts are true at once, and that is the point.

`test_fixed_tick.cpp` ends with a negative control that computes what a level-sampling resolver
would have answered on the same events and asserts the two differ — so the file's green cannot
become vacuous.

## What is here

| Header | What it owns |
|---|---|
| `types.h` | Device kinds, resolved `Control`s, action value types, `Interpretation`, trigger kinds and phases, schemes, `EventSource`. The one path parser lives in a `cook` namespace. |
| `event.h` | `DeviceEvent` and the accumulation window. **Never coalesces**; drops are counted and reported. |
| `device.h` | Devices, capabilities, lifecycle, the reconnection memory, and the keyboard/mouse ownership policy. |
| `action.h` | Action declarations, the two identities (stable and dense), and the compact per-user state record. |
| `binding.h` | Processors (numerical), modifiers (contextual), composites, mapping contexts. |
| `profile.h` | Player overrides and the explicit rebinding flow with its conflict policy. |
| `scheme.h` | Control schemes, and detection with **both** a significance threshold and hysteresis. |
| `frame.h` | The per-tick `CommandFrame`, and the text stream. |
| `diagnostics.h` | Why an action did not trigger, per-processor values, the event trace, the latency view. |
| `server.h` | `InputServer`: users, contexts, the window, and `resolve_tick()`. |

## Two decisions worth knowing

**An override addresses a component, not a binding.** `{action stable id, scheme, slot}` where
`slot = binding_ordinal * kMaxComponents + component_index`. That is what makes *"full remapping
including composite elements"* expressible: a player may rebind the `A` of WASD without touching the
other three. It is also why an override survives a content update — the stable id survives a rename
and a reorder, and a binding index would not.

**Every setter that can change what an idle binding produces marks the next resolution as a full
pass.** The evaluation path deliberately touches only the bindings whose controls changed, so a
dead-zone slider changed through a reference would appear not to work until you wiggled the stick.
`set_accessibility`, `set_setting`, `set_state`, `set_focus`, `set_text_entry_active` and
`set_reference_frame` all set the flag, and there is no accessor that could bypass it.

## Headless is the default, not a mode

`initialize()` opens no device and never fails for want of one. A device exists when something calls
`devices().connect()`. Every case in `tests/` runs with no platform layer at all, which is what makes
them tests of the model rather than of SDL — and it is the same path a dedicated server takes.

## What is thinner than the tasks claim

* **Input assets are not cooked.** `input-and-actions` requires actions, contexts, bindings,
  processors and triggers to be authored as assets and cooked into these tables, participating in
  the derived data cache and the identity manifest. M4 builds the tables in code; the runtime side
  is complete and performs no string lookup, but nothing writes them from an asset yet.
* **`ActionStableId` is not the identity manifest's number.** The shape is right — an opaque value
  the declaration carries, never derived from the name — but nothing allocates it from
  `identity/manifest.toml` yet.
* **Interface routing is the focus-layer half only.** Focus layers, pass-through and text-entry
  suppression are implemented; `ui-system`'s side of the boundary does not exist yet.
* **Performance is unmeasured.** The structure meets the requirement's shape — no allocation in the
  evaluation path, no lock, only changed controls and active bindings processed — but there is no
  benchmark asserting the eight-user, thousand-action figure.
* **The pose value type is declared and unfed.** `ActionValueType::Pose` and `ActionValue::rotation`
  exist so that tracked devices need no model change; no backend produces one.
* **Windows and macOS are unverified.** The SDL3 event source is written against SDL3's own API and
  compiles here; only Linux was executed.
