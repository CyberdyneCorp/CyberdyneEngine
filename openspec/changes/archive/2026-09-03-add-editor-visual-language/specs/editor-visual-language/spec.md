## ADDED Requirements

### Requirement: The viewport is the subject
The editor's visual system SHALL be built so that the **game content is the most visually prominent
thing on screen**, and the interface around it recedes.

Application chrome SHALL use near-black and charcoal surfaces. Saturated colour SHALL be reserved
for meaning — selection, state, warnings and errors, transform axes, graph semantics, asset type,
active tools, and team or gameplay visualisation — and SHALL NOT be used to decorate panels.

The intended reading order of the screen SHALL be: the scene, then the selection within it, then the
active controls, then important state, then panel content, then secondary metadata.

An interface element that competes with the viewport for attention without carrying meaning SHALL be
treated as a defect.

#### Scenario: The scene is the brightest thing on screen
- **WHEN** a lit scene is open in the editor
- **THEN** the viewport SHALL carry the screen's colour and luminance range, and no panel SHALL
  present a larger area of saturated colour than the content

#### Scenario: Decoration is rejected
- **WHEN** a panel introduces a coloured header, gradient, or accent that encodes nothing
- **THEN** it SHALL be flagged against this requirement

### Requirement: Cyberdyne identity, not an engine emblem
The editor SHALL present the **Cyberdyne visual identity**: the existing corporate mark, placed in
the upper-left application area, adjacent to the application menu.

The mark MAY be cropped or simplified for compact presentation. Its geometry SHALL NOT be
redesigned for the editor, and the editor SHALL NOT introduce a separate engine emblem in the
manner of competing engines.

The mark SHALL identify the product and SHALL NOT dominate the workspace: it occupies the header
only, and no larger surface.

The normative artwork is `docs/design/images/cyberdyne-mark.png`.

#### Scenario: The mark is not redrawn
- **WHEN** a compact header presentation is needed
- **THEN** the mark SHALL be cropped or scaled, and SHALL NOT be substituted with a new symbol

#### Scenario: No second brand
- **WHEN** editor branding is proposed
- **THEN** it SHALL use the Cyberdyne mark, and an engine-specific logo SHALL be rejected

### Requirement: Surface system
Panels SHALL be differentiated by **small luminance steps, spacing, and subtle separators** rather
than by borders, cards, or shadows.

The system SHALL NOT use large gradients, glass or blur effects, heavy drop shadows, ornamental
rules, or deeply nested cards. Corner rounding SHALL be subtle; large rounded cards SHALL NOT be
used.

A panel's boundary SHALL be legible without an outline in both the dark and light themes required by
`editor-ui-ux`.

#### Scenario: Hierarchy without borders
- **WHEN** three panels are docked adjacently
- **THEN** their boundaries SHALL be readable from luminance and spacing alone

#### Scenario: A card stack is rejected
- **WHEN** an inspector section is proposed as a rounded card containing further rounded cards
- **THEN** it SHALL be flagged against this requirement

### Requirement: Semantic colour
Colour SHALL carry meaning. The editor SHALL maintain one colour vocabulary across every panel,
overlay, gizmo, graph, and visualisation:

| Family | Meaning |
|---|---|
| Neutral grey | Ordinary interface surface |
| White / light grey | Primary text |
| Muted grey | Secondary and derived information |
| Blue | Active, focused, or informational state |
| Green | Success, live, valid |
| Yellow / gold | Selection and attention |
| Orange | Warning |
| Red | Error and destructive consequence |

A hue SHALL NOT be reused for an unrelated meaning within one surface, and a colour SHALL NOT be
introduced for visual variety.

This requirement is subordinate to the accessibility rule in `editor-ui-ux`: colour SHALL NOT be the
sole encoding of any state, and every meaning above SHALL also be carried by shape, icon, or text.

#### Scenario: Meaning survives a colour-blind palette
- **WHEN** a colour-blind-safe palette is selected
- **THEN** selection, warning, and error SHALL remain distinguishable, because each is also encoded
  by outline weight, icon, or label

#### Scenario: A hue is not repurposed
- **WHEN** a panel proposes green to mean "selected"
- **THEN** it SHALL be flagged, because green means valid or live and gold means selection

