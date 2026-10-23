# Design: the widget toolkit

## 1. A widget is elements and a record

A widget is the elements the store already lays out, styles and flattens — a slider is a track, a
fill and a thumb — plus a record in the toolkit saying what each element is (its "part") and which
widget owns it. Nothing downstream learns a new path: `layout()`, the cascade, `flatten()` and the
renderer see elements. The widget's handle is its root `ElementId`, generational like every other.

The alternative — widgets as a draw list of their own — would have duplicated clipping, batching and
the budget, and made a HUD of elements and a menu of widgets two interfaces.

## 2. A part is styled with its widget's states

`.cyss` matches an element by its type, id, classes and states. A part's states are its widget's, so
`button:hover` reaches the button's background and `button:hover button-label` its caption, and one
rule styles a part wherever it is. A part's base layout (a thumb's size, a track's padding) is the
toolkit's; the theme is ordinary `.cyss` (`default_theme_source()`), so a game restyles any widget
without code.

## 3. Animation is paint-only, and the toolkit's own

An animation or a transition drives an element's paint channels — opacity, the render transform,
the background — through overlays the toolkit applies after the cascade, so a style change and an
animation of the same property compose rather than fight, and no frame of an animation lays
anything out. Time is the `seconds` handed to `update`; a module starts an animation through one
ABI call and nothing crosses the boundary per frame. Reduced motion ends a decorative animation on
its first frame; state feedback (a press) is not decorative.

## 4. Documents reload as a diff

`DocumentInstance::reload` expands the new text and matches each node to the widget it made — by id,
or by kind and position among unmatched siblings — keeping the widget, its focus and the player's
value; a node whose creation-only property changed is made again. A save that does not parse keeps
the interface and reports its diagnostics. `DocumentHost` uses the VFS's settling `FileWatcher`, so a
half-written save is not read.

## 5. The interface consumes input through the input system

The toolkit takes `UiAction`s, never device events. The bridge raises the input user's focus layer
to `Interface` while a dialog, a drop-down or a focused text field holds the input; the input
server then drops bindings of the `Gameplay` layer, so "UI consumes before gameplay" is the input
system's resolution, not a flag a game checks.

## 6. Opacity groups and HDR are passes, not shader tricks

A group's subtree is flattened after the main stream; the renderer draws it into a cleared
transient target and the main pass samples it once at the group's opacity. A frame with no group
declares nothing new, which keeps every existing frame byte-identical. The HDR composite is a second
renderer at a new stage before bloom, decoding to linear light at the frame's exposure; the existing
display-referred pass is unchanged.

## 7. The ABI appends, and the toolkit is attached

ABI 1.12 appends entries and `UiBackend` virtuals with UNAVAILABLE defaults, so an embedder with no
toolkit is unchanged. Ownership is by record, as 1.6's is: a module reaches the widgets it made and
those of its documents, never the embedder's.
