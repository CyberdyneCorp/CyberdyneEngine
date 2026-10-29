# Playing the RTS

`samples/13-rts-api --play` turns the battle of [RTS combat](rts-combat.md) into a game a person
plays in a window: blue's forty-unit army under the mouse and keyboard, red's under an opponent, on
[The Frontier](rts-map.md), until one side has won. This guide is the route through it: how a
desktop game's loop is put together from the engine's pieces, where each decision lives, how a
played session replays, how it is tested without a person, and what it costs.

```
just play-rts                                  play
just play-rts --record game.log                play, and keep the session's command log
just play-rts --replay game.log                watch a recorded session
just capture-rts-play                          the README's images and video
ctest -R rts_api_play                          integration.rts_api_play and smoke.rts_api_play
```

The controls are in [the sample's README](../../samples/13-rts-api/README.md#playing-the-rts).

## The loop, piece by piece

```
SDL's queue ──► Sdl3InputSource (pointer mapping) ──► InputServer ──► RtsHost::frame()
     ▲                                                                   │  fixed step (unless paused)
     │                                                                   │  Swift onFixedUpdate: orders
Sdl3EventInjector (--input)                                              │  Swift onUpdate: the hand read
                                                                         ▼
Sdl3DisplayServer (window, resize, close) ◄── PlayWindow ◄── MapRenderer::frame + present pass
```

`host/play.cpp` is the whole loop, and it decides nothing about the game:

1. **The window and its events.** `Sdl3DisplayServer` opens a resizable window; its `pump_events`
   drains SDL's queue once a display frame, reports a close and a resize.
2. **The platform's input.** `Sdl3InputSource` attaches to the host's `InputServer` and connects a
   keyboard and a mouse; the host assigns them to user 0 (`RtsHost::adopt_devices`) in place of the
   scripted pair a run without `--play` injects through.
3. **Sixty frames a second.** A `Pacer` owes one host frame per 16.67 ms of wall time, at most four
   at once, whatever the display's refresh; one host frame is one fixed tick.
4. **The frame.** `MapRenderer::frame` draws what the host just ran, then a second, small graph
   draws the output onto the acquired swapchain image (`PresentPass`) and presents it.

## Where each decision lives

Everything a player's input means is the Swift game's — `game/Skirmish.swift` and its neighbours:

| | |
|---|---|
| `SkirmishCamera` | pan by keys and edges, zoom by the wheel, turn by a middle drag, all in the camera's own frame, the focus clamped to the field; told to the engine as a pose (`Camera.overridePose`) |
| `SkirmishSelection` | a click picks the blue unit whose chest is nearest on screen within 18 px; a drag past 6 px is a box that picks every living blue unit inside it; Shift adds |
| `ControlGroups` | Ctrl and a number keeps the selection; the number calls it back, twice in a third of a second centres the camera |
| `PlayerOrder` | a right click on an enemy is an attack, on a building an attack on it, on the ground a move — or an attack-move with Ctrl or after F; one command a unit, to its own group, into a formation slot |
| `RedOpponent` | every half second from the session's state: defend a threatened building, attack when stronger or after 25 s, otherwise hold |
| `Outcome` | victory when both red buildings have fallen, defeat when no blue unit is alive |
| `SkirmishUI` | the command bar (CyberUI elements), the Esc menu and the result screen (the widget toolkit, ABI 1.12) |

The units are the battle's (`Battle.Mode.skirmish`): the same squads, arms and buildings as the
scripted battle, enlisted with **a group a unit**, because the session's orders address groups and a
player's selection is any set of units. The scripted battle keeps its squads' groups and its plan,
and its committed digest.

A pick is made in `onUpdate`, where the pointer and the camera may be read; the order it becomes is
**recorded** there and given in the next `onFixedUpdate` through `Lockstep.order`, where the
simulation may change. That is the pattern `add-swift-game-api`'s design gives an RTS, and it is what
makes the orders commands in one log.

## The host is asked, never told: `SkirmishControl`

A pause stops the session's clock, and only the host can do that; only the game may decide it. The
game writes a component it registers, `SkirmishControl` (`paused`, `quit`), every frame, and the host
reads it after the frame (`RtsHost::session_request`) — the one component the host acts on.

A frame the game holds (`RtsHost::run_frame(false)`) runs the tree's pump, the input's resolution
(so P and Esc are still heard), the camera, the interface and the game's frame update, and no fixed
step: the session, the bodies, the animation clock, the battle's effects and the sound stand. The
input's tick is the frame's; the simulation's tick (`RtsHost::ticks`) advances only with a fixed
step, and `CyTime.flags` carries `CY_TIME_PAUSED` in a held frame. A request is honoured from the
frame after the one that made it.

## A window of any size

The frame is drawn at an extent fixed when the window opens — its targets, its GI, its fog and its
HUD are sized once — and 16:9, because the game lays out, picks and hit-tests in a 1280 by 720 view.
The window is the player's to resize, so the frame is drawn ONTO the window's image, scaled to fit
and centred with black bars (`letterbox.h`, `shaders/present.slang`), and the same letterbox is
inverted where the pointer is first read:

```cpp
cy::PointerMapping mapping;        // view = (window - offset) * scale
mapping.scale_x = 1280.0F / box.width;
mapping.offset_x = box.x;          // ... and y
source.set_pointer_mapping(mapping);
```

`Sdl3InputSource` applies it to every position and relative motion it forwards (`integration.
sdl3_input`'s "the pointer mapping turns window coordinates into the view's"), so the game never
sees a window coordinate. A window resize rebuilds the swapchain into the same handle and updates
the mapping.

## Presenting without timing the compositor

The frame's own graph knows nothing of the swapchain: it ends with its output in a sampled state,
and its cost is recorded, submitted and waited for as `smoke.rts_api_map` measures it. A second graph
imports the acquired image as one (`import_swapchain_texture`), draws the output onto it, declares
the `Access::Present` hand-off, and is executed with presentation's semaphores
(`ExecuteOptions::wait_acquire` and `signal_present` on `GraphExecutor::execute`). Waiting for the
presentation engine to hand an image back — which a compositor under load held for over four
seconds once on this host — is that graph's time, reported apart as `present_ms`, never the frame's.

## Replaying a played session

A lockstep session is its orders. `--record <file>` writes the issuing session's command log —
every order's tick, kind, group and raw `Fixed` target — with the tick count and both peers' digests
(`host/replay.h`); `--replay <file>` places `/Level/SkirmishReplay`, by which the game gives no
orders, and records the logged ones into the session before each tick. The replay ends in the
recorded digest:

```
rts replay   source=game.log orders=76 ticks=1539 recorded_ticks=1539 digest=7a08c7ae2199cd3b
             recorded=7a08c7ae2199cd3b follower=7a08c7ae2199cd3b recorded_follower=7a08c7ae2199cd3b match=1
```

The log holds the opponent's orders as well as the player's: a replay needs neither.

## Testing a game nobody plays

`--input <script>` (`host/input_script.h`) is a written hand: keys, pointer moves, clicks, the wheel
and window resizes, at frames, pushed as real `SDL_Event`s by `cy::Sdl3EventInjector`. Nothing in the
game or the host is called; the events travel the scancode table, the pointer mapping, the event
watch and the input server's window as a person's do. `aim unit <i>` and `aim field <x> <z>` move the
pointer where the game's camera shows that unit or point now (`RtsHost::project`), and `aim ui
<name>` onto an interface element or widget.

With `--headless` there is no window and no device — SDL's event subsystem alone
(`cy::Sdl3EventSystem`) — so `integration.rts_api_play` runs on every CI leg:

- `play/select-attack.input`: a box round six swordsmen and a right click on a red one — six
  outlined, six ATTACK commands in the log on one tick, to the six groups, at one point, and the six
  going for that unit; D held for thirty frames — the focus moved by 0.75 × 38 × ½ m along the
  camera's right and not across.
- the recorded log replays to the recorded digest, and the same log with one order's target moved
  by 2⁻³² m does not;
- two sessions from the same hand write the same log;
- `play/pause-menu.input`: P holds the session for as long as it says, the menu's Resume and Quit.

`smoke.rts_api_play` plays `play/skirmish.input` in a window on a device: victory, 1080p within
16.7 ms, two resizes presented after, the outlines drawn, and a short run with no validation error.

## Measured

On the RTX 5060 at 1920x1080, Development, no validation layer, the whole skirmish: the frame 10.7
ms on average, the worst 24 ms; presenting it 3.6 ms more, the wait for the image included.
`integration.rts_api_play` takes about five seconds; `smoke.rts_api_play` about forty-five.

## Pitfalls

- **One host frame, one tick.** The game reads `justPressed` from the tick its frame resolved; a
  display frame that runs two host frames reads the second's. The pacer runs host frames one at a
  time for that reason.
- **The frame must be 16:9.** `--play` makes the frame's height follow its width; the camera's
  projection is the view's.
- **A pointer that has never moved is nobody's.** A device at (0, 0) would pan the camera into a
  corner; the game ignores the edges until the pointer has moved.
- **The swapchain's semaphores.** One presentation semaphore a swapchain image: a binary semaphore a
  present waits on may be signalled again only once its image has come back.

## Not built yet

- Training units: the lockstep session enlists before its first tick.
- Gamepad play: the platform delivers a gamepad's events; the game reads none.
- A window where the Vulkan renderer is not built — macOS: the frame is the Vulkan renderer's, as
  `--map`'s is. `--headless` runs everywhere.
- More than one player: the session is one issuer and its follower in one process.