### Requirement: Axis colour is one language
The mapping **X = red, Y = green, Z = blue** SHALL hold everywhere an axis is presented: transform
gizmos, rotation rings, scale handles, the view-orientation widget, vector property fields in the
inspector, coordinate readouts, and debug visualisation.

An axis-coloured element SHALL use the same three hues as the gizmos, so that a value in the
inspector and a handle in the viewport are recognisably the same axis.

This mapping SHALL NOT be user-remappable, because it is the one colour convention shared with
every other tool in the industry.

#### Scenario: The inspector matches the viewport
- **WHEN** a user drags the red handle in the viewport
- **THEN** the red-labelled X field in the inspector SHALL be the value that changes

#### Scenario: A new visualisation adopts the mapping
- **WHEN** a plugin adds an axis-aligned debug display
- **THEN** it SHALL use the same three hues rather than choosing its own

### Requirement: Selection appearance
Selection SHALL be indicated by a **thin, high-contrast outline in the attention hue** — the warm
yellow-gold of the semantic palette.

The outline SHALL remain legible against both bright and dark scene content, SHALL NOT bloom or
glow, and SHALL NOT obscure the material it surrounds — a user must be able to judge a surface while
it is selected.

Selection SHALL be identifiable at a glance among hundreds of unselected objects of the same type.

Where a project distinguishes **editor selection** from simulated **gameplay selection**, the two
SHALL be visually distinct, and the distinction SHALL hold while playing in the editor.

#### Scenario: Selection is legible on a bright surface
- **WHEN** an object lit by direct sunlight is selected
- **THEN** its outline SHALL remain visible without increasing its width or brightness

#### Scenario: Editor and gameplay selection do not merge
- **WHEN** a strategy project selects units in play mode while the editor also has a selection
- **THEN** the two SHALL be distinguishable

### Requirement: Multi-selection is stated, not implied
When more than one object is selected, the inspector SHALL **state the count and the composition**
of the selection — how many objects, and of what kinds.

Properties common to the selection SHALL remain editable across it. A property whose value differs
across the selection SHALL be shown as **mixed**, and SHALL NOT display an arbitrary member's value.

Editing a mixed property SHALL apply to the whole selection, and SHALL be one transaction.

#### Scenario: A mixed value is visible
- **WHEN** three objects with different scales are selected
- **THEN** the scale field SHALL read as mixed rather than showing the first object's scale

#### Scenario: The selection is countable
- **WHEN** a rectangle selection captures many objects
- **THEN** the inspector SHALL state how many and of what kinds

### Requirement: Gizmo legibility
Transform gizmos SHALL be **acquirable without precision** — handles sized for confident grabbing
rather than for minimal footprint, in the manner of a modern game editor rather than a CAD
application.

The three modes SHALL be visually distinct by shape, not only by colour:

| Mode | Handle form |
|---|---|
| Translate | Directional arrows along each axis, with plane handles at the intersections |
| Rotate | Axis-coloured arcs or rings surrounding the object |
| Scale | Axis-aligned box handles, with a centre handle for uniform scale |

A user SHALL be able to identify the active mode from the gizmo alone, with no reference to a
toolbar.

A **universal** mode combining all three MAY be offered. If offered, it SHALL remain readable — a
universal gizmo that resolves into a dense cluster of overlapping handles SHALL be treated as a
defect, and explicit Move, Rotate and Scale modes SHALL always remain available.

Hover and active states SHALL emphasise the handle under manipulation and de-emphasise the rest.

#### Scenario: The mode is readable from the gizmo
- **WHEN** a screenshot of the viewport is shown with the toolbar cropped out
- **THEN** the active transform mode SHALL be identifiable

#### Scenario: Universal mode stays readable
- **WHEN** the universal gizmo is displayed on a small object
- **THEN** its handles SHALL remain individually acquirable, or the mode SHALL degrade to a
  simpler presentation rather than overlapping

### Requirement: The orientation widget is not a manipulator
The viewport's view-orientation widget SHALL show **only the three principal axes** and their
labels.

