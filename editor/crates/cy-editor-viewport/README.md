# `cy-editor-viewport` — layer 2

The viewport: **what the editor decides should be shown, and how it asks for it.** Tasks 4.1–4.5 of
M5, governed by `openspec/specs/editor-viewport-and-gizmos/spec.md`.

## The one rule, and what it means for what is in here

> The editor decides *what should be shown*. The renderer decides *how it is drawn*.

Read as an instruction about what may exist in this crate, that sentence produces the module list.
There is no render pass here, no render target, no pipeline, no command buffer and no draw call — and
no dependency that could supply one. The crate depends on `cy-editor-core`, `cy-editor-documents` and
`cy-editor-protocol`, and on nothing else in the world.

| Module | Task | What it owns |
|---|---|---|
| `math` | — | The geometry manipulation intent needs, and nothing more |
| `state` | 4.1, 4.5 | A view state: pose, projection, view mode, filters, time — capturable and restorable |
| `transport` | 4.1 | How the image arrives, how old it is, and what it cost |
| `navigation` | 4.1 | Orbit, pan, zoom, fly, focus, axis snap; bindings and four presets |
| `picking` | 4.2 | Pick requests against a presented frame, and the selection they produce |
| `gizmo` | 4.3 | Manipulation from captured drag-start state, as exactly one transaction |
| `snapping` | 4.3 | Grid, angle and scale snapping; numeric entry with units and expressions |
| `reconcile` | 4.3 | Local prediction and the runtime's authoritative echo, keyed by frame |
| `viewmode` | 4.4 | The engine's debug views, described well enough for a command palette |
| `overlay` | 4.4 | Overlays, the orientation widget, and what a capture contains |
| `play` | 4.4 | Editing while playing: what is unmistakable and what persists |
| `budget` | 4.5 | Cadence, degradation and what the user is told about it |
| `viewport` | 4.1, 4.5 | A viewport, and several of them with one focused |

The engine's half is `src/servers/render/picking.h` and `src/servers/render/viewport_transport.h`,
which are layer 2 there for the same reason this is layer 2 here: no device, no graph, no pass.

## Task 4.1's measurement, which was the milestone's open risk

The control-path spike closed by naming what it had not covered:

> **NOT MEASURED.** The viewport transport. If the shared-texture or encoded-stream path is two or
> three frames behind, a drag will feel laggy however fast the transform applies. That belongs to
> task 4.1 and should be measured before four panels are built on it.

`tests/frame_age.rs` measures it, across a **real process boundary** — `cy-viewport-probe` produces
frames at 60 Hz over a pipe and the test consumes them at a slower rate with a 40 ms stall every
twelfth iteration, which is what an editor is. Ages are measured against the wall clock, which is the
one clock two processes share.

i9-12900K, Linux, `cargo test -p cy-editor-viewport --test frame_age -- --nocapture`. All figures are
the age of the frame **at the moment it was consumed**, in milliseconds. The queue-versus-mailbox
table below is from the dev profile; it is within noise of the shipping profile's (queue p50 525.7,
mailbox p50 8.5), because what it measures is scheduling rather than work.

### The finding that decided the design

| case | discipline | n | p50 | p99 | max | dropped |
|---|---|---:|---:|---:|---:|---:|
| 60 Hz → 40 Hz, no payload | **queue** | 90 | **523.5** | **1045.3** | 1053.7 | 0 |
| 60 Hz → 40 Hz, no payload | **mailbox** | 53 | **8.4** | **16.6** | 16.8 | 37 |

**A queue between a producer and a slower consumer accumulates without bound and never recovers.**
Sixty-two times worse at p50, and still climbing when the run ended — the last frame consumed was
1.05 seconds old. Every individual measurement inside that run looks healthy: no frame was lost, the
throughput was exactly what was produced, and the queue never blocked. The failure is only visible if
you measure *the age of what you are looking at*, which is the number a designer feels and the only
one that matters.

A single-slot mailbox stays at about one producer interval, and says how many frames it dropped.
`transport::Mailbox` is that decision, and there is no queue anywhere in this crate.

### What the payload costs

Measured in two Cargo profiles, because this is the one number that moves between them — the copy
and the encode are real work, and an unoptimised build is doing them at `opt-level = 0`.

| case | dev p50 | dev p99 | shipping p50 | shipping p99 | moved |
|---|---:|---:|---:|---:|---:|
| shared texture (no copy) | 8.9 | 16.4 | 9.0 | 16.3 | 0 MB |
| encoded stream (64 KB/frame) | 9.8 | 17.2 | 9.5 | 16.8 | 5.6 MB |
| uncompressed 1080p (8.3 MB/frame) | **105.9** | **143.7** | **27.9** | **41.8** | 356 MB |

