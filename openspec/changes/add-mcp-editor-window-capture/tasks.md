## 1. Agent read surface

- [x] 1.1 Add `WindowCapture` (PNG, window size, returned rectangle, panel kind, frame sequence) and `Reading::Window` to `cy-editor-agent`, and parse `editor:window` and `editor:window?panel=<kind>`. Unit tests should accept both forms and refuse an unknown kind with the list of kinds.
- [x] 1.2 Crop a capture to a panel rectangle given in points and `pixels_per_point`, clamping to the image, and re-encode it as PNG. Unit tests should cover a crop at scale 1 and 2, a rectangle clamped at the edge, and a panel that was not drawn, which is refused.
- [x] 1.3 Charge one render per answered window read and audit it as a render request; do not charge a refusal. A unit test should read until the budget refuses and check the audit trail.
- [x] 1.4 List `editor:window` and `editor:window?panel=viewport` in the connection's resources with their descriptions. A unit test should check that the listing includes them and that the headless session refuses them because it has no window.

## 2. Desktop queue

- [x] 2.1 Add `DesktopAgentHost::wants_window_image` and `provide_window_capture`. Make `pump` hold window reads until a capture is present while serving other requests in order, answer all waiting window reads from one capture, and refuse after the bounded wait. Unit tests should cover deferral, a mixed queue, a shared capture, and the timeout.
- [x] 2.2 Add `AgentResponse::Window` and encode it in the MCP server as an `image/png` blob with a description of the window size, rectangle, and panel. A unit test should decode the JSON and compare the PNG bytes.

## 3. Shell capture

- [x] 3.1 Record `(kind, rect)` for each panel `Panels::ui` draws in a frame, and keep the list for the frame a screenshot is taken in. A shell test should check that a default layout reports the viewport, hierarchy, and inspector.
- [x] 3.2 When a window read is queued, send `ViewportCommand::Screenshot` with a sequence number and request a repaint. Receive the matching `Event::Screenshot`, encode it as PNG, and give it to the agent host with that frame's panel rectangles on Linux and macOS. Verify by reading `editor:window` from a live desktop session on Linux.

## 4. MCP smoke

- [x] 4.1 Write `samples/05b-editor-window/mcp_window.py`, which runs the hosted runtime and the editor with `--mcp`. It checks the viewport panel's chroma, that a translate changes the panel, that undo restores it, and that the whole window contains the hierarchy, without sending any input. Verify it passes locally on Linux.
- [x] 4.2 Register `smoke.editor_window_mcp` with the same display and runtime guards as `smoke.editor_window`, skipping with code 3 when they are missing. Verify that it fails on `worlds/city.cyworld` (black viewport) and passes on `worlds/city-blocks.cyworld`.
- [ ] 4.3 Run `smoke.editor_window_mcp` 20 times each in dev and debug while the desktop stays in use, and record the result in the sample README.

## 5. Documentation and gates

- [x] 5.1 Document `editor:window` in the editor MCP section and the sample README. Run strict Clippy, `cargo test` for the touched crates, the format check, and `openspec validate add-mcp-editor-window-capture --strict`.
