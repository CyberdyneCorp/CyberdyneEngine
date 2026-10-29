# Runtime UI in CyberEngine

How a game puts an interface on screen with CyberUI: build elements in the store, lay them out,
attach text, flatten them, and hand the stream to the interface pass. The route goes through
[`src/ui/README.md`](../../src/ui/README.md) and
[`src/ui/render/README.md`](../../src/ui/render/README.md); the contract is
[`ui-system`](../../openspec/specs/ui-system/spec.md).

![A strategy HUD and the developer console over samples/13-rts-selection](../design/images/rts-hud-2x.png)

*`samples/13-rts-selection`: the squad the drag box caught listed bottom-left, resources across the
top, the minimap with the camera's rectangle bottom-right, and the developer console — all one
`ElementStore`, one `flatten()` and one pass.*

## Contents

1. [The pieces](#1-the-pieces)
2. [Building an interface](#2-building-an-interface)
3. [Text](#3-text)
4. [Drawing it](#4-drawing-it)
5. [The developer console](#5-the-developer-console)
6. [A HUD from game code](#6-a-hud-from-game-code)
7. [The widget toolkit](#7-the-widget-toolkit)
8. [Documents, style sheets and hot reload](#8-documents-style-sheets-and-hot-reload)
9. [Bindings and animation](#9-bindings-and-animation)
10. [Input, focus and navigation](#10-input-focus-and-navigation)
11. [Opacity groups and the HDR composite](#11-opacity-groups-and-the-hdr-composite)
12. [The toolkit from Swift](#12-the-toolkit-from-swift)
13. [The UI layout editor](#13-the-ui-layout-editor)
14. [Menus: settings, rebinding and player files](#14-menus-settings-rebinding-and-player-files)
15. [Testing](#15-testing)
16. [Not built yet](#16-not-built-yet)

---

## 1. The pieces

| Target | Directory | What it does | Names a device? |
|---|---|---|---|
| `cy::ui` | `src/ui/` | the element store, layout, `.cyss`, input routing, flattening to primitives | no |
| `cy::ui-text` | `src/ui/text/` | `TextPainter`: an element's text measured and painted as glyph primitives; the interface font (with `CY_TEXT`) and the built-in fallback font | no |
| `cy::ui-console` | `src/ui/console/` | `DevConsole`, the developer console | no |
| `cy::ui-toolkit` | `src/ui/toolkit/` | `Toolkit`: the widgets, their focus, styles, animations and bindings; `.cyui` documents and their hot reload | no |
| `cy::ui-input` | `src/ui/input/` | `UiInputBridge`: the `UI.*` input actions driving the toolkit, and gameplay suppressed while it holds the input | no |
| `cy::ui-render` | `src/ui/render/` | `UiRenderer`: the stream drawn at the frame's `UiAndDebug` stage, opacity groups offscreen, and an HDR composite before the tone curve | yes |

All six are behind `CY_UI`. Elements are not entities: `cy::ui` does not link `cy::ecs`.

```mermaid
flowchart LR
    store["ElementStore<br/>(game code builds it)"] --> layout["ui::layout<br/>measure + arrange"]
    text["TextPainter<br/>(ContentMeasurer)"] --> layout
    layout --> flatten["ui::flatten<br/>primitives + batches"]
    text2["TextPainter<br/>(ContentPainter)"] --> flatten
    flatten --> submit["UiRenderer::submit<br/>rows + one draw per batch"]
    submit --> pass["ui pass at UiAndDebug<br/>after tonemap and outlines"]
```

## 2. Building an interface

An element is created in a parent, given a layout input and paint data, and flagged. Colours are
**premultiplied ARGB** (`0xAARRGGBB`).

```cpp
ui::ElementStore store(allocator);
auto root = store.create(ui::kNoElement, Name::intern("hud"));
store.layout_input(*root)->model = ui::LayoutModel::Absolute;   // children placed by anchors

auto panel = store.create(*root, Name::intern("panel"));
ui::LayoutInput& input = *store.layout_input(*panel);
input.anchor_min = input.anchor_max = Vec2{1.0F, 1.0F};          // bottom-right corner
input.offset_min = Vec2{-108.0F, -80.0F};
input.offset_max = Vec2{-6.0F, -6.0F};
ui::PaintData& paint = *store.paint(*panel);
paint.background = 0xFF1E3A24U;
paint.border_width = 1.0F;
paint.border_colour = 0xFF8FA3B5U;
paint.corner_radius = 3.0F;
(void)store.set_flags(*panel, ui::ElementFlags::Visible | ui::ElementFlags::ClipsChildren);
```

What the paint data means on screen:

| Field | Effect |
|---|---|
| `background` | the fill. Zero, with no border, means the element draws nothing — a layout container — and puts no primitive in the stream |
| `border_width`, `border_colour` | a border inside the bounds |
| `corner_radius` | rounds the fill, the border and an image alike |
| `opacity` | multiplies down the tree into every colour beneath |
| `material`, `atlas`, `uv` | `BuiltinMaterial::Image` samples an atlas page; `Glyph` is what text uses |
| `ElementFlags::ClipsChildren` | children are clipped to this element, intersected with every clip above |

When game state changes, write the input and mark what changed: a new colour or text of the same
size is `Dirty::Paint`; a size change is `Dirty::Measure`; a move is `Dirty::Arrange`.

## 3. Text

With the complete text backend (`CY_TEXT`, the default) CyberUI draws its text in the **interface
font** — Noto Sans, Latin, as a multi-channel distance field — through FreeType, HarfBuzz and
msdfgen (issue #86, [the text guide](text.md)):

```cpp
cy::text::CompleteTextBackend backend;          // src/backends/text-complete/; outlives the server
(void)backend.start();
cy::text::TextServer server;
(void)server.start_with(cy::text::TextServerConfig{}, backend);
ui::TextPainter text(allocator);
(void)text.start(server, cy::text::FontSource{ui::interface_font_bytes()},
                 ui::interface_font_desc(), /*first atlas page*/ 1);

auto label = store.create(*root, Name::intern("label"));
ui::TextStyle style;
style.colour = 0xFFFFD34DU;
style.size = 18.0F;                             // reference units; zero is the painter's line height
style.outline_width = 1.5F;                     // from the distance field, not eight copies
style.outline_colour = 0xFF101010U;
style.shadow_colour = 0x80000000U;
style.shadow_offset = Vec2{2.0F, 2.0F};
(void)text.set_text(*label, "Gold 1250", style);
```

A label with no preferred size is measured by the painter (pass `&text` to `ui::layout`); its glyphs
are painted after its background and before its children (pass `&text` to `ui::flatten`). A style
with no `size` draws a line exactly as tall as the built-in font's — 13 units times `pixel_scale` —
so a layout written in rows of that font keeps every row. A line in one face is one batch: the
shadow pass and the glyphs share the distance-field page and the `GlyphField` material.
`set_colours` gives ranges of a label their own colour; `gradient_colour` blends across the line.

![A paragraph and an outlined, shadowed title in the interface font](../design/images/text-outlined.png)

**The built-in font stays as the fallback.** It is the X Window System's public-domain `misc-fixed`
6x13, printable ASCII, compiled in as data (`src/ui/text/src/builtin_font_data.h`, generated by
`src/ui/text/tools/make_builtin_font.py`; provenance in `deps/shipped-content.md`). It is the last
face of the interface font's chain, and with `CY_TEXT` off it is the whole of the interface's text,
started exactly as before:

```cpp
(void)server.start(cy::text::TextServerConfig{});
(void)text.start(server, ui::builtin_font(), /*atlas page*/ 1);
```

Draw it at whole-number `pixel_scale`s; it is point-sampled. The suites that pin a golden image
drawn before #86 (`render.ui` (a)–(g), `render.rts_api_hud`) start on it explicitly.

## 4. Drawing it

```cpp
ui::render::UiRenderer renderer(allocator);
(void)renderer.create(device, {width, height, output_format});
// outside a frame, after the labels are set: the text's pages (coverage, distance field, colour —
// cy/ui/render/text_atlas.h), again whenever text.atlas_revision() changes; and any image pages
(void)ui::render::upload_text_atlases(renderer, text);

// every frame, before the frame is assembled:
ui::LayoutReport laid;
(void)ui::layout(store, scale_settings, viewport, &text, laid);
ui::FlattenReport flattened;
(void)ui::flatten(store, ui::Rect{0, 0, viewport.x, viewport.y}, buffer, flattened, &text);
(void)renderer.submit(buffer, ui::resolve_scale(scale_settings, viewport));
sinks.ui = renderer.stage();          // rendering::assembly::FrameSinks
```

The pass runs after tonemapping, bloom, grading and the selection outlines, so a colour is the
output's bytes. With nothing to draw it records nothing and the frame is byte-identical to a frame
without it. `renderer.report()` says how many indirect draws it recorded — one per batch `flatten()`
made — and how many batches a clip emptied.

## 5. The developer console

```cpp
ui::DevConsole console(allocator);
ui::ConsoleStyle style;                      // anchors, rows shown, history, colours
(void)console.create(store, text, *root, style);
(void)console.add_command("spawn", &spawn, &game);
(void)console.type("spawn tank");            // from text input
(void)console.submit();                      // echoes, runs, prints the reply
(void)console.print("ready", ui::kConsoleWarning);
```

`help`, `clear` and `echo` are built in. The scrollback is a ring held by the console; the store
holds exactly `visible_rows` row elements whatever has been printed, and a print repaints them
without a relayout. `set_open(false)` collapses the panel out of layout and the stream.

## 6. A HUD from game code

`samples/13-rts-selection/hud.h` is a strategy HUD written as game code over the same calls:
`set_resources`, `set_selection` (one row per selected unit with a health bar) and `set_minimap`
(unit dots and the camera's rectangle, clipped by the minimap). The sample selects a squad with a
drag box, writes the selection into the HUD, and photographs the frame:

```
build/dev/samples/13-rts-selection/cy_sample_rts_selection --out docs/design/images
```

| The frame | With the HUD and the console |
|---|---|
| ![](../design/images/rts-selection-after.png) | ![](../design/images/rts-hud.png) |

### The same HUD from Swift

ABI 1.6 lets a scripting module build in the same store. `samples/13-rts-api/game/Hud.swift` is this
HUD rewritten in Swift with CyberdyneKit's declarative layer — panel for panel, colour for colour —
plus a Build button, and the RTS game there writes it from its own state every frame:

```swift
hud = try mountUI {
    Panel("resource-bar") {
        Label("0", colour: gold, name: "gold").id("gold")
        …
    }
    .layout(.anchored(min: Vec2(x: 0, y: 0), max: Vec2(x: 1, y: 0), offsetMax: Vec2(x: 0, y: 17))
                .flex(.row, gap: 4, align: .centre, padding: UIInsets(horizontal: 6, vertical: 2)))
    .style(UIStyle(background: panel))
    Button("Build", colour: text, name: "build-button") { _ in onBuild() }
}
try hud["gold"].setText("\(gold)", colour: gold)
```

![The Swift-built HUD with its Build button](../design/images/rts-api-hud.png)

*`render.rts_api_hud`: the HUD a Swift behaviour built through the ABI, over the pipeline suites'
scene. Without the button it is the C++ HUD's frame byte for byte.*

The host side is three calls on `cy::game_backend::UiAdapter` (`cy::game-backend-ui`), which
implements the ABI's `UiBackend` over the store, a `TextPainter` and an `Interaction` it owns:

```cpp
cy::game_backend::UiAdapter ui(allocator, store, text, root);
(void)ui.start();                              // the root transparent to hits, a HUD layer
cy::game_backend::bind(host, &ui);             // every ui_* entry now answers
// every frame, before the behaviours' frame update:
(void)ui.layout(scale_settings, viewport);
(void)ui.route_pointer(position, buttons_pressed, buttons_released);
input_adapter.set_pointer_focus(0, true, ui.pointer_over());
ui.drain_events([&](const CyUiEvent& event) noexcept { (void)runtime.ui_event(event); });
```

Deliver through `drain_events`, not a loop over `events()`: a receiver that moves focus or destroys
what it clicked queues a BLUR and a FOCUS while the batch is delivered, and `drain_events` keeps
those for the next frame instead of walking an array that is growing under it and clearing them.

A module reaches the root and its own elements only; the console beside them in the same store
answers `NOT_FOUND`. A press and a release of the left button over one button is a click, delivered
to the behaviours of the entity that owns it. The Swift side is in [the Swift guide](swift.md#the-interface-abi-16).

## 7. The widget toolkit

`cy::ui::Toolkit` is a retained widget tree on the same store: a widget is a handful of elements —
a slider is a track, a fill and a thumb — with a record saying what they are, so everything above
(layout, `.cyss`, flattening, the renderer) draws it with no new path. A widget's handle is its
root element (`WidgetId`).

![The widget gallery: every widget the toolkit has, from one .cyui document](../design/images/ui-widget-gallery.png)

*`render.ui_toolkit` (d): `src/ui/toolkit/tests/data/gallery.cyui` at 1920×1080, twice the
reference resolution, in the interface font — buttons, a toggle, checkboxes, a radio group, a
slider with the gamepad's focus ring, a progress bar, text fields, a drop-down, tabs over a
virtualised list, an image with a rich-text caption and a scroll view. Held to a golden image.*

| Kind | What it is | Events |
|---|---|---|
| `panel`, `row`, `column`, `grid`, `stack` | containers: anchors and offsets, flex, cells, layers | — |
| `scroll-view` | a clipped viewport, scrolled by the wheel, `UI.ScrollUp`/`Down` and by focus | — |
| `label`, `rich-text`, `image` | text; text in runs (`[color=#rrggbb]`, `[size=N]`, `[img=page,u,v,w,h]`); an atlas region | — |
| `button` | pressed and released over it, or Accept with focus | Clicked |
| `toggle`, `checkbox` | on or off | Changed (value 0 or 1) |
| `radio-group` | one of its `items`, each a `radio` | Changed (index) |
| `slider`, `progress-bar` | a value in a range, by a step | Changed (value) |
| `text-field` | one line: caret, selection, composition (IME), undo, clipboard, a mask, a length limit | Changed, Submitted |
| `dropdown` | one of its `items`, picked from a list over the interface | Opened, Changed, Closed |
| `list-view` | items from a `ListSource`, **virtualised**: only the rows in view exist | Changed, Submitted |
| `tabs` | a bar over its pages; each child is a page titled by its `title` | Changed |
| `dialog` | modal, over a dimmed backdrop, in a layer of its own; focus returns where it was | Opened, Closed |

```cpp
ui::Toolkit toolkit(allocator, store, text);
(void)toolkit.start();                               // a root of its own, and the overlay above it

ui::WidgetDesc desc;
desc.kind = ui::WidgetKind::Slider;
desc.id = "volume";
desc.minimum = 0.0F;
desc.maximum = 10.0F;
desc.step = 1.0F;
const ui::WidgetId volume = *toolkit.create(menu, desc);

// every frame: bindings, animations and transitions, styles, the lists' rows, layout, and what is
// placed after layout — the focus ring, the caret, an open drop-down's list, a tooltip
(void)toolkit.update(seconds, scale_settings, viewport);
for (const ui::WidgetEvent& event : toolkit.events()) {
    if (event.kind == ui::WidgetEventKind::Changed && event.widget == volume) {
        settings.volume = event.value;
    }
}
toolkit.clear_events();
```

A game's own `set_value`, `set_text` or `set_selected` raises no event: Changed means the player
did it. The lists' rows are made for the window, not the items — a list of ten thousand items and
a list of a hundred both hold the same twenty-five rows (`integration.ui_toolkit`).

**Measured** (this machine, the `dev` preset, an RTX 5060 for the device suite):

| What | Cost |
|---|---|
| A frame of the widget gallery on the CPU, nothing changed: `update`, `flatten`, `submit` | 64 µs (65 widgets, 374 primitives, 48 draws) |
| A frame of a scrolling 10 000-item list: new rows given items, laid out, styled, flattened | 69 µs mean, 77 µs worst — the 100-item list costs 51 µs |
| An opacity group | one graph pass and one transient target the size of the frame, one composite primitive (GPU time not measured) |

The gallery binds a page for almost every draw, because a label's glyphs and the panel under it
alternate between the shape and glyph pages; a frame with fewer, larger text runs batches better.

## 8. Documents, style sheets and hot reload

A `.cyui` document is the interface as text: one widget a line, children indented two spaces,
`#id` and `.class` after the kind, properties as `key=value`. Any `.cyss` property is accepted as a
property and becomes an inline style.

```text
cyui 1
stylesheet "ui/menu.cyss"
column #menu .panel anchor=[0.5, 0.5, 0.5, 0.5] offset=[-160, -120, 160, 120] gap=8
  label .heading text="Options"
  slider #volume min=0 max=10 step=1 value=5 bind="volume" bind-mode=two-way
  dropdown #quality items=["Low", "Medium", "High"] selected=1
  use "ui/stat.cyui" #health label="Health" value=0.7
  button #play .primary text="Play"
```

`use` instantiates another document with parameters (`param label="Stat"` in it, `$label` where
it is used). `DocumentInstance` makes a document's widgets; its `reload` is a **diff** — a widget
is matched by its id, or by its kind and position, and kept with its state (a slider the player
moved keeps the player's value, the focus stays where it was). `DocumentHost` reads a document
through the VFS and watches it and everything it reaches:

```cpp
ui::DocumentHost menu(allocator, toolkit, vfs);          // settles 100 ms before a reload
(void)menu.open("ui/menu.cyui", ui::kNoElement, &bindings);
// every frame:
if (Expected<bool, Error> reloaded = menu.poll(now_ns); reloaded && *reloaded) { … }
// a save that does not parse keeps the interface; menu.instance().diagnostics() says why
```

**Style sheets** cascade theme → sheet → inherited → inline. The toolkit's built-in theme
(`default_theme_source()`) styles every widget and its parts; a game's sheet is over it, and
`set_theme` replaces the theme itself. Selectors are a kind (`slider`), a part (`slider-thumb`),
an `#id`, `.class`es and a state — `:hover`, `:active`, `:focus`, `:disabled`, `:checked`,
`:selected`, `:open` — read from the owning widget, so `button:hover` recolours a button's
background and `button:hover button-label` its caption. `var(--name)` reads a theme variable.

**Cooking.** The build graph's `ui` producer turns a document into one `cyuicook 1` bundle holding it
and every sheet and document it reaches, each read through `NodeContext::discover` — saving a shared
sheet re-cooks only the documents that reach it. A shipped game reads the bundle through
`BundleResolver` and no project file ([`tools/build/README.md`](../../tools/build/README.md)).

## 9. Bindings and animation

A binding drives a widget property from a `BindingSource` and, `TwoWay`, writes the player's changes
back. While the player is editing — a text field with focus, a slider being dragged — the source
does not overwrite the field.

```cpp
class Volume final : public ui::BindingSource {
    ui::BoundValue read() noexcept override {
        ui::BoundValue value;                              // Kind::Number
        value.number = game.volume;
        return value;
    }
    bool changed() const noexcept override { return game.volume_revision != seen; }
    Status write(const ui::BoundValue& value) noexcept override { … }
};
(void)toolkit.bind(volume, ui::WidgetProperty::Value, source, ui::BindingMode::TwoWay);
```

A document names its bindings (`bind="volume"`) and a `DocumentBindings` finds the source by name.

**Transitions** are a sheet's: `transition-duration: 0.1s; transition-timing-function: ease-out`
on a rule makes a state change — a hover, a press — ease between the two styles. **Animations** are
code's, on paint properties only, so an animation never lays anything out again:

```cpp
(void)toolkit.fade(dialog, 0.0F, 1.0F, 0.2F);
(void)toolkit.slide(toast, Vec2{0.0F, -24.0F}, 0.25F);     // from 24 units above, to its place
(void)toolkit.pop(badge, 0.3F);                            // grows in with `Back` easing
ui::Animation pulse;
pulse.property = ui::AnimatedProperty::Background;
pulse.from_colour = 0xFF1E2A3AU;
pulse.to_colour = 0xFF3A5A8AU;
pulse.easing = ui::Easing::EaseInOut;
pulse.decorative = false;                                  // state feedback: plays under reduced motion
(void)toolkit.animate(button, pulse);
```

Easings: linear, ease-in, ease-out, ease-in-out, ease, spring and back. With
`settings().reduced_motion`, a decorative animation jumps to its end on its first frame; state
feedback still plays. Time is the `seconds` handed to `update`, so an animation is reproducible to
the frame.

## 10. Input, focus and navigation

The toolkit takes **semantic actions**, never device input. `cy::ui-input` declares the `UI.*`
actions (Accept, Cancel, the four directions, next and previous tab, focus next and previous,
scroll up and down, context) with default bindings for a keyboard and a gamepad, and
`UiInputBridge` delivers what fired each tick:

```cpp
ui::UiInputActions actions;
(void)ui::declare_ui_actions(registry, actions);
auto context = ui::make_ui_context(actions);                  // Enter, Space, a gamepad's south…
ui::UiInputBridge bridge(toolkit, input_server, user);
(void)bridge.start(actions, *context_handle, /*priority*/ 10);
// every tick, after the input server resolved it:
(void)bridge.pump();
```

The bridge sets the input user's focus layer to `Interface` while a dialog, an open drop-down or a
focused text field holds the input, and to `Gameplay` otherwise, so the **input system** drops a
gameplay action bound to the same key — the interface consumes before gameplay without a flag in
game code. Directional navigation picks the nearest focusable widget in the pressed direction,
skipping what a collapsed or hidden ancestor removed; Tab follows document order; `set_neighbour`
overrides either. The focus ring is drawn when focus moved by keyboard or gamepad, and not after a
click.

## 11. Opacity groups and the HDR composite

![An opacity group: the overlap of two children of a faded panel is the colour of either alone](../design/images/ui-opacity-group.png)

`ElementFlags::OpacityGroup` makes an element's subtree draw **offscreen** and composite once at the
element's opacity, so the overlap of two children of a fading panel is not darker than either (the
picture above, `render.ui_toolkit` (b), matches the host reference). The dialog is a group, which is
what makes its fade-in clean. `flatten` puts each group's primitives after the main stream with
batches of their own; the renderer declares one graph pass per group into a cleared transient
target, and a frame with no group declares nothing new.

![An emissive panel composited before the tone curve: it blooms](../design/images/ui-hdr-bloom.png)

**The HDR composite.** `FramePassKind::UiHdr` runs after motion blur and before bloom, on the linear
scene colour. A second `UiRenderer` made for the frame's colour format, handed to
`FrameSinks::ui_hdr`, draws a document there: a float target decodes interface colours to linear
light and `UiRendererDescription::scene_scale` multiplies them to the frame's exposure, and
`PaintData::intensity` above one makes an element emissive — the panel in the picture is bright
enough to bloom, which the same panel drawn after the tone curve is not. World-space and diegetic
documents are still to come; this is the stage they will draw at.

## 12. The toolkit from Swift

ABI 1.12 puts the toolkit behind sixteen `ui_*` entries (`cy_abi.h`, "1.12: the widget toolkit"),
and CyberdyneKit wraps them. A behaviour loads a document or builds widgets declaratively, and its
entity receives the events:

```swift
@Behaviour
final class SettingsMenu: Behaviour {
    var volume: Float = 0.5
    var quality = 1
    var menu: WidgetTree?

    override func onCreate() throws {
        menu = try mountWidgets {
            WidgetNode.column(id: "menu") {
                WidgetNode.label("Options").classes("heading")
                WidgetNode.slider(in: 0...1, step: 0.05).bind(.value(self, \.volume))
                WidgetNode.dropdown(["Low", "Medium", "High"]).bind(.selected(self, \.quality))
                WidgetNode.button("Play") { [unowned self] _ in self.start() }.appear(.pop())
            }
        }
    }

    override func onUpdate(_ delta: Double) throws {
        try menu?.refresh()          // pushes what the game changed since; only that
    }
}

// or from a document, reloaded on save:
let hud = try loadUI(path: "ui/hud.cyui")
on(.click, of: hud["build"]) { [unowned self] _ in self.build() }
try hud["gold"].setText("\(gold)")
try hud["toast"].animate(.slideY(from: -24))
_ = try UI.navigate(.navigateDown)  // a game that maps its own input
```

A binding writes the player's change back before the node's own `onChange` runs. The host side is
`UiAdapter::attach_toolkit(&toolkit, &resolver, &vfs)` and `UiAdapter::frame` in place of
`layout`; a widget the embedder made answers `NOT_FOUND` to a module, as the console does. The
Swift API is in [the Swift guide](swift.md#the-widget-toolkit-abi-112).

## 13. The UI layout editor

The editor's UI Layout panel (`editor-ui-layout`, issue #29) edits `.cyui` documents on a canvas the
ENGINE draws. The editor does not lay out or draw an interface itself: it sends the document, the
files it reaches (`use` components, style sheets) and the preview settings to the hosted runtime's
`ui.preview.render`, and shows the picture and the node rectangles that come back.

![The UI layout editor: palette, hierarchy, the engine-drawn canvas with the Play button selected, and the inspector](../design/images/editor-ui-layout.png)

| Part | What it does |
|---|---|
| Toolbar | the document; the resolution (720p to 4K, 4:3, 16:10, 21:9, portrait); DPI 1x to 2x; the scale mode (fixed pixel, or following the width, the height, the smaller or the larger ratio); the theme (built-in, or any `.cyss` in the project); and a state to preview on the selected widget: hover, pressed, focused, disabled, open |
| Palette | the toolkit's widget kinds, from the engine's `ui.catalogue.get`. Click to add into the selection, or drag onto the canvas or a hierarchy row |
| Hierarchy | the document's widgets; drag a row onto another to reparent it |
| Canvas | the engine's picture. Click selects. Dragging the body moves the widget by its offset, the corner resizes it, the two anchor handles move its anchors; one command is committed when the gesture ends. Below it, the engine's counts and its layout and draw times |
| Inspector | id and classes (the classes the theme and the sheets define are offered), the kind's properties from the catalogue, layout (anchor, offset, size, margins, gap, fill parent), a binding and its mode, inline styles, and the document's style sheets |

Every edit is a command, an undoable transaction in the open world's history, and an MCP tool of
the same name:

| Command | Does |
|---|---|
| `ui.document.create`, `ui.document.read`, `ui.document.stylesheet` | make a document with a full-screen root panel; read it back as widgets; add or remove a sheet |
| `ui.widget.add`, `.remove`, `.move` | add a widget of a catalogue kind under a parent at an index; remove one and its children; reparent or reorder |
| `ui.widget.set`, `.identity`, `.layout`, `.bind` | a property or inline style; the id and classes; anchors, offsets, size and margins; a binding |
| `ui.theme.save` | write a theme sheet |
| `ui.preview`, `ui.status` | change the canvas's settings and draw; what the engine last answered |

A widget is named `#id` or by its index. A save writes the document through the same writer the
engine uses, so the text round-trips byte for byte, and the canvas follows: an edit, an undo or a
redo of the document, a sheet it names or the theme redraws it. A game that loaded the document by
path (`loadUI(path:)`, `DocumentHost`) reloads the save while it runs, keeping its widgets' handles
and state — `samples/16-menus`' `navigate.script` saves over its main menu mid-run and its test
reads the new subtitle back.

The engine side is `cy::editor::UiLayoutService` (`src/editor_backend/include/cy/editor/ui_layout_service.h`):
a toolkit of its own, unloaded and reloaded per request, settled through its transitions, flattened
and drawn by `ui::render::draw_reference` — the renderer's own shader path on the host. Drawing the
gallery document at 320x180 lays out in 0.08 ms and draws in 0.60 ms on this machine. The panel
refuses to open until the engine answered `ui.catalogue.get`, because a palette the editor made up
could offer widgets the engine's toolkit cannot make.

## 14. Menus: settings, rebinding and player files

ABI 1.17 adds what a game's menus need beyond widgets (`cy_abi.h`, "1.17: menus"):

| Entries | What |
|---|---|
| `input_rebind_begin`, `input_rebind_poll`, `input_rebind_cancel` | rebinding through the input system's `RebindOperation`: the next control the player presses on that scheme is offered to it, under a conflict policy (`REJECT`, `SWAP`, `ALLOW`, `UNBIND_PREVIOUS`); Escape or a gamepad's Back cancels. A conflict is only another action of a context that binds this one: the interface's `UI.*` actions share a gamepad with the game by design |
| `input_binding` | the control an action's slot has on a scheme, the player's override or the authored binding |
| `input_profile_save`, `input_profile_load`, `input_profile_reset` | the player's overrides as text (`cyinputprofile 1`), keyed by the actions' stable ids |
| `video_get_settings`, `video_set_settings`, `video_modes` | resolution, quality preset, upscaler mode, fullscreen and v-sync, and the display's modes. The host's `game_backend::VideoSettingsAdapter` holds them and moves a revision the renderer rebuilds on |
| `user_data_write`, `user_data_read` | the player's files, one plain name each, written atomically in the game's user-data directory (`game_backend::UserDataDirectory`) |
| `ui_widget_set_tooltip` | the text a widget shows when the pointer rests on it |

In Swift (CyberdyneKit's `Menus.swift`):

```swift
let jump = try Input.find(action: "Game.Jump")
try jump.rebind(scheme: .gamepad, conflict: .swap)       // listen; the engine takes the next button
let state = try Rebinding.poll()                         // .listening, .applied, .conflict, .cancelled
if state.status == .applied {
    try UserData.write("bindings.cyinputprofile", text: try InputProfile.save())
}
if let saved = try UserData.readText("bindings.cyinputprofile") { try InputProfile.load(saved) }

var video = try Video.settings
video.width = 1920; video.height = 1080
video.quality = .medium; video.upscale = .quality
try Video.apply(video)                                    // the renderer rebuilds at the new output
try settings["resolution"].setItems(try Video.modes.map { "\($0.width)x\($0.height)" })
try slot.setTooltip("Health potion: restores 25 health")
```

In C++, a host binds the two new backends beside the others:

```cpp
cy::game_backend::VideoSettingsAdapter video(allocator);
(void)video.start(modes, initial);              // the display's modes and the settings in effect
cy::game_backend::UserDataDirectory files(allocator);
(void)files.start("menus-user-data");
cy::game_backend::bind(host, &video, &files);
// each frame: if (video.revision() != seen) rebuild the frame at video.current()
```

A document's bindings can reach the host's own values: `UiAdapter::set_document_bindings` hands the
toolkit a `ui::DocumentBindings`, so a module's `slider bind=volume.music bind-mode=two-way` is the
host's music bus, with no module code between.

`samples/16-menus` is the worked example: a main menu, settings (graphics, audio, controls), a pause
menu and an inventory with drag and drop and tooltips, in Swift over documents the editor made, and
navigable with a keyboard or a gamepad.

| ![](../design/images/menus-settings-graphics.png) | ![](../design/images/menus-settings-controls.png) |
|---|---|
| ![](../design/images/menus-inventory-tooltip.png) | ![](../design/images/menus-pause.png) |

*Settings at 1280x720; the controls after Jump was rebound to E and the gamepad's Y; the inventory's
tooltip and the pause menu at 1920x1080, rendered at 1286x724 by the Quality upscaler. A
[video](../design/videos/menus-gamepad.mp4) of the whole script.*

## 15. Testing

| Suite | What it holds |
|---|---|
| `unit.ui` | opacity down the tree, content painted in order under its clip, empty containers, the shape fields |
| `unit.ui_text` | the built-in font's glyphs, measurement, glyph placement and uv, the warmed atlas |
| `integration.ui_interface_font` | the interface font: distance-field glyphs on one page and one batch, the built-in line height, size scaling one atlas entry, outline and shadow, gradient and colour spans, glyphs made resident by `set_text`, a colour glyph as an image |
| `unit.ui_console` | a virtualised scrollback, commands, layout and what the console flattens to |
| `unit.ui_render` | the 64-byte row round-trips, the scissor rule, one draw per batch, the host reference, a distance-field glyph and its outline band |
| `unit.render_forward` | the interface stage's producer is handed the chain's last colour |
| `render.ui` | on a device: nothing to draw is byte-identical to the frame before the pass; the device matches the host reference to one step; order, nested clipping and opacity; the HUD and console against a golden image; (h) a paragraph in the interface font is one draw and matches the host to one step; (i) an outlined, shadowed title against a golden image |
| `integration.game_backend_ui` | the ABI 1.6 entries over a real store: ownership, kinds, the progress fill, visibility, clicks, hits, focus, destroy |
| `render.rts_api_hud` | the Swift-built HUD flattens to the C++ HUD's primitives and draws the same bytes on a device; with its Build button against a golden image |
| `unit.ui_toolkit` | every widget's states and events, rich text, text editing (caret, selection, composition, undo, clipboard, masks), focus order by keyboard and gamepad, transitions and animations by time, bindings, documents: parse, write, round-trip, `use`, reload as a diff |
| `unit.ui_input` | the `UI.*` actions and their default bindings, the bridge, the focus layer suppressing gameplay |
| `integration.ui_toolkit` | every container's layout and the gallery's against goldens at one DPI and two; the 10 000-item list at the cost of a hundred and inside a frame budget; a document reloaded from disk keeping its state |
| `render.ui_toolkit` | (a) a started, empty toolkit is byte-identical; (b) an opacity group matches its host reference; (c) the HDR composite blooms; (d) the gallery, closed and with its dialog, against golden images; (e) the HDR stage's place in the frame |
| `integration.game_backend_ui` | also ABI 1.12: widgets, documents by text and by path with reload, events to their owners, navigation, the embedder's widgets out of reach |
| `integration.editor_backend_ui_layout` | the editor's preview service: the catalogue, every widget drawn, a state previewed (hover, pressed, focused, disabled, an open drop-down), a theme switched, a `use` and a sheet the editor sends, scale modes and DPI, a broken document keeping the last good picture, the wire both sides hold byte for byte |
| `integration.menus_sample` | `samples/16-menus` played twice by a scripted gamepad and keyboard: navigation, settings applied to the engine, rebinding and its persistence, the inventory, a hot reload |
| `smoke.menus_frames` | the same session on a device: four frames against goldens, the extents the upscaler drew at, the frame cost |
| `integration.build_content` | the `ui` producer: discovered sheets and used documents, a re-cook on a saved sheet, a missing file and a broken document named |

```sh
just test-suites render:^render.ui$
CY_RENDER_UPDATE_GOLDEN=1 build/dev/cy_test_render_ui   # rewrite references; look before committing
CY_UI_UPDATE_GOLDEN=1 build/dev/cy_test_integration_ui_toolkit   # rewrite the layout goldens
python3 src/ui/render/shaders/regenerate.py --slangc build/dev/Development/bin/slangc
just build-shaders --strict src samples
```

## 16. Not built yet

Issue #91's remaining stages: custom UI materials and blur-behind, world-space and surface-space
documents, the immediate-mode API, the inspector and the conformance suite. The UI layout editor
edits a document's text and previews it; it does not yet edit a theme's rules visually (a theme is
saved as text with `ui.theme.save`), and the canvas's state preview applies to one widget at a time.
