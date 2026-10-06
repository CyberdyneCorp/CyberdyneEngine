# Falsification

Each mutation was applied to the named file, the named target rebuilt, the suite run, and the file
restored with `git checkout` and its md5 compared with the one taken before the edit. Every row
failed the cases listed and passed again after the restore.

| # | File | Mutation | Suite | Cases that failed |
|---|---|---|---|---|
| 1 | `src/abi/src/game/ui_thunks.cpp` | `kUiPhases = kPhaseAny` (a fixed step allowed) | `unit.abi` | the interface is presentation: a fixed step is refused |
| 2 | `src/abi/src/module.cpp` | `BehaviourRuntime::ui_event` ignores the owner | `unit.abi` | an interface event reaches the behaviours on its owner |
| 3 | `src/game_backend/ui/src/ui_backend.cpp` | the release is matched against the pressed button rather than where the pointer is | `integration.game_backend_ui` | a press and a release over one button is one click |
| 4 | same | a handle is accepted whatever its record says | `integration.game_backend_ui` | the root is the embedder's and every module element is made under it |
| 5 | same | a zero preferred size passed through as zero | `integration.game_backend_ui` | anchors and offsets place an element as the C++ store does |
| 6 | same | `set_progress` marks only the fill Arrange — the code as first written | `integration.game_backend_ui` | a progress bar's fill covers its fraction of the track |
| 7 | `samples/13-rts-api/host/rts_host.cpp` | the host never calls `BehaviourRuntime::ui_event` | `integration.rts_api_sample` | the 1.3, 1.5 and 1.6 cases (no Build click: one worker fewer) and the no-systems control |
| 8 | `samples/13-rts-api/game/Commander.swift` | no `UI.hitTest` guard on a left click | `integration.rts_api_sample` | the 1.6 case and the 1.3 case (the Build click deselects the unit) |
| 9 | `samples/13-rts-api/game/Hud.swift` | the selection panel one unit to the right | `render.rts_api_hud` | (a) stream, (b) device frame, (c) golden |
| 10 | same | the health colour one step bluer | `render.rts_api_hud` | (a), (b), (c) |
| 11 | `bindings/swift/Sources/CyberdyneKit/BehaviourBridge.swift` | the `ui_event` thunk never runs a button's action | `swift test --filter UITests` | the two click-routing cases |

## The bug the adapter suite found

Row 6 is not invented: `set_progress` first marked only the fill `Arrange`. CyberUI places an
element in its parent's arrange, and a dirty element under a clean parent keeps the rect it had
(`src/ui/src/layout.cpp`), so a health bar written after the first layout kept its old width. The
progress case's second half — `ui_set_progress(bar, 0)` after a layout — failed with a width of 32
against 0 on the first build; the fix marks the track, and the case is the regression test.
`samples/13-rts-selection/hud.cpp` never hit this because it also marks the selection panel.

## Events a receiver raises while they are delivered

The embedder first delivered with `for (event : ui.events()) runtime.ui_event(event);` and then
`clear_events()`. A click handler that calls `ui_set_focus` queues a BLUR and a FOCUS into the
array being walked: they were appended past the walk's end, then cleared, so the module never
learned that focus moved (and the append could reallocate the array under the walk).
`UiAdapter::drain_events` moves the batch out before delivering, so what a receiver raises waits for
the next frame. `integration.game_backend_ui`'s "a receiver that moves focus while handling a click
loses none of the events" failed against the old walk-then-clear with 2 events delivered of 4.
