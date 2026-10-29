# Design — playing the first- and third-person samples

## Where the play host lives

`samples/play`, a static library at the `tools` layer beside the samples that use it. It names the
SDL3 platform, the render graph and the ABI's interface adapter together, which no engine layer
below `tools` may; `platform/host` stays the engine's one frame `while` (`run_host_loop`), and the
play host's loop is that loop's body. A game implements `cy::play::PlayGame` — `input`, `tick`,
`drain`, `frame`, `draw` — and `HostedGame<Host, Renderer>` implements it once for every host that
exposes a scripted frame's parts, so the two samples share every line of the window, the input, the
loop, the pause and the presentation.

## The frame, split, and the scripted run unchanged

A scripted host's `frame()` was pump, one tick, the frame. The hosts now expose those parts, and
`frame()` is the parts in their old order with one step's seconds and an interpolation of zero —
which is why the scripted reports and digests are byte for byte main's (compared, not assumed). A
tick is counted on its own; presentation advances by the frame's seconds, or by nothing while
paused.

## Presented at the frame's own time

`CyTime::interpolation` (ABI 1.3) is "how far between the last committed tick and the next the frame
is". The frame is drawn at that time: each moving thing carried forward from the last tick by its
motion over it — props, targets and the hero by their hosts (`drawn_*`), the first-person head and
the third-person focus by the Swift game, by the character's velocity. Interpolating back between
the last two ticks would have drawn the frame a tick late and, at an interpolation of zero, at the
PREVIOUS tick — every scripted frame would have changed. Carried forward, a frame on its tick moves
by nothing. The cost is an overshoot of at most one step at a collision.

## The pause, the menu and the look's sensitivity

The pause is the host's (Esc, Start, the focus) and reaches the game as `CY_TIME_PAUSED`, which the
ABI has carried since 1.3. The menu is the game's: CyberdyneKit's `PauseMenu`, a `.cyui` document
the game loads and owns. The look's sensitivity is a player's setting, and the first-person camera
is the one place the spec lets it be applied (`FirstPersonLookSettings::sensitivity`, "applied once,
here"), so the menu hands it to the game and the game reconfigures its camera — the adapter keeps the
look's state across a reconfigure, for exactly this. A host-owned menu would have needed a way to
hand the setting to the game the ABI does not have; a game-owned menu needs a way to ask the host to
resume or quit, and that is two widget ids the host notes as their clicks pass: `play-resume`,
`play-quit`. No component, no callback, no ABI entry.

## Input

The platform's devices feed the game's input server and its actions resolve them; the played game's
bindings are appended after the scripted run's so their indices — and the arbitration, which breaks
ties by the lower index — are unchanged. The mouse's look is the pointer's per-frame counts, not an
action resolved per tick, so a display faster than the simulation turns smoothly. A paused frame
resolves the input again at the tick it stopped on (`drain`): the menu's cursor moves, and nothing
pressed during the pause reaches the next tick as an edge — the click on Resume fires nothing.

## The picture

The game's renderer draws at its own extent on the play host's device and leaves its output sampled
(a hand-off pass). The presenter samples it into the swapchain image inside a letterbox (one
full-screen triangle, `shaders/present.slang`), draws the interface over it at the window's
resolution, and presents. A resize rebuilds the swapchain and the interface renderer, never the
game's frame, whose GI, upscaler and targets are sized once. V-sync is FIFO; without it, immediate.

## Testing a played game

Nothing calls the game. Input scripts — frame, verb, arguments — are pushed through
`Sdl3EventInjector` onto SDL's queue, a pad through SDL's own virtual joystick; `--headless` is SDL's
dummy video driver with no device, so the play tests run in CI; `--fixed-clock` makes a frame one
tick, so a run is reproducible. The drawn suites skip on a machine with no display or device.

## Alternatives not taken

- **An ABI entry for the pause or for quitting.** Two widget ids carry what is needed, and the ABI
  stays where it is while other tracks take its next minors.
- **Rebuilding the game's renderer on a resize.** Its frame, GI and upscaler are sized at start;
  rebuilding them re-uploads the level and stalls for seconds.
- **The look through an action with a `SettingScale` modifier.** That would apply the sensitivity a
  second time, which `camera-system` forbids, and resolve the look per tick.
