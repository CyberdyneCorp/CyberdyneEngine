## MODIFIED Requirements

### Requirement: Specialised editors
The editor SHALL provide dedicated editors for: materials (including the node graph),
animation graphs and clips (a timeline with curve editing), the VFX graph (see `vfx-system`),
**terrain**, **foliage**, **water**, and **environment fields**, tilemaps, UI layout, audio buses
and mixing, navigation baking, lighting and lightmap baking, and localisation tables.

The environment tools SHALL include: terrain sculpting and material painting over a
**non-destructive modifier stack**, biome and field painting, river spline authoring with live flow
and shoreline preview, lake and ocean configuration, foliage painting and rule authoring with
regional preview, and road and spline tools.

Rule-driven tools — foliage placement, field-driven terrain materials — SHALL be able to show
**why** a result occurred, naming the input that drove or excluded it, since a procedural result
that cannot be explained cannot be corrected.

Each SHALL be a plugin using the same panel and undo infrastructure as user plugins, so the
extension API is exercised by the engine's own tooling.

#### Scenario: The engine dogfoods its plugin API
- **WHEN** a built-in editor is implemented
- **THEN** it SHALL use only the public plugin API, so any limitation is discovered internally
  first

#### Scenario: Graph editors share infrastructure
- **WHEN** the material graph and the VFX graph editors are implemented
- **THEN** they SHALL share the node-graph canvas, undo, and inspector infrastructure rather than
  each implementing its own

#### Scenario: A procedural result is explainable
- **WHEN** a designer asks why no trees appear in a region
- **THEN** the tool SHALL name the rule input responsible

#### Scenario: Sculpting is non-destructive
- **WHEN** a designer inserts an erosion pass beneath existing sculpting
- **THEN** the sculpting SHALL be preserved and reapplied above it