It SHALL NOT display rotation rings, scale boxes, translation arrows, or any other form that
resembles a transform gizmo, because its purpose is camera and world orientation and confusing it
with object manipulation is a direct cost to the user.

It SHALL be visually quieter than the transform gizmo, and SHALL support clicking an axis to
align the camera.

#### Scenario: The widget is not mistaken for a gizmo
- **WHEN** a user sees the widget in the viewport corner
- **THEN** it SHALL be distinguishable from the transform gizmo by form, not only by position

#### Scenario: Rings are rejected
- **WHEN** an orientation widget with rotation rings is proposed
- **THEN** it SHALL be flagged against this requirement

### Requirement: Viewport chrome is overlay, not toolbar
Viewport controls — projection mode, rendering mode, show flags, camera speed, snapping, transform
mode, maximise, and debug visualisations — SHALL be presented as **compact overlays within the
viewport**, not as an additional full-width toolbar above it.

Overlays SHALL be individually toggleable, SHALL not obstruct the content being judged, and SHALL
follow the capture rules in `editor-viewport-and-gizmos` — excluded from any image intended to
represent the shipping frame.

Every pixel of vertical chrome removed from around the viewport is viewport, which is the point.

#### Scenario: Controls do not cost viewport height
- **WHEN** viewport controls are added
- **THEN** the viewport's rendered area SHALL NOT shrink

### Requirement: The performance overlay is ambient
Frame cost SHALL be observable without opening a profiler: a compact overlay MAY present frame
rate, frame time, draw time, GPU time and memory.

The overlay SHALL be movable and disableable, SHALL default to a position that does not obstruct
scene content, and SHALL be excluded from clean captures.

Detailed profiling — per-subsystem graphs over time — belongs to the profiler panel, not the
overlay. The overlay answers "is this frame affordable"; the profiler answers "why".

#### Scenario: The overlay does not become a profiler
- **WHEN** a new per-subsystem breakdown is proposed for the overlay
- **THEN** it SHALL be placed in the profiler panel instead

### Requirement: Typography
The editor SHALL use one modern sans-serif family for interface text, and a monospaced family only
for code, console output, and identifiers where character alignment carries meaning.

Typography SHALL be optimised for **density with legibility**: a small number of sizes, clear
separation between primary and secondary text by weight and luminance rather than by size, subtle
headings, and no decorative type.

Numeric values SHALL use tabular figures so that columns of numbers align, and SHALL align on the
decimal separator where a column is compared.

Tab labels and tree rows SHALL remain legible at the compact density mode.

#### Scenario: Numbers align in a column
- **WHEN** an inspector shows a column of float values
- **THEN** their digits SHALL align vertically

#### Scenario: A heading is not oversized
- **WHEN** a section header is added to the inspector
- **THEN** it SHALL be distinguished by weight and spacing rather than by a substantially larger size

### Requirement: Iconography
The editor SHALL use one coherent icon system: geometric, monochromatic by default, consistent in
stroke weight and optical size, and legible at the smallest size the compact density mode uses.

Colour SHALL enter an icon only where it carries semantic meaning from the colour vocabulary.

Icons SHALL NOT reproduce the recognisable iconography of Unity, Unreal Engine, Godot, Blender, or a
host operating system, because a borrowed icon set makes the product read as a derivative of the
tool it borrowed from.

An icon SHALL be accompanied by a tooltip, and by a text label wherever the icon alone is ambiguous.

#### Scenario: An icon is legible at compact density
- **WHEN** the interface is set to compact density
- **THEN** every toolbar icon SHALL remain distinguishable from its neighbours

#### Scenario: A borrowed icon is rejected
- **WHEN** an icon closely reproduces another engine's recognisable symbol
- **THEN** it SHALL be flagged against this requirement

### Requirement: Default workspace composition
The default workspace SHALL be **viewport-first**: the viewport receives the largest single region,
and the surrounding tools SHALL NOT fragment it.

The default arrangement SHALL be:

