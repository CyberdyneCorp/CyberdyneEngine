## MODIFIED Requirements

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
