# Design: M5.5 — Operable

## Why this is an insertion rather than a correction to M5

M5 did what its row said. The row was wrong.

It claimed `editor-ui-ux` at Working — docking, workspaces, the command palette, keyboard-first
operation, the generated inspector — and closed on a *scripted* session. `cy-editor-interface`
implemented exactly that: those things as models a test can drive, with no window and no toolkit.
Correct against the capability spec, correct against the artefact, and it leaves nothing to look at.

The rule that catches it is `delivery-roadmap`'s own: a milestone ends in an artefact exercising its
capabilities *through the entry points a user would use*. A script driving command objects is not
that entry point.

So M5 closes on what it achieved, its row is corrected to Seed, and the window becomes its own
milestone. Inserting rather than renumbering keeps every reference to M6–M11 valid, and the
fractional number says plainly that the ladder gained an entry rather than always having had one.

## 1 — The toolkit decision, finally taken

`editor-rust-application` says the interface toolkit is an implementation detail and deliberately
does not name one. That deferral has been free for five milestones. It stops being free here.

**Decision.** The toolkit is chosen in this milestone by spike, against one criterion that dominates
every other consideration: **can the viewport present an engine-rendered image without a round trip
through the CPU?**

The editor renders through the engine — no second renderer, per `editor-viewport-and-gizmos` — so
the frame arrives as a GPU image from the runtime process. A toolkit that can only display a CPU
bitmap turns every frame into a device-to-host-to-device copy, and at that point the viewport's cost
is set by the toolkit rather than by the scene. Everything else — widget richness, styling, ecosystem
— is subordinate, because it can be worked around and this cannot.

The choice is recorded with its reasoning, and the layer above it stays toolkit-agnostic so the
decision remains reversible. That is what "implementation detail" has to mean in practice.

## 2 — The viewport shows the engine's image, or it shows nothing

The temptation, once there is a window, is to draw a quick preview in the toolkit and wire the real
renderer later. That is how an editor acquires a second renderer, and `editor-viewport-and-gizmos`
forbids it for a reason that outlives this milestone: what the editor shows must be what the game
will show, or the editor is lying about the thing it exists to let you judge.

**Decision.** The viewport is the engine's frame from the first pixel. If the transport is not ready,
the viewport shows a message saying so — not an approximation.

## 3 — Agents author, and the loop closes on observation

M5 built the projection surface: a command registry with typed parameters, machine-readable
descriptions and declared effect classes. M4 proved a Swift module reloads with live state. M3
renders. This milestone joins them.

**Decision.** The agent interface reaches **Working**, and the capability it must demonstrate is the
full authoring loop rather than any individual tool:

    compose a scene  ->  write a gameplay script  ->  build and reload  ->  play  ->  LOOK  ->  decide

The last two steps are what make it authoring rather than data entry. An agent that places a light
and cannot see that the scene is now blown out is typing coordinates. The observation step is why
`editor-agent-interface` specifies viewport observation through the engine's own renderer, and why
it is a requirement rather than a convenience.

**Why this moves from M8 to M5.5.** The original placement assumed driving an editor is worth little
until there is a substantial project to drive. That is backwards. The agent loop is *most* valuable
while the engine is being built, because it is the only thing besides a test suite that exercises
the editor, the runtime, the reload path and the renderer together — and every serious defect this
project has found came from something running end to end rather than a model tested in isolation.

## 4 — Writing source is a transaction like any other

An agent creating a `.swift` file in the project is mutating the project, and it gets no special
path. It is a transaction, it carries an actor and an intent, it is refused outside the connection's
scope, and its effect class is declared.

**The awkward case, decided here:** a file write is not undoable by the transaction system alone.
The decision is that source creation and edit are **reversible-mutation** where the editor holds the
prior contents in its journal and can restore them, and **irreversible-mutation** where it cannot —
overwriting a file the editor never read, for instance. The class is computed, not assumed, and the
confirmation rule follows from it. That keeps `editor-agent-interface`'s promise that confirmation
is rare and meaningful rather than a habit.

## 5 — Two artefacts, because there are two audiences

A single artefact cannot demonstrate both a person operating an editor and an agent authoring
through it, and pretending otherwise would repeat M5's mistake in a different shape.

- `samples/05b-editor-window` — a person: open, select, drag a gizmo, undo, save.
- `samples/05b-agent-authoring` — an agent: compose a scene from empty, write and reload a script,
  capture the viewport, confirm.

The second is scriptable and therefore CI-testable; the first needs a display and is honest about
skipping where there is none, in the manner `tests/render/` already established.

## 5b — The visual references are binding, and this is the milestone that uses them

`docs/design/` has held a specification and two reference images since M3, waiting for the milestone
that would draw something. This is it, and the imagery is normative rather than inspirational —
`editor-visual-language` says so in as many words.

**Read before writing any interface code**: `docs/design/editor-visual-language.md`, and look at
`docs/design/images/editor-rts-desertfrontier.png` and `editor-adventure-ancientfrontier.png`. The
document reads both images region by region; the specification states the rules they embody.

The constraints that will be got wrong if nobody looks:

| | |
|---|---|
| **Viewport first** | The scene carries the screen's colour and luminance range. Every panel is charcoal. This is the single easiest property to erode, one accent at a time. |
| **Selection** | A thin gold outline that does not glow and does not obscure the material beneath — you must be able to judge a surface while it is selected. |
| **Axis language** | X red, Y green, Z blue in the gizmo, in the inspector's vector fields, in the orientation widget, in debug views. Not user-remappable. |
| **Gizmos by shape** | Arrows translate, arcs rotate, boxes scale. The active mode is identifiable from the gizmo with the toolbar cropped out of the picture. |
| **The orientation widget is not a manipulator** | Three axes, no rings, no boxes, visually quieter than the transform gizmo. |
| **Chrome is overlay** | Projection, rendering mode and show flags float *in* the viewport. No second full-width toolbar. Every pixel not spent on chrome is viewport. |
| **Surfaces** | Small luminance steps, spacing and subtle separators. No cards, no gradients, no heavy shadows. |
| **Density** | More compact than consumer software, less cramped than legacy engineering tools. |
| **Vocabulary** | Node and entity, not Actor. Graph, not Blueprint. Content browser, not Content Drawer. |

**Four things in the references must NOT be copied**, and `docs/design/editor-visual-language.md`
records why: the mockups use Unreal's product vocabulary throughout; the panel labelled "World
Partition" in the adventure image contains a profiler; the two images brand the header differently
and `cyberdyne-mark.png` is the normative one; and the universal gizmo shown on a slender column sits
at the edge of readability, which is the case the "readable or it degrades" rule was written for.

**A reference states visual language, not feature completeness.** Both images depict volumetric
clouds, procedural forests and thousands of instanced units — none of which exists before M7–M10.
Every constraint above is implementable now against whatever the renderer can produce, including a
grey box on a flat plane. The chrome, the colour, the density and the vocabulary do not depend on
what is in the scene.

## 6 — What M5.5 deliberately does not do

- **No editor feature completeness.** Docking works; every panel a shipping editor eventually has
  does not exist. `editor-ui-ux` reaches Working, not Complete.
- **No agent autonomy beyond the loop.** The interface is capable; nothing schedules or supervises
  an agent, and nothing here makes one act unprompted.
- **No collaborative editing.** Multiple agents or users on one document is out of scope and the
  transaction model does not yet address it.
- **No visual scripting.** Graphs are M8. An agent writes Swift, which is what M4 delivered.
