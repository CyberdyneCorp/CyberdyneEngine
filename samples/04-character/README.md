# samples/04-character — a game, written in Swift

M4's closing artefact. A third-person character controller that moves, jumps, collides with a level,
is heard, and is followed by a camera — with **no C++ gameplay code**.

```
just run-sample character                    900 fixed ticks of scripted play, headless
just run-sample character --ticks 120        a short run, which is what the smoke test does
just run-sample character --no-behaviours    the negative control: see "The claim", below
just run-sample character --jolt             the same run over the Jolt backend
just run-sample character --verbose          one line per tick
just run-sample character --device-input     a window, and your own keyboard
```

## What is here

| | |
|---|---|
| `game/` | **The game.** Five Swift files: the boundary, the character, the camera director, the level, and the module's two entry points. Nothing else in the sample decides anything. |
| `host/` | **The host.** Brings up five servers, loads `game/` over the C ABI, and translates between them once per fixed tick. |
| `module.toml` | The module manifest `native-abi` requires: name, entry symbol, minimum ABI version, per-platform library. |
| `tools/check_no_cpp_gameplay.py` | Task 5.3's static half, with its own negative cases. |
| `tests/smoke/test_character_sample.cpp` | Task 5.3's *real* half, and task 5.4. Declared from this directory's `CMakeLists.txt`, for the reason that file gives. |

## The boundary

`game/Contract.swift` is the whole of it: eight component types, and the direction each one flows.

| host → game | game → host |
|---|---|
| `PlayerInput` — one tick of committed intent | `CharacterSpec` — the capsule to build |
| `CharacterState` — where the body ended up | `CharacterDrive` — the motion wanted this tick |
| | `CameraSpec` — the rig's numbers |
| | `CameraIntent` — what to frame, and from where |
| | `AudioCue` — monotone counters; each advance is a sound |
| | `LevelBox` — one static box of the level |

Fields are resolved **by name**, not by position: `CyWorld_T` keeps the field records a registration
produced, so `host/contract.h` asks for `"velocity"` once at bring-up and holds the offset. Renaming a
Swift property fails the resolve with the name it could not find, before a frame runs, rather than
silently reading the field next to it.

### Why components and not ABI entries

ABI 1.0's interface table carries the engine-neutral core — diagnostics, values, entities,
components, behaviours — and nothing about input, physics, audio or cameras. Those entries are
appended by the subsystems that own them, which is what the append-only rule is for and which has not
happened yet. So the game reaches those subsystems the way any ECS game does, and the host carries the
values across. When `CyInterface` grows input, physics, camera and audio entries, the four components
that exist only to carry them become calls and nothing in `game/` changes shape.

## The claim, and how it is checked

Task 5.3 is "a check that the sample contains no C++ gameplay code". There are two, and the weaker one
is the grep.

**The static half** — `tools/check_no_cpp_gameplay.py`, run as
`integration.character_no_cpp_gameplay` — checks four things: the partition (Swift under `game/`, C++
under `host/`), one call into the game per tick, **the direction of every component access**, and that
no distinctive `@Export`ed tunable appears as a literal in the C++. The third is the one that means
something: a host that wrote into `CharacterDrive` would be choosing the character's velocity, and a
host that wrote into `CameraSpec` would be choosing the framing.

Its negative cases are a **second registered test**, `integration.character_no_cpp_gameplay_selftest`
— `--selftest`, which edits the live tree in memory and requires each of the four rules to reject its
own violation, with the unedited sample as case 0. Separate rather than a flag on the first, so that a
failure names which half broke: "the sample gained C++ gameplay code" and "the check that would have
noticed has stopped working" are different defects with different fixes. Both match
`-R character_no_cpp_gameplay`, which is the command the gate and the ledger declare.

**The real half** — `--no-behaviours`, asserted by `smoke.character_sample`. It loads the same
module, builds the same level out of it, brings up the same five servers, resolves the same contract,
and pushes the same scripted input through the same command stream. It creates neither `Character` nor
`CameraDirector`. **Every line of C++ in this sample runs.** The character does not move, does not
jump, makes no sound, and the camera does not turn.

A static check can only fail on the words somebody thought to forbid. This one fails on any decision
at all having leaked to the C++ side.

## One input path

The simulation's only input is the committed command stream, which is `gameplay-framework`'s M4
invariant. In `host/game.cpp` it reads:

```
input.resolve_tick()  ->  CommandFrame  ->  Command  ->  commit()  ->  PlayerInput  ->  the game
```

`publish_input` takes the **committed command's payload**, not the command frame. Reading the frame
there would produce the same numbers today and quietly make the log incomplete — a command validation
rejected would still reach the game, and a replay of the log would diverge from the run it came from.
That is the second path the requirement forbids, and it is one identifier wide.

The bridge lives here, in the sample, and not in `src/gameplay/`: `cy::gameplay` declares no dependency
on `cy::servers-input`, so an input header is not on a gameplay translation unit's include path at all
(`src/gameplay/tests/test_bypass.cpp` makes that structural). Something has to see both, and that
something lives above both.

## The scripted player

A headless run has no hands on it, so the default input is a timeline of key transitions submitted
through `InputServer::submit()` — the same door `Sdl3InputSource` uses, and the only one there is. The
events go through the event buffer, the bindings, the triggers, the per-tick resolution and the
command frame exactly as a keyboard's would. `--device-input` swaps the source for a real keyboard
with nothing else changed.

**The tap at tick 250 is the milestone's own requirement, inside the artefact.** Both edges of the
jump key are stamped inside one tick's window, so the key is already up when the tick resolves —
design.md §5's "a button pressed and released *between* two ticks must still be observable as both a
press and a release by the tick that follows". A level-sampling resolver reports no jump at all, the
character never leaves the ground, and the smoke test's `jumps` assertion is zero.

## What is thinner than the task list claims

* **It draws nothing, and that is M3's gap rather than a choice.** `cy::rhi::Device` exposes no way to
  obtain the graphics-API instance a window surface must be created against, so a host can create a
  window and a device and cannot join them — `tools/roadmap/milestones/m3.toml` records it and
  `samples/03-first-light` renders offscreen for the same reason. This sample instead produces the
  `cy::render::ViewDescription` a renderer would draw, every tick, and counts it. The camera half of
  "render view production feeding M3's renderer" is exercised; the renderer half is not.
* **No mouse look.** `Look` is bound to the arrow keys as a two-dimensional axis, in a *rate*
  interpretation. A mouse delta is a `Delta` interpretation and mixing the two on one action is the
  confusion `src/servers/input/README.md` warns about; doing it properly is a second binding and a
  second processor, and it is not here.
* **The level is axis-aligned boxes.** `LevelBox` carries a centre and half-extents and no
  orientation, so the "ramp" is a flight of shallow steps. A rotated box is a `Quat` field away.
* **No hot reload in this sample.** The module declares `hot_reload = true` and the loader supports it;
  reload is exercised by `integration.swift_reload`, over a fixture built for it. Wiring a file watcher
  into this host is small and is M5's, where the editor needs it anyway.
* **No renderer, no animation, no navigation.** design.md §7: the character is a capsule that moves.
* **Windows and macOS are unverified.** This is a Linux-only machine. `module.toml` names a library for
  each and neither has been loaded.
