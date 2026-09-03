# editor-viewport-and-gizmos Specification

## Purpose

Defines the viewport and the **division of rendering responsibility** between editor and engine:

> The editor decides *what should be shown*. The renderer decides *how it is drawn*.

The editor owns camera, selection, filters, gizmo intent and visualisation requests. The engine owns
the render graph, culling, LOD, materials, lighting, gizmo geometry and drawing. There is no second
renderer, so what the editor shows is what the game will show.

The image arrives through an abstract **transport** — local surface, shared texture, or encoded
stream — so editing on a console uses the same code as editing locally. Picking is engine-side, so
what is picked matches what was rendered, including virtualised geometry and foliage. Manipulation
works from the state captured at drag start, so a round trip returns the exact original value.

## Requirements

### Requirement: The rendering responsibility split
The division of responsibility between editor and engine SHALL be:

> **The editor decides what should be shown. The renderer decides how it is drawn.**

Accordingly:

| Responsibility | Owner |
|---|---|
| Camera pose, projection, and view state | Editor |
| Selection, visibility filters, isolation, view modes | Editor |
| Gizmo intent, manipulation state, snapping rules | Editor |
| Debug visualisation requests and their parameters | Editor |
| View mode requests (wireframe, unlit, overdraw, lightmap density, GI probes, virtual texture feedback) | Editor |
| Render graph construction and execution | Engine |
| Culling, LOD selection, virtual geometry, streaming, residency | Engine |
| Material evaluation, shading, lighting, global illumination | Engine |
| Post-processing, temporal accumulation, upscaling | Engine |
| Gizmo geometry generation, depth handling, and drawing | Engine |
| Selection outlines, highlight rendering, and overlays | Engine |

The editor SHALL NOT construct render passes, allocate render targets, bind pipelines, record command
buffers, or issue draw calls.

**There SHALL NOT be a second renderer.** Editor visualisation SHALL be implemented as engine
capabilities driven by editor requests, so that the editor sees the same image the game will see.

#### Scenario: A debug view is an engine feature
- **WHEN** the editor offers an overdraw view
- **THEN** it SHALL be an engine debug view requested by the editor, not editor-side drawing

#### Scenario: What you see is what ships
- **WHEN** a scene is viewed in the editor viewport at shipping settings
- **THEN** the image SHALL be produced by the same render graph the game uses

### Requirement: Viewport transport
The viewport SHALL obtain its image through an **abstract transport** with at least three
implementations:

| Transport | Used when | Characteristics |
|---|---|---|
| Local surface | Embedded runtime | Direct presentation into an editor-owned surface |
| Shared texture | Hosted runtime on the same machine | Zero-copy or near-zero-copy sharing between processes |
| Encoded stream | Remote device or console | Compressed video with input forwarded back |

The editor SHALL treat all three uniformly: a viewport feature SHALL work over any transport unless
it documents a reason not to.

The transport SHALL carry, alongside the image, the **frame's view state and identifiers**, so that
picking, gizmo interaction, and overlay alignment are correct for the frame actually presented rather
than for the editor's current state.

Latency and frame pacing SHALL be reported per transport, and the editor SHALL surface when it is
viewing a stale or degraded stream.

#### Scenario: Console editing is not special
- **WHEN** the runtime is a console
- **THEN** viewport navigation, selection, and gizmos SHALL work through the encoded stream

#### Scenario: Interaction matches the presented frame
- **WHEN** the user clicks in a streamed viewport
- **THEN** the hit SHALL be resolved against the view state of the frame shown, not a newer one

### Requirement: Multiple viewports and view states
The editor SHALL support **multiple simultaneous viewports** — perspective and orthographic, split
views, and secondary cameras — each with its own view state, view mode, and visualisation settings.

Each viewport SHALL declare its cost, and the editor SHALL be able to limit rendering of unfocused or
hidden viewports rather than paying for all of them continuously.

A viewport SHALL be able to render **through a game camera** so that composition can be judged with
the shipping camera's settings.

#### Scenario: Hidden viewports do not cost
- **WHEN** a viewport is not visible
- **THEN** it SHALL not be rendered

### Requirement: Navigation
The viewport SHALL provide standard navigation — orbit, pan, zoom, fly, focus on selection, frame
all, and view-axis snapping — with speed adaptive to the distance to the point of interest.

