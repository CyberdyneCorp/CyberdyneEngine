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

## 1 — The toolkit decision, taken and measured

`editor-rust-application` says the interface toolkit is an implementation detail and deliberately
does not name one. That deferral was free for five milestones. The spike took the decision.

**Chosen: egui 0.36.1 + egui_dock 0.21.1 over wgpu 30.0.1.** Runner-up: Dear ImGui via
`dear-imgui-rs`. Third and closer than expected: GTK 4.

### The dominant criterion did not decide it

All three present an engine-rendered GPU image with **no CPU copy**, proven by running rather than
read: a separate process allocates a 1920×1080 `VkImage` with DRM-format-modifier tiling, exports
the memory as a dma-buf fd, passes it over a unix socket with `SCM_RIGHTS`, and the editor composites
it into a docked panel. Presenting it costs **below the measurement floor — 0.44 ms with the image
and 0.44 ms without, and the same at 4K.**

The counterfactual is what the deferral has been costing us in the abstract:

| leg, 1080p | quiet | under load |
|---|---|---|
| runtime device→host | 0.86 ms | 3.28 ms |
| IPC (shared mapping) | 0.32 ms | 2.30 ms |
| editor upload | 0.78 ms | 1.17 ms |
| **total overhead** | **≈ 2.0 ms/frame** | ≈ 6.8 ms |

That is 2.75× the editor's own frame cost, plus a frame of latency — and it scales with pixels:
**≈ 8.9 ms at 4K, more than half a 60 Hz budget before anything is drawn.**

### What actually decided it

**Byte-exactness.** The runtime clears to `(200, 100, 50)`. egui and GTK present exactly that.
`dear-imgui-wgpu` applies a gamma curve to the whole draw list including the imported frame, giving
`(201, 100, 46)` in its default mode and `(229, 168, 122)` in the other — **neither supplied mode
passes the image through unchanged.** Task 5.2 asks us to prove the viewport image is the engine's
by comparing against a direct render; with egui that comparison is `==`, with Dear ImGui it is a
tolerance, and a tolerance hides exactly the regressions the comparison exists to catch. The error is
worst in dark regions, which is the worst possible shape for a tool used to judge lighting.

**Accessibility.** eframe ships AccessKit on by default with AT-SPI, UIA and NSAccessibility
backends. **The Dear ImGui ecosystem exposes nothing to any platform accessibility tree** —
structurally, because it draws its own pixels. `editor-ui-ux` requires accessibility hooks.

### What we give up, named

Immediate mode, so a 30,000-row outliner needs deliberate virtualisation (`virtualise.rs` already
exists and is the right shape). Docking is a third-party crate on its own cadence. **No complex-script
text shaping** — egui has no HarfBuzz, so RTL and Indic scripts are unhandled; a real ceiling we
cannot lift ourselves. And Dear ImGui's docking is genuinely better: its `WindowKey` separates stable
identity from display title, which is exactly what a persisted workspace needs across a renamed or
localised panel — **we should steal that idea for our own `PanelId` ↔ title mapping.**

Losing ImGuizmo is close to a non-loss: `editor-viewport-and-gizmos` puts gizmo geometry, depth
handling and drawing in the engine, so it would have been the wrong tool anyway.

### Cross-process synchronisation — solved, and measured

The toolkit spike left this as the largest open item. A second spike answered it, on this hardware,
by building and running.

**Timeline semaphores work across processes, without forking wgpu.** The escape hatch is smaller
than `WgpuSetup::Existing` implies: `wgpu_hal::vulkan::Adapter::open_with_callback` hands you the
extension list before `vkCreateDevice`, so pushing `VK_KHR_external_semaphore_fd` and passing the
result to `create_device_from_hal` is about thirty lines. `VK_KHR_timeline_semaphore` needs no work —
wgpu enables it unconditionally at API ≥ 1.2, and its own `Fence` *is* a timeline semaphore.
Timelines export and import over `OPAQUE_FD`; `SYNC_FD` is binary-only and must not be planned on.

**It is worth the work.** The fence wait the first spike fell back to costs 2.69× on a cheap frame.
With an editor attached: fence and one image gives 1,287 runtime fps, 0.85 ms latency and **99.9%
corruption**; timelines with three images give **1,727 fps, 0.42 ms, and 0%**. There is no trade-off
to argue about.

**Three images minimum, four preferred.** One wedges. Two throttle the runtime to the editor's
refresh rate and cost a whole editor frame — 16 ms — of latency. Three is where pipelining starts;
four removes the last stalls for 8.4 MB. And the editor must never throttle the runtime, so the
runtime drops on a full ring rather than blocking.

