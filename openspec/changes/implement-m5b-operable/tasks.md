# Tasks: M5.5 — Operable

Ordered. The toolkit spike first, because it is the one decision here that is expensive to reverse.

## 0. Spike — the toolkit, on one criterion

- [ ] 0.1 Evaluate candidate Rust interface toolkits against the criterion that dominates:
      **can the viewport present an engine-rendered GPU image without a copy through the CPU?**
- [ ] 0.2 Measure the alternative honestly — device-to-host-to-device per frame at 1080p — so the
      cost of the wrong choice is a number rather than an intuition
- [ ] 0.3 Choose, and record the reasoning where a future reader will find it
- [ ] 0.4 Confirm the layer above stays toolkit-agnostic, so the decision remains reversible

## 1. The editor opens — `editor-ui-ux` → Working

### 1.0 Three things the spike handed back, before any panel exists

- [ ] 1.0.1 **Cross-process synchronisation.** wgpu does not enable `VK_KHR_external_semaphore_fd`,
      so an imported semaphore cannot be created on its device as configured. Fence waits are correct
      but serialise. Use `WgpuSetup::Existing` to supply our own `VkDevice`, or establish that
      serialising is acceptable and say why. **First task of the viewport work, not a later
      discovery** (`design.md` §1).
- [ ] 1.0.2 **Widen `FrameImage::SharedTexture`** from `{ handle: u64 }` to carry fd, DRM format
      modifier, stride, offset, size and fourcc. Vulkan and DRM facts, not toolkit facts, so the
      layer stays toolkit-agnostic. `DRM_FORMAT_MOD_LINEAR` is **not** supported on this hardware,
      so the negotiated modifier must travel with the image.
- [ ] 1.0.3 **A toolkit-containment test**, in the shape of `cy-editor-app/tests/layering.rs`, that
      fails if `egui`, `eframe`, `egui_dock` or `wgpu` appears in any crate's dependencies except the
      one render crate. Without it the boundary erodes, because importing `egui::Color32` into
      `cy-editor-visual` is locally reasonable every single time.
- [ ] 1.0.4 Move the MSRV to 1.95 in `editor/Cargo.toml` and on every CI leg; egui 0.36 refuses to
      build on 1.92.
- [ ] 1.0.5 Adopt Dear ImGui's `WindowKey` idea for our own `PanelId` ↔ title mapping — stable
      identity separated from display title, which is what a persisted workspace needs across a
      renamed or localised panel.


- [ ] 1.1 A window, and the application shell
- [ ] 1.2 Docking, floating, tabbing, and named workspaces that persist and reset
- [ ] 1.3 The hierarchy, the inspector generated from reflection, the content browser
- [ ] 1.4 The command palette over the registry M5 built; keyboard-first operation with chords
- [ ] 1.5 Density modes; validation surfacing; notifications that do not interrupt
- [ ] 1.6 **Read `docs/design/editor-visual-language.md` and look at both reference images in
      `docs/design/images/` before writing interface code.** They have waited since M3 for this
      milestone and they are normative, not inspirational (`design.md` §5b).
- [ ] 1.7 `editor-visual-language` → Working: viewport-first hierarchy with charcoal chrome; the
      semantic palette (gold selection, green live, orange warning, red error); **X red, Y green,
      Z blue everywhere** including the inspector's vector fields; selection as a thin gold outline
      that does not glow; gizmos distinguishable by **shape** — arrows translate, arcs rotate, boxes
      scale; the orientation widget that shows three axes and is **not** a manipulator; chrome as
      overlay rather than a second toolbar; surfaces by luminance step rather than cards; tabular
      figures; engine vocabulary throughout
- [ ] 1.8 Do **not** copy the four things the references get wrong, each recorded in
      `docs/design/editor-visual-language.md`: Unreal's vocabulary, the profiler mislabelled as
      "World Partition", the inconsistent branding, and the universal gizmo at the edge of
      readability

## 2. The viewport shows the engine's frame

- [ ] 2.1 The viewport presents the engine's own rendered image (`design.md` §2) — **no second
      renderer, no toolkit-drawn approximation**. If the transport is not ready it says so.
- [ ] 2.2 Camera navigation
- [ ] 2.3 Selection by clicking, over M5's engine-side picking
- [ ] 2.4 Translate, rotate and scale by gizmo, at the latency M5's bridge spike measured — one
      transaction per manipulation, and a drag returned to its origin restores exact values
- [ ] 2.4.1 **Build the gizmo against `docs/design/images/transform-gizmo.png`**, which is normative:
      Move `W`, Rotate `E`, Scale `R`, Universal `T`; axis arrows with **planar handles** at the axis
      pairs; rotation rings plus an outer screen-space ring; box handles with a **centre cube for
      uniform scale**; and a centre carrying three separately targetable affordances — screen move,
      uniform scale, screen rotate
