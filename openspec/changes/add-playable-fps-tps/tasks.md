# Tasks

- [x] `samples/play`: the clock, the pause, the rebinding file, the input script, the interface, the presenter and its present shader (SPIR-V and MSL checked in, a group in `tools/shaders/embedded_headers.toml`), the play host and `HostedGame`; `unit.play_host`.
- [x] `Sdl3EventInjector` with its virtual gamepad; `integration.sdl3_injector`.
- [x] `DisplayServer::set_window_relative_mouse` and `Feature::RelativeMouse`, over SDL3.
- [x] The input adapter: the pointer's delta in window pixels, and a frame's motion published whether or not a tick ran; `InputServer::resolutions`; `integration.game_backend_input`'s three cases.
- [x] An override's slot counted per action; `unit.input_actions`' case.
- [x] The Vulkan swapchain resized in place.
- [x] samples/15-first-person: the host's frame in parts, presentation at the frame's own time, the pad and reload bindings, targets that rise, the renderer on the play host's device; the Swift game's magazine, respawn, HUD, pad look and pause menu; `integration.first_person_play` and `smoke.first_person_play`.
- [x] samples/14-third-person: the same split, the fence, the pad bindings; the director's mouse orbit and pause menu; `integration.third_person_play` and `smoke.third_person_play`.
- [x] CyberdyneKit's `PauseMenu` and `PlaySettings`, and `UIToolkitTests`' case.
- [x] The scripted runs' reports and digests compared with main's, byte for byte.
- [x] Prove the new cases red by mutation, and record it in `evidence/`.
- [x] `just play-fps`, `just play-tps`, `just capture-play-fps`, `just capture-play-tps`; the films and stills.
- [x] The READMEs, `docs/guides/playing-a-game.md`, the demo guides, the requirements map and the specifications.