### The failure mode that decides the wait policy

`add_wait_semaphore` on the editor's wgpu queue is **one bad value away from an editor that renders
nothing and cannot be closed**. Measured: an unsatisfiable wait returns from `submit` in 0.19 ms
because the wait is on the GPU, then every later independent submission times out, and shutdown
hangs forever in `vkDeviceWaitIdle`. That is a direct violation of `editor-rust-application`'s
requirement that a runtime failure not terminate the editor.

**So the wait is a bounded host wait, not a GPU wait.** `vkWaitSemaphores` with a 2 ms timeout shows
97% of the newest frames at the same latency as the GPU wait's 100%, and can never leave an
unsatisfiable wait in a queue. Trading three frames in 360 for an editor that cannot be wedged is
not a close call.

Process death, separately, is survivable — six SIGKILL runs, all six survived, the editor freezing on
the last complete frame with a banner. But it is survivable **only because the protocol announces
after `vkQueueSubmit`**, so every value the editor can wait on is already submitted and will
eventually signal. That invariant was not written down anywhere; announce-before-submit, or a runtime
whose own GPU work hangs, lands in the unrecoverable case. It is written down now.

### One thing that passed by luck, and is worth remembering

The spike's first capability check asked whether ash's `import_semaphore_fd_khr` function pointer was
non-null. It returned true **without the extension enabled**, because ash installs a panicking stub
rather than a null pointer — and calling it aborts the process rather than returning an error. The
only honest check is whether the extension was enabled.

### Reversibility, checked rather than assumed

None of M5's fourteen crates names a toolkit. `docking.rs` owns `Layout` as the editor's own tree —
its header already says *"a dock manager renders a Layout; it does not decide one"* — and
`cy-editor-visual` owns colour, density and the axis language. Swapping the toolkit means changing
one render crate plus four adapters.

**That boundary is not self-enforcing.** Without a test, `egui::Color32` will appear in
`cy-editor-visual` within a milestone, because it is locally reasonable every single time. Hence the
containment test in task 1.0 below.

Also required: **the MSRV moves to 1.95.** egui 0.36 refuses to build on 1.92, which is this
machine's `stable` and what `editor/Cargo.toml` currently declares.

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

## 5c — The transform gizmo, and one thing in its reference to resolve

`docs/design/images/transform-gizmo.png` is now normative and it settles a great deal that the
capability spec left open: four modes on `W`/`E`/`R`/`T`, planar handles at the axis pairs, a centre
that carries three separate affordances, world and local space, four pivot modes, snapping with
`Ctrl` temporary and `Shift` precision and `Alt` duplicate-and-transform, per-axis numeric entry, and
constant screen size regardless of camera distance.

Two properties in it are worth calling out as load-bearing rather than decorative:

**Constant screen size** is what makes a scene at real scale editable at all. Drawn in world units, a
gizmo on a distant object becomes unusable exactly when precision matters most, and the failure is
gradual enough that nobody files it as a bug.

**Hover emphasis before the press.** The reference shows normal, hover and active as three distinct
states. Without the middle one a user learns which axis they grabbed by dragging the wrong one, and
then undoing — a small cost paid on every single manipulation.

### The one conflict, and how it resolves

The reference tints the **active** state red. `editor-visual-language`'s semantic palette assigns red
to **error and destructive consequence**, and the axis language assigns red to **X**. Three meanings
on one hue, on the same object, at the moment of manipulation.

**Decision.** The active state is expressed by **luminance and saturation lift on the handle already
being dragged**, not by recolouring it. A dragged X arrow gets brighter and more saturated red; a
dragged Y arrow gets brighter green. Hover uses the same mechanism at lower intensity, which is what
the reference's gold hover is approximating.

That keeps the axis language intact — the whole point of X red, Y green, Z blue is that the hue
identifies the axis and nothing else — and it keeps red meaning error everywhere in the product. The
reference is right about *that there are three states* and wrong about *how the third is encoded*;
`editor-visual-language` requires a reference that no longer reflects the intended language to be
corrected, so this one gets a note beside it rather than being followed literally.

## 6 — What M5.5 deliberately does not do

- **No editor feature completeness.** Docking works; every panel a shipping editor eventually has
  does not exist. `editor-ui-ux` reaches Working, not Complete.
- **No agent autonomy beyond the loop.** The interface is capable; nothing schedules or supervises
  an agent, and nothing here makes one act unprompted.
- **No collaborative editing.** Multiple agents or users on one document is out of scope and the
  transaction model does not yet address it.
- **No visual scripting.** Graphs are M8. An agent writes Swift, which is what M4 delivered.