Navigation SHALL be **bindable and preset-selectable**, including presets matching existing engines.

Navigation SHALL remain smooth and predictable while the world is streaming, and SHALL not be blocked
by loading.

Camera state SHALL be per viewport, persisted per document, and restorable.

#### Scenario: Navigation is not blocked by streaming
- **WHEN** the user flies into unloaded regions
- **THEN** navigation SHALL remain responsive while content loads progressively

### Requirement: Selection and picking
Picking SHALL be **engine-side**, so that what is picked matches what is rendered — including
virtual geometry, instanced content, foliage, terrain, and skinned meshes.

Picking SHALL support click selection, rectangle and lasso selection, cycling through overlapping
candidates, selecting through transparent surfaces by intent, and selecting the prefab root or the
inner instance explicitly.

Selection SHALL be expressed in **stable identity** (see `world-partition-and-streaming` persistent
identifiers), SHALL survive streaming and reload, and SHALL be shared through the editor's selection
service.

Selection SHALL be filterable by type, layer, tag, and locked state, and locked or hidden objects
SHALL not be picked unless explicitly requested.

#### Scenario: Virtual geometry is pickable
- **WHEN** the user clicks a virtualised mesh
- **THEN** the correct instance SHALL be selected

#### Scenario: Selection survives a reload
- **WHEN** a region streams out and back in
- **THEN** the selection SHALL be preserved by identity

### Requirement: Gizmos and manipulation
The editor SHALL provide translate, rotate, and scale gizmos in world, local, parent, view, and
custom spaces, with a configurable pivot — origin, centre, or active object.

Gizmo geometry, depth behaviour, occlusion handling, and screen-constant sizing SHALL be produced by
the engine; the editor SHALL supply intent and manipulation state.

Manipulation SHALL be **numerically stable**: it SHALL operate on the state captured at drag start
rather than accumulating per-frame deltas, so that a drag returning to its origin returns the exact
original values.

A manipulation SHALL produce exactly one transaction, SHALL be cancellable, and SHALL show numeric
feedback of the delta and the resulting value.

Custom gizmos SHALL be registrable by plugins through editor abstractions and rendered by the engine
through the same path.

#### Scenario: A round trip is exact
- **WHEN** a user drags an object and returns to the starting position
- **THEN** the transform SHALL equal its original value exactly

#### Scenario: Plugin gizmos are first class
- **WHEN** a plugin registers a gizmo
- **THEN** it SHALL render, pick, and undo identically to built-in gizmos

### Requirement: Snapping and precision
The editor SHALL support grid snapping, angle snapping, scale snapping, vertex and surface snapping,
pivot snapping, and alignment to another object's transform.

Snapping increments SHALL be configurable, unit-aware, and toggleable transiently with a modifier.

Numeric entry SHALL be available for every manipulation, and SHALL accept expressions and units.

Precision SHALL be maintained at large world coordinates, consistent with the engine's camera-relative
and large-world conventions.

#### Scenario: Precision holds far from the origin
- **WHEN** an object is manipulated far from the world origin
- **THEN** manipulation SHALL remain precise and free of visible jitter

### Requirement: View modes and debug visualisation
The editor SHALL expose the engine's debug views — including wireframe, unlit, shading complexity,
overdraw, LOD and virtual geometry cluster views, lightmap and GI probe visualisation, virtual texture
feedback and residency, shadow cascade and virtual shadow page views, physics colliders, navigation
data, audio emitters, streaming region state, and light complexity — as **selectable view modes**.

Every debug view SHALL state what it shows and how to read it, and SHALL be reachable from the
command palette.

Debug views SHALL be requestable per viewport, and SHALL be composable with visibility filters and
isolation.

Where a subsystem specification requires a causal explanation — why an object is not resident, why a
region is dark, why an LOD was chosen — the viewport SHALL be an entry point to that explanation.

#### Scenario: Why is this dark
- **WHEN** the user asks why a surface is unlit
- **THEN** the viewport SHALL lead to the GI explanation defined by the illumination capability

#### Scenario: Debug views are discoverable
- **WHEN** a user searches for a visualisation
- **THEN** it SHALL be findable in the command palette with a description