| Region | Contents |
|---|---|
| Header | Cyberdyne mark, application menus, project and scene name, platform and configuration, live status, account and settings |
| Toolbar | Selection mode, play controls, build, transform tools, snapping, viewport options |
| Left, upper | Scene hierarchy and world outliner, with permanent search |
| Left, lower | Content browser |
| Centre | Viewport, with overlaid chrome |
| Centre, lower | The active specialised editor — visual scripting, animation, materials, sequencing |
| Right | Inspector |
| Right, lower | Diagnostics — console, profiler, tasks |
| Footer | Output log, find in files, command input, save and source-control state |

Users SHALL be able to rearrange all of it, per `editor-ui-ux`. This requirement fixes the
**default**, because the default is what a new user learns and what a screenshot teaches.

#### Scenario: The viewport dominates the default layout
- **WHEN** the editor is opened with a default workspace on a typical display
- **THEN** the viewport SHALL occupy the largest single region

### Requirement: Spatial stability under context
Panel content SHALL adapt to what is selected; **panel position SHALL NOT**.

Selecting terrain, a character, or a strategy unit MAY change which tools and sections a panel
offers. It SHALL NOT move the hierarchy, the inspector, the content browser, or the viewport, and
SHALL NOT dock, undock, open, or close a panel without the user asking.

Users build muscle memory around location. Contextual behaviour that relocates panels destroys it,
and the cost is paid on every interaction thereafter.

#### Scenario: Context changes content, not position
- **WHEN** the user selects a terrain object and then a character
- **THEN** the inspector's sections SHALL change and every panel SHALL remain where it was

#### Scenario: A tool does not open its own panel
- **WHEN** a specialised tool becomes relevant to the selection
- **THEN** it SHALL be offered within an existing region rather than opening a new floating window

### Requirement: Progressive disclosure is the default state
An object's inspector SHALL open showing the properties that are ordinarily edited, with advanced
detail collapsed but present.

Advanced sections SHALL be discoverable — visibly collapsed rather than hidden — and their expanded
state SHALL persist per type, so an advanced user configures once.

Expansion state is presentation, not document state, and SHALL NOT dirty a document.

Adding capability to the engine SHALL NOT automatically add permanently visible interface. A change
that introduces new properties SHALL state where they sit in the disclosure hierarchy.

#### Scenario: A first-time user sees a workable inspector
- **WHEN** a mesh object is selected for the first time
- **THEN** transform, mesh, materials and the common rendering flags SHALL be visible, and
  occlusion, ray tracing, instance data and debug detail SHALL be collapsed

#### Scenario: New capability defends its interface cost
- **WHEN** a subsystem adds twelve new properties
- **THEN** the change SHALL state which are default-visible and why, rather than appending all
  twelve to the default view

### Requirement: Status is ambient
Ordinary state — live connection, save state, source-control state, background operations such as
importing, shader compilation, procedural generation, navigation building — SHALL be communicated
**without interrupting the user**.

Long operations SHALL show visible, cancellable progress in place, per `editor-ui-ux`. Modal
dialogs SHALL be reserved for decisions the user must make and for destructive consequences.

Status indicators SHALL state the condition in text as well as colour, and SHALL be readable
without hovering.

#### Scenario: A background cook does not block editing
- **WHEN** assets are importing
- **THEN** progress SHALL be visible in the footer and the user SHALL continue editing

#### Scenario: Saved state is legible at a glance
- **WHEN** every document is saved
- **THEN** the footer SHALL state it in words, not only by an icon colour

### Requirement: Asset thumbnails are renders
The content browser SHALL present assets as **useful previews** rather than generic file icons
wherever a visual representation is meaningful: meshes as rendered thumbnails, materials on a
representative surface, textures as their own content, and prefabs or units as recognisable
miniature renders.

Thumbnails SHALL be produced by the engine's own renderer, so that a preview and the shipping image
are the same path.

Thumbnails SHALL be generated in the background, SHALL be cached, and SHALL degrade to a typed
placeholder rather than blocking the browser.

Selecting an asset SHALL be able to present a compact preview and its salient properties beside the
browser, without opening a separate editor.

#### Scenario: A hundred similar units are distinguishable
- **WHEN** a project contains many visually similar unit assets
- **THEN** their thumbnails SHALL be renders that distinguish them, not one shared type icon

