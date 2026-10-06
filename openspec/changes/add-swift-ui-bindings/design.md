# Design

## Context

CyberUI (`src/ui/`) is a retained store with three dirty states, layout over flex, grid and
absolute models, an `Interaction` with hit testing, capture, focus and a layer stack, and
`flatten()`; `cy::ui-text` measures and paints a label's text; `cy::ui-render` draws the stream.
`samples/13-rts-selection/hud.cpp` is a HUD written against the store directly. The ABI reaches a
subsystem only through an abstract backend bound on `CyEngine_T::game` (`cy/abi/game/services.h`),
so `cy_abi` gains no dependency; a behaviour reaches Swift only through `CyBehaviourVTable`.

## Decisions

### D1. One appended block, ABI 1.6

Fourteen entries after `character_state` and one vtable member after `exit_tree`. `CyUiElementDesc`,
`CyUiLayout`, `CyUiStyle` and `CyUiEvent` carry `struct_size`; a module compiled against a shorter
struct reads as zeros past its end, which every 1.6 struct defines as the default. `CyUiElement` is
`cy::ui::ElementId` flat — the generation high, the index low — so zero is null and a destroyed
element's handle is refused rather than aliasing the slot's next occupant.

`CyUiEvent` and `CyUiElement` are declared above the behaviour vtable, because `ui_event` takes one;
everything else is in the 1.6 block before the table.

### D2. Zero means the default, so a zeroed layout is the engine's

`cy::ui::LayoutInput`'s defaults are not all zero (a preferred size of -1 asks the content, the
maximum is 1e9, shrink is 1, spans are 1, align is Stretch). The C struct is defined so that zero
is each default: a zero preferred axis is the content's, a zero maximum unbounded, a zero shrink 1
(`CY_UI_LAYOUT_NO_SHRINK` asks for 0), a zero span 1, and `CyUiAlign` numbers STRETCH zero. The
adapter converts. *Rejected:* mirroring `LayoutInput` field for field, which would make every
zero-initialised C or Rust caller lay out zero-sized elements. `flex_basis` is not carried: its
default (-1) and a deliberate zero basis cannot both be zero, and nothing yet needs a basis.

### D3. Kinds, not widgets

Five kinds: panel, label, image, progress bar, button. A kind decides which writes an element takes
(text on a label or button, an atlas page on an image, a fraction on a progress bar) and nothing
else; a button is a focusable panel with text whose clicks are routed. A progress bar is a track and
a fill child the adapter owns, anchored over the fraction — the structure of hud.cpp's health bar,
so the two flatten alike. Its layout model is held at ABSOLUTE whatever the module sets, because the
fill is placed by anchors.

### D4. A module touches its own elements and the root

The adapter keeps a record per element a module created (kind, owner, fill), indexed by store
index and checked by generation. Every write and destroy needs a record; the root is a parent only.
The embedder's own elements — the developer console in the same store — answer NOT_FOUND. A destroy
drops the records of the whole subtree and their text.

### D5. Events reach behaviours through the vtable

The adapter routes the pointer through an `Interaction` with one HUD layer over the root (focus not
captured, input not blocked, the root itself transparent to hits). A press and a release of the
left button over the same button element is a CLICK; the release is tested against where the
pointer is (`probe`), not the capture, so dragging off a button cancels. A focus change is BLUR
then FOCUS. The embedder delivers the queued events with `BehaviourRuntime::ui_event`, which calls
`ui_event` on every live instance whose entity is the element's owner, in the frame phase, through
the creating generation's vtable. *Rejected:* a polled event queue in the table — it would make
every game write its own dispatcher and leave "which behaviour" to convention.

CyberdyneKit registers `ui_event` for every class (an action is attached at run time, so no
compile-time answer exists). Its thunk runs the clicked button's action, recorded on the behaviour
by `mountUI`, then `onUIEvent` if the class wrote it; a throw disables the instance like any
callback.

### D6. What each write dirties

The same as hud.cpp marks for the same change: text Measure and Paint, layout Measure and Arrange,
style and opacity Paint, a progress fraction Arrange on the fill, visibility whatever
`ElementStore::set_flags` decides. With the same inputs written, a store built through the ABI and
one built in C++ flatten to the same primitives, which `render.rts_api_hud` checks primitive by
primitive and pixel by pixel.

### D7. Declarative, mounted once, updated by handle

`UIBuilder` describes a tree with a result builder and mounts it once; the `UITree` it returns finds
elements by `.id`. Updates go through handles. *Rejected:* re-describing every frame and diffing on
the Swift side — the store already tracks dirtiness, and a reconciler across the boundary would
repeat `widgets.h` in a second language. The sample's `RtsHud.show` writes only the parts of its
model that changed, so an idle frame writes nothing.

## Phases and determinism

| Entries | Phases | Why |
|---|---|---|
| all `ui_*` | N U | Presentation: nothing in a fixed step may depend on the interface, and a resimulated tick must not build an element twice. A button's click is recorded in `U` and acted on in the next `F`, as a key is. |
| `ui_event` (vtable) | U | Delivered by the embedder before `frame_update`. |

## Risks

- The store is the embedder's, and a module that builds thousands of elements builds them in the
  embedder's budget. The frame budget ladder still applies to the whole store.
- Event delivery is the embedder's job; a host that binds the adapter and never calls
  `BehaviourRuntime::ui_event` drops clicks. The sample's host is the reference.