A **compressed** frame costs about half a millisecond more than a zero-copy shared texture, in either
profile — genuinely second order, smaller than one stalled interface frame, and the same shape of
distribution.

An **uncompressed** one is a different thing: 8.3 MB at 60 Hz is half a gigabyte a second across the
boundary, and it is the only row where the profile matters. At `opt-level = 0` it is twelve times the
shared-texture case; optimised, three times, and its p99 is still 42 ms — two and a half frames.
Neither number is one to build a viewport on, and the honest reading is that this row measures a copy
rather than a transport.

So the third transport is an *encoded* stream, in this crate's vocabulary and in the engine's, and
shipping raw pixels across the boundary is not a transport anyone should build. That is a decision
this measurement settles rather than a preference — and it is the same decision in both profiles,
which is what makes it a decision rather than an artefact of how the measurement was built.

### What is still not measured

* **A real shared texture.** `TransportKind::SharedTexture` moves no bytes here because a real one
  moves no bytes either, but the platform handle exchange, the fence and the layout transition are
  not modelled — they need a device, and this crate cannot have one. What is measured is the
  scheduling, which is what the queue finding is about.
* **A real encoder.** The 64 KB case is a payload of that size, not a codec. An encoder's own latency
  is additive on top of these numbers and belongs with the transport implementation.
* **A remote link.** The control-path spike measured propagation, jitter and loss for 40-byte
  messages; nothing here does it for images. A metro link's 25 ms is additive on these numbers and
  the queue finding gets *worse* with distance, not better.
* Linux only. Windows and macOS are unverified.

## Which the drag loop does: predict and reconcile

Stated plainly because the milestone asked for it to be: **this crate predicts locally and reconciles
against the runtime's echo. It does not do a round trip per frame.**

A drag writes to the document, which is in the editor's process, so the gizmo is drawn from a value
that is already correct with no wait. The same operations go to the runtime asynchronously. The echo
arrives later carrying the frame identifier the request named — the viewport transport's
`FrameId`, not a second one invented for it — and `reconcile::Reconciler` matches them up, reporting
a `Divergence` when the runtime is entitled to disagree (a constraint clamped the value, a script
moved the object, another editor did).

## Three decisions worth knowing before changing anything

**Picking sends a pixel, never a ray.** A `PickRequest` carries the pixel and the identifier of the
frame it was clicked on; the runtime builds the ray from *that frame's* view state and resolves it
against the draw list that frame produced. The editor has a camera and could build a ray in three
lines — which is exactly how an editor acquires the forbidden "editor-side picking that does not match
what the engine rendered", because once the ray exists, intersecting it is three more lines.

**A manipulation is computed from captured state, every frame, from scratch.** `gizmo::Drag` copies
every selected object's transform at drag start and never reads them again. The accumulating version
is the obvious one and it is wrong in a way that hides: a drag out and back leaves the object a few
ten-thousandths from where it started, and after a week of nudging, the grid is not a grid.
`a_drag_out_and_back_restores_the_original_bits` compares `f32::to_bits`.

**The view state an interaction resolves against is the presented frame's, not the editor's.**
`Viewport::interaction_view` is one function so there is one place to get it right. On a remote
transport the editor's camera and the frame on screen are tens of milliseconds apart, and a drag
computed against the newer camera moves the object by the camera's motion as well as the cursor's —
a gizmo that "slides" while the user orbits, and nobody can say why.

## What is deliberately elsewhere

* **The selection.** A service, per `editor-documents-and-transactions`. A viewport holding its own
  copy would be the second source of truth that requirement exists to prevent.
* **Command registration.** `cy-editor-commands` is at layer 2, the same layer as this crate, so
  nothing here can name the registry. `viewmode::view_mode_commands()` returns palette-ready
  descriptors for all nineteen debug views; registering them is three lines in
  `cy_editor_services::builtin`.
* **The pick's wire message.** `PickRequest`/`PickResponse` carry their own committed encoding over
  `cy_editor_core::codec`. They are not variants of `cy_editor_protocol::Message` because that enum
  belongs to another crate; folding them in is one variant each when somebody owns both.
* **How any of it looks.** `editor-visual-language` owns the palette, the axis colours and gizmo
  legibility; the engine owns the geometry, the depth behaviour and the screen-constant sizing.
