# Design: a playable RTS

`docs/guides/playing-the-rts.md` is the long form; this records the decisions.

## The input comes through the platform, and a test's does too

A person's key reaches the game through SDL's queue, `Sdl3InputSource`'s watch, its scancode table,
the input server's accumulation window, the tick's resolution and an action. A test that injected
into the input server would skip the first four. So the written hand pushes real `SDL_Event`s
(`Sdl3EventInjector`, inside `platform/desktop-sdl3` because no SDL type may appear above it), and
`--play --headless` brings up SDL's event subsystem alone, which CI's runners have.

## The view is fixed, the window is not

The game lays out, picks and hit-tests in a 1280 by 720 view and the frame is drawn at a fixed 16:9
extent, because its targets are sized once. The window is drawn onto letterboxed, and the inverse
letterbox is applied where the pointer is first read — `Sdl3InputSource`'s pointer mapping — rather
than in every reader. A frame drawn at the window's size would have to resize every stage on every
drag; a pointer converted in the game would put window coordinates in the game.

## A lockstep game's orders are commands, one a unit

The session addresses groups. A player's selection is any set of units, so a skirmish enlists a
group a unit and an order to n units is n commands, each to a formation slot computed in `Fixed`.
The opponent gives the same commands from the session's state. A played session is then its command
log, and a replay records the logged orders before each tick into a fresh session, which ends in the
same digest; the game gives none in a replay.

## The game asks the host to pause

Only the host can stop the clock, and only the game may decide to. A new ABI entry would do it; a
component the game writes and the host reads after the frame does it with no ABI change and keeps the
decision in Swift. A held frame resolves input on the frame's tick, so the key that resumes is heard.

## Presenting is timed apart

The frame's graph knows nothing of the swapchain; a second graph draws the output onto the acquired
image and presents it with the semaphores. The frame's cost is then what `smoke.rts_api_map`
measures, and waiting for the compositor — seconds, once, under load — is reported apart.

## Its own loop beside `samples/play`

`samples/play` (#149) is the first- and third-person games' play host: relative mouse, a pause the
host owns on Esc and focus loss, interpolated frames. The RTS needs the opposite of two of those —
an absolute pointer the game picks with, and a pause the game decides — and its fixed steps are a
lockstep session's, which a replay drives tick for tick. So it keeps its own small loop
(`host/play.*`) over the same platform pieces; folding the two hosts into one is a follow-up.