#### Scenario: Browsing never blocks
- **WHEN** thumbnails are still generating
- **THEN** the browser SHALL remain navigable with placeholders

### Requirement: Graph surfaces are restrained
Graph editors — visual scripting, materials, VFX, AI, animation, procedural generation — SHALL share
one dark graph surface and one node visual language.

Node colour SHALL encode **category** — event, execution, data, gameplay, transformation,
condition, resource — and SHALL be drawn from the semantic vocabulary. A graph SHALL NOT become a
field of arbitrary per-node colours.

Connections SHALL remain readable at the zoom levels users actually work at, SHALL be
distinguishable by type, and SHALL not rely on colour alone to convey type.

The same node in two different graph domains SHALL look the same, because the graph infrastructure
is shared.

#### Scenario: Categories are readable at working zoom
- **WHEN** a graph is viewed at the zoom level that fits a typical function
- **THEN** node categories SHALL be distinguishable and connections SHALL be traceable

### Requirement: Engine vocabulary
Interface text SHALL use **the engine's own vocabulary**, taken from the specifications, and SHALL
NOT adopt another engine's product terms.

Where a competing engine's term is widely understood, the engine's term SHALL be used in the
interface and the familiar term MAY be offered as a search alias, so a user who types the term they
know still finds the feature.

| Use | Not |
|---|---|
| Node, entity | Actor, GameObject |
| Mesh, mesh instance | Static Mesh Actor |
| Graph, script graph | Blueprint |
| Content browser | Content Drawer |
| Prefab | Blueprint class, Prefab variant asset |
| World, scene, cell | Level, Persistent Level |

Vocabulary is identity. An editor that calls things by another engine's names reads as a
reimplementation of that engine regardless of what it does.

#### Scenario: A familiar term still finds the feature
- **WHEN** a user searches for "blueprint"
- **THEN** the script graph editor SHALL be offered, labelled with the engine's own term

#### Scenario: Borrowed vocabulary is caught
- **WHEN** a panel labels a selection count in another engine's terms
- **THEN** it SHALL be flagged against this requirement

### Requirement: The design references are normative
The editor's visual direction SHALL be recorded as **committed reference images** under
`docs/design/`, and those images SHALL be the shared reference for implementation and review.

A reference image is a statement of **visual language** — hierarchy, density, colour, chrome,
composition — and not a statement of feature completeness. Where a reference depicts capability the
engine has not reached, the visual language SHALL still be implementable at the current milestone
with the content that exists.

A change that alters the visual language SHALL update or add a reference image and SHALL state what
changed and why. A reference that no longer reflects the intended language SHALL be replaced rather
than left to decay.

Where a reference image and this specification disagree, **this specification is authoritative**,
and the reference SHALL be corrected.

#### Scenario: A reference does not gate a milestone
- **WHEN** a reference shows volumetric clouds and procedural forests at M5
- **THEN** the layout, density, chrome, and colour system SHALL be implemented at M5, and the
  scene content SHALL be whatever the renderer can produce

#### Scenario: The language changes deliberately
- **WHEN** a change alters selection colour, gizmo form, or workspace composition
- **THEN** it SHALL update the reference imagery in the same change

### Requirement: Forbidden visual patterns
The following SHALL NOT appear, and each SHALL be checkable:

- A panel with a saturated fill, gradient, or accent that encodes nothing
- Colour used for variety where the semantic vocabulary assigns it a meaning
- An axis presented in any mapping other than X red, Y green, Z blue
- A view-orientation widget carrying rings, boxes, or arrows
- A second full-width toolbar between the header and the viewport
- A selection indicator that glows, blooms, or obscures the surface beneath it
- An icon that reproduces another engine's recognisable symbol
- Another engine's product vocabulary in interface text
- A contextual behaviour that moves, opens, or closes a panel the user did not ask for
- A new subsystem's properties appended to the default inspector view without a disclosure decision
- A modal dialog for information that could be ambient status
- A generic file icon where the engine could render a preview

#### Scenario: A proposal is checked
- **WHEN** a panel is proposed with a coloured title bar to distinguish it from its neighbours
- **THEN** it SHALL be flagged against this requirement, and luminance and spacing SHALL be used
  instead
