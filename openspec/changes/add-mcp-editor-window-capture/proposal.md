## Why

The only way to prove today that the editor window shows the engine's frame is `smoke.editor_window`.
That test drives real X11 keyboard and pointer events through XTEST, which takes over the machine for
the whole run. A person using the same desktop also breaks the run: a Caps Lock press during the
issue #18 verification failed four of twelve runs. The MCP read surface cannot fill the gap.
`viewport:` returns the engine's frame, not the window a person sees, and it carries pixels only on
macOS. On Linux it answers that the image is on the device and carries no bytes.

## What Changes

- Add an `editor:window` MCP resource. It returns a PNG of the desktop editor window exactly as it
  was composited, including panels, overlays, the palette, and the engine image inside the viewport
  panel. Linux and macOS use the same path.
- Add `editor:window?panel=<kind>`, which crops the capture to one visible dock panel such as
  `viewport`, `hierarchy` or `inspector`, using the editor's own layout. Tests no longer need to hard-code
  window proportions.
- The capture comes from the window's own renderer through egui's screenshot request. The answer
  arrives on the next UI frame, so the request waits in the agent queue without blocking the window.
  No input is synthesised and no input grab is taken.
- A headless session, a panel that is not visible, and a window that has not drawn a frame are all
  refused, and each refusal names its reason and a remedy.
- A window capture is charged as one render against the connection's budget. It is audited as a
  render request and listed with the other resources.
- Add `smoke.editor_window_mcp`, which runs the editor with `--mcp` beside the hosted runtime. It
  checks through `editor:window?panel=viewport` that the viewport panel shows the engine's image and
  that an MCP transform changes it. It also checks that undo restores the image. It does not use XTEST.
- Out of scope: Linux `viewport:` pixel readback from the dma-buf. `editor:window` answers the
  "what is on screen" question on both platforms; engine-frame bytes on Linux remain a separate task.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `editor-agent-interface`: adds a requirement that an agent can read the editor window as a person
  sees it, whole or cropped to one panel, with its refusals and budget.

## Impact

- `editor/crates/cy-editor-agent`: new resource address, a window-capture reading, deferral of
  window reads until the shell supplies a capture, listing, audit and budget.
- `editor/crates/cy-editor-mcp`: serialisation of the window capture as a PNG `blob`.
- `editor/crates/cy-editor-shell`: requesting the screenshot, receiving it next frame, and recording
  visible panel rectangles during docking.
- `samples/05b-editor-window`: new MCP smoke driver and its CTest entry, which skips without a display.
- No engine, runtime, or wire protocol changes. The existing `viewport:` behaviour is unchanged.