### Requirement: Overlays and in-viewport interfaces
The viewport SHALL support overlays — measurement, statistics, safe frames, composition guides,
camera information, coordinate readouts, and object labels — which SHALL be individually toggleable
and SHALL not be baked into the rendered image used for judging appearance.

Overlays SHALL be excluded from any capture intended to represent the shipping image, and a capture
SHALL state whether overlays were included.

In-viewport controls SHALL not obstruct the content being judged, and SHALL be dismissible.

Viewport **controls** — projection and rendering mode, show flags, camera speed, snapping,
transform mode, maximise, and debug visualisation selection — SHALL likewise be presented as
overlays within the viewport rather than as an additional toolbar occupying vertical space above
it. Their visual treatment is specified in `editor-visual-language`.

The **view-orientation widget** is an overlay, not a manipulator: it presents camera and world
orientation, SHALL show only the three principal axes, and SHALL NOT carry rotation rings, scale
handles, or translation arrows that would make it read as a transform gizmo.

#### Scenario: Controls do not consume viewport height
- **WHEN** viewport controls are added or extended
- **THEN** they SHALL be placed as overlays, and the rendered viewport area SHALL NOT shrink

#### Scenario: Orientation is not manipulation
- **WHEN** a user drags the view-orientation widget
- **THEN** the camera SHALL orient, and no object transform SHALL change

#### Scenario: A capture is clean
- **WHEN** the user captures a viewport image for reference
- **THEN** overlays SHALL be excluded unless explicitly requested

### Requirement: Editing while playing
When the runtime is in a play mode (see `live-editing`), the viewport SHALL make the distinction
between **edit state and play state visually unmistakable**, and SHALL state what will persist when
play ends.

Manipulation during play SHALL follow the live-editing rules for propagation and persistence rather
than being silently discarded or silently persisted.

The editor SHALL support detaching a viewport camera from the game camera during play to inspect the
running world without altering it.

#### Scenario: Play state is unmistakable
- **WHEN** the runtime is playing
- **THEN** the viewport SHALL indicate it clearly, and the editor SHALL state which edits persist

#### Scenario: Inspection without interference
- **WHEN** the viewport camera is detached during play
- **THEN** the game camera and gameplay SHALL be unaffected

### Requirement: Viewport performance and degradation
The viewport SHALL declare and honour a rendering budget, and SHALL degrade **visibly and
explicitly** rather than stalling: reducing rate for unfocused viewports, lowering resolution, or
pausing background viewports.

Degradation SHALL be surfaced to the user, so that a lower-quality image is never mistaken for the
project's appearance.

The editor SHALL never block its interface thread on runtime rendering; a stalled runtime SHALL
produce a stale-frame indication rather than a frozen editor.

#### Scenario: A stalled runtime does not freeze the editor
- **WHEN** the runtime stops producing frames
- **THEN** the viewport SHALL indicate staleness and the editor SHALL remain interactive

#### Scenario: Degradation is honest
- **WHEN** the viewport reduces quality to stay within budget
- **THEN** it SHALL indicate that the image is not at full quality

### Requirement: Viewport determinism and reproduction
A viewport view state SHALL be **capturable and restorable**, including camera pose, projection,
view mode, visibility filters, and time state, so that an observation can be reproduced exactly.

A captured view state SHALL be attachable to a defect report and to the diagnostics trace, so that
the frame that showed a problem can be revisited.

#### Scenario: A visual defect is reproducible
- **WHEN** a user reports a rendering problem from the viewport
- **THEN** the captured view state SHALL restore the same view

### Requirement: Forbidden viewport patterns
The following SHALL NOT appear, and each SHALL be checkable:

- Editor code constructing render passes, allocating render targets, or issuing draw calls
- A second renderer or a separate shading path used only by the editor
- Editor-side picking that does not match what the engine rendered
- Gizmo manipulation accumulating per-frame deltas rather than operating from drag-start state
- A viewport feature that works only with an in-process runtime without a documented reason
- Overlays included in an image presented as representative of the shipping appearance
- Blocking the editor's interface thread on runtime frame production
- Selection stored as pointers or indices that do not survive streaming

#### Scenario: A proposal is checked
- **WHEN** a feature proposes drawing selection outlines in editor code
- **THEN** it SHALL be flagged against this requirement
