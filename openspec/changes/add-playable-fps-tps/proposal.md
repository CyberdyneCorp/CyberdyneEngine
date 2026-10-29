# Play the first- and third-person samples in a window

## Why

`samples/15-first-person` (#144) and `samples/14-third-person` (#145) are games written in Swift and
drawn by the engine's frame, and nobody can play either: both run a scripted session and write
images. Nothing in the tree opened a window a person drives with a mouse, a keyboard or a pad, ran a
game against real time, or paused it. Playing them found four defects below the samples, each a
reason a game could not have been played before.

## What changes

- **A desktop play host, `samples/play`.** An SDL3 window (resizable, v-sync, full screen), the
  platform's input through the game's own action maps (keyboard, raw mouse in relative mode, pads
  with radial dead zones; a player's rebinding file), a real-time loop of fixed sixty-hertz ticks
  with presentation at the frame's own time between them, a pause on Esc, a pad's Start and the
  window losing the focus, the interface a game builds through ABI 1.6 and 1.12, and the game's
  picture scaled into a Vulkan swapchain with the interface over it. `cy::play::PlayGame` is what a
  game implements; `HostedGame` implements it for any host that exposes its frame's parts.
- **Both samples played** with `--play` (`just play-fps`, `just play-tps`). The first person gains a
  magazine and a reload, targets that get back up ten seconds after they fall, a HUD (crosshair, hit
  marker, magazine, score) and the pad; the third person a mouse-orbited camera, the pad, and walls
  at the terrain's edge. Both get CyberdyneKit's `PauseMenu`: the look's sensitivity and inverted Y,
  Resume and Quit. Every gameplay decision stays in Swift; the scripted runs and their digests are
  byte for byte what they were.
- **`Sdl3EventInjector`**: synthetic input as real SDL events on SDL's queue — keys, the mouse, a
  window's focus, a virtual gamepad — so a played game's tests and captures take a person's path.
- **`DisplayServer::set_window_relative_mouse`** and `Feature::RelativeMouse`, over SDL3.
- **The pointer's motion in a frame.** The ABI's pointer delta is window pixels, +y down like its
  position; the input adapter added the input layer's +y-up `MoveY` unconverted, so a real mouse
  pushed away read as a pull. A frame that ran no tick published no motion at all — a fast display
  judders — and now the adapter observes the events still waiting for a tick, each once
  (`InputServer::resolutions` names the window).
- **A resized Vulkan swapchain kept no handle.** `resize_swapchain` destroyed the swapchain and made
  another under a handle it never returned; it now rebuilds in the record the caller's handle names,
  the old swapchain handed to the driver as the one replaced.

## Impact

No ABI entry is added or changed. A module that read the pointer's delta from a real mouse reads its
y with the sign its documentation always gave; the scripted first-person host injected the input
layer's convention wrong by the same sign, and both are corrected together. `DisplayServer` gains
two non-pure virtuals whose defaults refuse, so the native, stub and iOS ports are unchanged.