- [ ] 2.4.2 **Constant screen size regardless of camera distance.** Drawn in world units a gizmo
      becomes unusable exactly when precision matters most, and the failure is gradual enough that
      nobody files it (`design.md` §5c)
- [ ] 2.4.3 **Three states: normal, hover, active** — the hovered handle emphasised *before* the
      press. Encode the active state by luminance and saturation lift on the handle being dragged,
      **not** by recolouring it: the reference tints active red, which collides with red meaning both
      the X axis and error (`design.md` §5c)
- [ ] 2.4.4 World and Local space; Pivot, Centre, Bounds and Individual pivot modes
- [ ] 2.4.5 Snapping with configurable increments, grid and surface; `Ctrl` temporary snap, `Shift`
      precision, `Alt` duplicate-and-transform, `X`/`Y`/`Z` axis lock
- [ ] 2.4.6 Per-axis numeric entry for position, rotation and scale
- [ ] 2.4.7 **The scene orientation widget**, against
      `docs/design/images/scene-orientation-gizmo.png`: click an axis to snap the camera, drag
      anywhere to orbit, scroll to zoom, modifier-drag to pan; seven view presets; the current view
      as cycleable text; constant screen size 56–96 px, 72 default; normal, hover, active and
      disabled states in both themes. **Dragging orbits the camera and produces no transaction.**
- [ ] 2.4.8 Prove the two gizmos are unmistakable with both on screen at once
      (`docs/design/images/editor-scene-view.png` is the case), separated by form as well as by size
      and position — the widget carrying no rings, no planar handles, no scale boxes
- [ ] 2.4.9 Decide which of the three reference layouts is the shipped default, and say so. The
      composition requirement fixes regions rather than pixel positions and all three satisfy it,
      but a default left unstated becomes whichever reference a contributor opens first.
- [ ] 2.5 View modes and overlays; degradation under load

## 3. The agent interface — `editor-agent-interface` → Working

- [ ] 3.1 The MCP transport behind an engine-owned interface, gated at build time
- [ ] 3.2 Tools projected from the command registry, with declared exclusions carrying reasons
- [ ] 3.3 Resources: scene hierarchy, an entity's properties, selection, assets, diagnostics
- [ ] 3.4 **Viewport observation** returning the engine-rendered image, with overlay state declared
- [ ] 3.5 Manipulation through the interactive path — pivot, space, snapping, constraints
- [ ] 3.6 **Source authoring** (`design.md` §4): create and edit project scripts as transactions,
      with the effect class computed from whether the prior contents can be restored
- [ ] 3.7 Build and reload triggered by the agent, over M4's proven model
- [ ] 3.8 Play mode entered and left
- [ ] 3.9 Transactions carry actor, session and intent; history shows them
- [ ] 3.10 Scope and effect class enforced; confirmation only where undo cannot reach
- [ ] 3.11 The editor stays usable while an agent works; a human action wins a conflict
- [ ] 3.12 Budget: invocation rate, render cost, concurrency, reported to the agent

## 4. The artefacts

- [ ] 4.1 `samples/05b-editor-window` — open a project, select an object, drag a gizmo, undo, save.
      Needs a display; skips loudly where there is none.
- [ ] 4.2 `samples/05b-agent-authoring` — an agent composes a scene from an empty project, writes a
      gameplay script, reloads it, enters play mode, and **captures the viewport to confirm what it
      built**. Scriptable, and therefore in CI.
- [ ] 4.3 A screenshot of each, committed under `docs/design/images/`, so the milestone can be
      evaluated by looking — and so the concept art can be compared against what was actually built
- [ ] 4.4 Where the built editor and the reference imagery differ, say which is right. If the
      implementation is right, replace the reference; `editor-visual-language` requires a reference
      that no longer reflects the product to be replaced rather than left to decay.

## 5. Closing the milestone

- [ ] 5.1 The editor opens on a project and a person can select and manipulate
- [ ] 5.2 The viewport image is the engine's, proven by comparison against a direct render
- [ ] 5.3 A gizmo drag returned to its origin restores exact values, in one transaction
- [ ] 5.4 An agent composes a scene, writes and reloads a script, and observes the result
- [ ] 5.5 An agent's source write is a transaction with an actor and an intent
- [ ] 5.6 No agent capability exceeds a human one
- [ ] 5.7 All four profiles build clean and `just test-all` is green in each
- [ ] 5.8 `just roadmap-milestone m5b` exits zero
- [ ] 5.9 Update `status.yaml` and `capability-matrix.md`, including M5's corrected `editor-ui-ux` row
- [ ] 5.10 `openspec validate --specs --strict` passes; archive this change
- [ ] 5.11 Open the M6 change — the derivation key model is its named spike
