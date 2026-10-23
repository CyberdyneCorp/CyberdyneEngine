# A real UI toolkit: widgets, documents, animation, opacity groups, HDR, and the Swift API

## Why

CyberUI reached the screen in stage 1 of issue #91 with a store, a layout, `.cyss`, a renderer, a
developer console and a strategy HUD — and no widgets. A game that wanted a settings menu wrote its
own slider out of rectangles, its own focus rules and its own text field, in C++, and a Swift game
could set five element kinds one write at a time. `ui-system`'s "Widget set", "UI document asset",
"Animation and transitions", "Semantic input actions" and the opacity-group and HDR scenarios of
"Rendering" were answered by exemptions. This change is stages 2, 4 and 5 of issue #91.

## What changes

- `cy::ui-toolkit` (`src/ui/toolkit/`), a new module: `Toolkit`, a retained widget tree on the same
  `ElementStore` — panel, row, column, grid, stack, scroll view, label, rich text, image, button,
  toggle, checkbox, radio group, slider, progress bar, text field (caret, selection, IME
  composition, undo, clipboard, masks), drop-down, virtualised list view, tabs, tooltip and modal
  dialog — with hover, press and drag, keyboard and gamepad focus (directional, tab order, a focus
  ring), a built-in theme over `.cyss` with per-part selectors that take the owning widget's states,
  transitions, animations (fade, slide, scale, background; seven easings; staggered; reduced
  motion), and one- and two-way bindings that wait while the player edits.
- Documents: `.cyui`, a text form with components (`use` with parameters), sheets and bindings;
  `DocumentInstance` loads it and reloads it as a diff that keeps matched widgets and their state;
  `DocumentHost` hot-reloads it from the VFS on a settled save; the build graph's `ui` producer cooks
  it into a `cyuicook 1` bundle with everything it reaches, read by `BundleResolver`.
- `cy::ui-input` (`src/ui/input/`), a new module: the `UI.*` actions in the input server with
  default keyboard and gamepad bindings, and `UiInputBridge`, which delivers them and sets the input
  user's focus layer to `Interface` while the toolkit holds the input — so gameplay bindings on the
  same keys are suppressed by the input system. The input server's shadow chain no longer lets one
  binding of an action shadow another binding of it in the same context (a bug the bridge's default
  bindings found).
- `cy::ui`: `ElementFlags::OpacityGroup`, a render transform (`PaintData::translate`, `scale`) and
  `PaintData::intensity`; `flatten()` writes groups after the main stream with batches of their own;
  `UiAction::FocusNext`/`FocusPrevious` and `Interaction::focus_next`; navigation skips elements
  under a collapsed or hidden ancestor; `.cyss` colours are premultiplied as they are read; absolute
  children keep their margins; new style properties for transitions and the render transform.
- `cy::ui-render`: one graph pass per opacity group into a transient target and a composite
  primitive; an HDR composite — `FramePassKind::UiHdr`, `FrameSinks::ui_hdr`, a renderer made for
  a float format decoding to linear light at `scene_scale` — before bloom.
- ABI 1.12: sixteen `ui_*` entries reaching the toolkit through `UiBackend`'s appended virtuals and
  `UiAdapter::attach_toolkit`; `CyUiEventKind` CHANGED, SUBMITTED, OPENED and CLOSED. The Swift
  overlay and Rust SDK are regenerated. CyberdyneKit's `Widget`, `UI.loadDocument`, `UIAnimation`,
  and the declarative `WidgetNode`/`mountWidgets` with `UIWidgetBinding`.
- Tests: `unit.ui_toolkit`, `unit.ui_input`, `integration.ui_toolkit`, `render.ui_toolkit`, cases in
  `unit.ui`, `unit.ui_render`, `unit.input_actions`, `unit.abi`, `integration.game_backend_ui`,
  `integration.build_content`, and `UIToolkitTests` — each new behaviour proven red by a mutation
  (`evidence/falsification.txt`).

## Scope

Not built: custom UI materials and blur-behind, world-space and surface-space documents, the
immediate-mode API, the inspector, the visual designer and an editor preview panel, keyframes in a
sheet, multi-line text editing, and TreeView, SplitView, MenuBar, ContextMenu, ColorPicker,
NumericField and Separator. Metal and D3D12 shaders are compiled and not run on this host.
