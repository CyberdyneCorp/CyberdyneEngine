## MODIFIED Requirements

### Requirement: Cyberdyne identity, not an engine emblem
The editor SHALL present the product's own visual identity. The normative artwork is
`docs/design/images/cyberengine-logo.png`, which supersedes the bare mark previously named here: it
is a complete identity system rather than a single symbol.

**The product is CyberEngine; the publisher is Cyberdyne.** The lockup reads "CyberEngine — by
Cyberdyne", and interface text SHALL follow it: the application is CyberEngine, and Cyberdyne is
named only where a publisher is named. The repository directory retains its historical name; that is
a filesystem artefact and not a product name.

The identity provides four lockups, and each SHALL be used where it fits rather than one being
scaled to serve all:

| Lockup | Where |
|---|---|
| **Horizontal** | The editor's application header, and anywhere width is available |
| **Vertical** | Splash, about, and other centred contexts |
| **Monochrome** | Where the surface cannot carry the full mark, and in print |
| **App icon** | The desktop icon, the taskbar, and the window's own icon |

The mark's geometry SHALL NOT be redesigned, and no separate engine emblem SHALL be introduced. In
the editor header the mark identifies the product; it occupies the header and no larger surface, and
it SHALL NOT compete with the viewport for attention.

The identity is rendered with metallic gradients and a blue emissive core. **Those are properties of
the logo, not licence for the interface.** The surrounding chrome remains charcoal and flat per the
surface system; a header that picks up the logo's gradients has misread it.

#### Scenario: The lockup fits its context
- **WHEN** the identity appears in a narrow or a centred context
- **THEN** the lockup made for that shape SHALL be used, rather than the horizontal one scaled

#### Scenario: The mark is not redrawn
- **WHEN** a compact header presentation is needed
- **THEN** the mark SHALL be cropped or scaled, and SHALL NOT be substituted with a new symbol

#### Scenario: No second brand
- **WHEN** editor branding is proposed
- **THEN** it SHALL use this identity, and an engine-specific logo SHALL be rejected

#### Scenario: The logo's treatment does not spread
- **WHEN** the interface is styled
- **THEN** the metallic gradient and emissive core SHALL remain confined to the mark, and the chrome
  SHALL stay charcoal and flat

### Requirement: The orientation widget is not a manipulator
The viewport's view-orientation widget SHALL be **unmistakable for the transform gizmo**, because
reaching for one and getting the other is a cost paid on every glance.

What makes a manipulator is **rotation rings, planar handles and scale boxes**. The widget SHALL
carry none of them. Axis arrows are not what make a gizmo a manipulator — the transform gizmo is
identified by its rings and planes — and the widget SHALL be free to use arrows, a cube body and
axis labels, as `docs/design/images/scene-orientation-gizmo.png` does.

The two SHALL be separated by **form, size and position together**: the widget is small and fixed in
a viewport corner, the transform gizmo is large and centred on the selection. Neither separation
alone is sufficient.

The widget SHALL hold a **constant screen size**, in the range 56–96 pixels with 72 as the default,
and SHALL NOT scale with camera distance.

Its interactions SHALL be, per the reference: **click an axis to snap the camera to that view**,
**drag anywhere to orbit**, scroll to zoom, and modifier-drag to pan. It SHALL offer the seven view
presets — perspective, top, bottom, front, back, left, right — and SHALL show the current view as
text that can be cycled.

Dragging it orbits the **camera**, never the selection. That is not a contradiction of this
requirement; it is what the widget is for.

The widget SHALL have normal, hover, active and disabled states, and SHALL be legible in both
themes.

#### Scenario: The widget is not mistaken for a gizmo
- **WHEN** both the widget and a transform gizmo are visible in one viewport
- **THEN** they SHALL be distinguishable by form as well as by size and position — the widget
  carrying no rings, no planar handles and no scale boxes

#### Scenario: Dragging orbits the camera and moves nothing
- **WHEN** a user drags the widget while an object is selected
- **THEN** the camera SHALL orbit, the selection SHALL be unchanged, and no transaction SHALL be
  produced

#### Scenario: Clicking an axis snaps the view
- **WHEN** a user clicks the widget's Y axis
- **THEN** the camera SHALL move to the top view, and the current-view label SHALL say so

#### Scenario: Rings are rejected
- **WHEN** an orientation widget with rotation rings is proposed
- **THEN** it SHALL be flagged against this requirement

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

**The normative reference is `docs/design/images/transform-gizmo.png`.** It fixes the following,
which were previously left to implementation:

| | |
|---|---|
| Modes and shortcuts | Move `W`, Rotate `E`, Scale `R`, Universal `T` |
| Translate | Axis arrows, plus **planar handles** at the axis pairs for XY, XZ and YZ |
| Rotate | Axis-coloured rings around the object, plus an outer ring for screen-space rotation |
| Scale | Axis-aligned box handles, plus a **centre cube for uniform scale** |
| Centre handle | Three distinct affordances: drag the centre circle to move in screen space, the centre cube to scale uniformly, the outer ring to rotate in screen space |
| Space | World and Local, switchable |
| Pivot | Pivot, Centre, Bounds, Individual |
| Snapping | Configurable increments, with grid and surface snapping; `Ctrl` snaps temporarily, `Shift` is precision, `Alt` duplicates and transforms, `X`/`Y`/`Z` lock an axis |
| Numeric entry | Position, rotation and scale typed directly, per axis |

**The gizmo SHALL hold a constant size on screen regardless of camera distance**, so that a distant
object is as manipulable as a near one. This is what makes the tool usable in a scene with real
scale, and it is the property most often lost by drawing the gizmo in world units.

**Three states SHALL be distinguishable: normal, hover, and active.** The hovered handle SHALL be
emphasised before it is pressed, so a user knows what they are about to grab rather than discovering
it by dragging the wrong axis.

#### Scenario: A distant object is as manipulable as a near one
- **WHEN** the camera moves far from a selected object
- **THEN** the gizmo SHALL remain the same size on screen and its handles SHALL stay acquirable

#### Scenario: The handle under the cursor is known before it is pressed
- **WHEN** the pointer is over an axis handle
- **THEN** that handle SHALL be emphasised and the rest de-emphasised, before any drag begins

#### Scenario: The centre offers three operations, distinguishably
- **WHEN** a user targets the centre of the gizmo
- **THEN** the screen-move circle, the uniform-scale cube and the screen-rotate ring SHALL be
  separately targetable rather than resolved by guesswork

#### Scenario: The mode is readable from the gizmo
- **WHEN** a screenshot of the viewport is shown with the toolbar cropped out
- **THEN** the active transform mode SHALL be identifiable

#### Scenario: Universal mode stays readable
- **WHEN** the universal gizmo is displayed on a small object
- **THEN** its handles SHALL remain individually acquirable, or the mode SHALL degrade to a
  simpler presentation rather than overlapping
