# Make the RTS a game a person plays

## Why

`samples/13-rts-api` fights a forty-unit battle on The Frontier (#146) with fog of war, effects and
a topographic minimap, and every run of it is scripted: a headless player injects a fixed timeline,
and the pictures are captures. Nothing in the engine had opened a window a person plays in with a
mouse and a keyboard. Doing it found what was missing on the way from a desktop's input to a lockstep
command and from a frame to a window: the platform could not map a window's pointer into a view of
another size, nothing could push a wheel's turn or bring SDL's events up without a display, and
a game had no way to ask its host to stop the clock.

## What changes

- **`--play` in samples/13-rts-api**: an SDL3 window with vsync and resize, the platform's keyboard
  and mouse, frames at sixty a second, the map drawn and presented letterboxed. The Swift game
  (`game/Skirmish.swift` and neighbours) decides everything: an RTS camera (pan by keys and edges,
  wheel zoom, middle-drag turn, clamped), click and box selection with the engine's selection
  outlines, Ctrl+1..9 control groups, right-click attack, move and attack-move given as the same
  lockstep commands the scripted battle uses, a minimap click, a command bar, pause and an Esc menu
  and a result screen made with the widget toolkit, an opponent for red driven through the same
  commands, and victory and defeat. `just play-rts` runs it.
- **Replay**: `--record <file>` writes the session's command log; `--replay <file>` feeds it to a
  fresh session, which ends in the recorded digest.
- **`SkirmishControl`**: a component the game writes and the host reads after each frame — the one
  the host acts on — asking it to hold the fixed step (a pause) or to end the session.
- **Platform**: `Sdl3InputSource::set_pointer_mapping` (`cy::PointerMapping`) maps window
  coordinates into a view's; `cy::Sdl3EventInjector` (#149's) gains the wheel and a queue drain,
  and `cy::Sdl3EventSystem` brings SDL's event subsystem up alone.

## Impact

No ABI change: the game uses ABI 1.16's entries; `SkirmishControl` is a component registered from
Swift, and the selection outlines' component is registered by name. The scripted run, the battle and
their committed digests are unchanged. Additive in `platform/desktop-sdl3`. `--play` draws only where the Vulkan renderer is built; `--play --headless`
runs on every CI leg.
