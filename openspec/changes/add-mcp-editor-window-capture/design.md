## Context

A desktop MCP session already exists. `cyberdyne-editor --mcp` serves JSON-RPC on stdin and stdout
from a transport thread. Requests queue in `DesktopAgentHost`, and the UI thread drains a bounded
number per frame with `pump`, after it applies the person's intents. Reads go through
`AgentSession::read`. Text resources return `Reading::Text`, and `viewport:` addresses return
`Reading::Image(Observation)`, which the MCP server encodes as a base64 `blob`.

The `viewport:` path has pixels only on macOS. There, `EditorWindow::capture_agent_viewport` copies the
IOSurface into the focused viewport stream before the pump. On Linux the stream carries a
`SharedTexture` handle, so `Observation::bytes()` is `None`. Nothing captures the window itself on
either platform.

The window is eframe 0.36.1 on wgpu. egui-wgpu renders each frame into a capture texture before it
presents the frame. After `ViewportCommand::Screenshot(UserData)`, it copies that texture into a
buffer, maps it asynchronously, and delivers `egui::Event::Screenshot { user_data, image }` at the
start of a later frame. It paints and captures only while the window is visible.

## Goals / Non-Goals

**Goals:**
- One capture path for Linux and macOS, owned by the shell and served through the existing agent
  queue.
- A panel crop that uses the rectangles the dock drew in the captured frame, so the crop and the
  pixels come from the same layout.
- A smoke test that uses only MCP, runs beside a person's work, and still fails on a black viewport.

**Non-Goals:**
- Linux `viewport:` bytes from the dma-buf.
- Capturing native menus, OS window decorations, or other applications. egui draws the editor's
  menus, so they are included. OS title bars are not.
- Replacing `smoke.editor_window`. It remains the proof of real keyboard and pointer input.

## Decisions

**D1. Capture with egui's screenshot, not an X11 or OS grab.** `ViewportCommand::Screenshot` reads the
window's own render target before presentation. The result is independent of occluding windows,
compositors, and display servers, and it matches pixel for pixel what the editor presented.
Alternatives rejected: `XGetImage` or `xwd` are X11-only, include other windows that cover the
editor, and fail under Wayland. `CGWindowListCreateImage` is macOS-only and needs screen-recording
permission. A second egui render to an offscreen target would be a second presentation of the
window, which the spec forbids.

**D2. The address is `editor:window`, with an optional `?panel=<kind>`.** It uses the same
resource-read entry point as `viewport:`. `<kind>` is the dock panel kind the layout already stores
(`viewport`, `hierarchy`, `inspector`, …), not a translated title. The editor's resource listing
includes both the bare address and the query form.

**D3. The UI thread defers the request.** `DesktopAgentHost` gains a `wants_window_image()` check,
parallel to `wants_viewport_image()`, and `provide_window_capture(capture)`.
- On a frame where an `editor:window` read is queued and no screenshot is outstanding, the shell sends
  `Screenshot` with a sequence number in `UserData` and calls `request_repaint`.
- When the matching `Event::Screenshot` arrives, the shell encodes it as a PNG. It hands the PNG to the
  host together with the panel rectangles recorded for that frame.
- `pump` leaves window reads at the front of its queue until a capture is present. Then it answers
  them with `Reading::Window(WindowCapture)` and clears the capture. One capture can answer all window
  reads waiting in that frame, with each crop cut from the same image.
- Other requests are not held behind a deferred window read. The pump skips over it and serves the
  rest in order.
A synchronous capture is not possible, because the readback completes on a later frame. Blocking
the UI thread on the map would stall the window, which the budget requirement forbids.

**D4. Record panel rectangles during docking.** `Panels::ui` already dispatches on `tab.kind()` for
every drawn panel. It records `(kind, ui.max_rect())` in a per-frame list, the same way
`on_tab_button` records tab rectangles. A tab that is not drawn in a frame is not in the list, so a
hidden panel is refused rather than cropped from stale geometry. Rectangles are in points. The crop
multiplies them by the frame's `pixels_per_point` and clamps the result to the image.

**D5. A dedicated reading type rather than an `Observation`.** `Observation` carries a
`ViewportId`, overlay state, and a degradation value, and has a `represents_the_shipping_frame`
method. A window capture is none of those. `WindowCapture` holds the PNG, the window size in
pixels, the returned rectangle, the panel kind if any, and the frame sequence number. The MCP server
encodes it as an `image/png` blob with a text description, the same way `image_result` does.
`AgentRequest` needs no change, because the request is an ordinary `ReadResource`. `AgentResponse`
gains a `Window` variant.

**D6. Budget and audit match `observe`.** Before capturing, the session charges one render through
`Spending::charge_render` and records `RenderRequest` and `Cost` audit entries. A refusal does not
charge. Refusals are:
- headless: the session has no window provider;
- unknown panel kind: the refusal lists the kinds the editor has;
- panel not drawn in the captured frame;
- a capture that has not arrived within a bounded wait (default 2 s).
The wait uses the host's clock, so a minimised window gets a refusal rather than an indefinite wait.

**D7. The smoke driver speaks MCP over stdio.** `samples/05b-editor-window/mcp_window.py` starts the
runtime the way `window.py` does. It starts the editor with
`--open worlds/city-blocks.cyworld --host <socket> --mcp --agent-scope author`, then sends
JSON-RPC over stdin and stdout. It does not import `python-xlib` or any XTEST helper, so it has no
way to send input. The CTest entry
`smoke.editor_window_mcp` has the same guards as `smoke.editor_window`: it needs a display and the
runtime target, and it skips with code 3 otherwise. Its acts:
1. Read `editor:window?panel=viewport` and require chroma ≥ 40 in the panel, the same threshold
   `window.py` uses.
2. Select Pillar and translate it through the registered commands, then require the panel capture to
   change.
3. Run `edit.undo` and require the capture to return to the first within a tolerance.
4. Read the whole window and require the hierarchy panel to be present and inside it.

## Risks / Trade-offs

- [The window is minimised, or the compositor stops painting a hidden window] → D6 refuses
  after a bounded wait and names the cause. The smoke test runs with the window mapped.
- [The engine image changes between captures (orbit, frame pacing), so "unchanged after undo" is
  noisy] → The smoke world has no animation and the editor camera is still. Comparison uses a mean
  absolute difference threshold, not exact equality.
- [Large windows produce multi-megabyte PNGs over stdio] → PNG compresses a flat UI heavily. A
  capture is already bounded by the render budget, and `?panel=` lets callers ask for less.
- [The window can show private project content] → The read is available at the default `read` scope
  like other resources, and it is audited and budgeted. A connection sees no more than a person at the
  window sees.
- [The window gains focus when it opens and could interrupt the person] → This happens only at
  start-up, as with any launched application. No input is sent after that.

## Migration Plan

This is additive and needs no migration. Existing `viewport:` reads and `smoke.editor_window` are
unchanged. To roll back, remove the resource and the test.
