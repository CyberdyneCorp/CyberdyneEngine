## MODIFIED Requirements

### Requirement: Specialised editors
The editor SHALL provide dedicated editors for: materials (including the node graph),
animation graphs and clips (a timeline with curve editing), the VFX graph (see `vfx-system`),
**terrain**, **foliage**, **water**, and **environment fields**, tilemaps, UI layout, audio buses
and mixing, navigation baking, lighting and lightmap baking, **abilities and effects**, **gameplay
and utility graphs**, **sequences and cinematics**, and localisation tables.

**All node-graph editors SHALL be built on the shared graph infrastructure defined in
`visual-scripting`**: one canvas, one identity model, one serialization and diff format, one
debugging model — while each domain keeps its own lowering. A sixth bespoke graph editor SHALL NOT
be created.

**All timeline and curve editors SHALL likewise share infrastructure**: the sequence editor, the
animation timeline, and any other keyed-time editor SHALL use one curve editing surface, one keying
model, and one notion of stable identity for tracks, sections, and keys — so that a project has one
editing experience and one diff format for time-based content.

The sequence editor SHALL provide: scrubbing, playback and frame stepping, range looping, markers and
snapping, curve editing, nested sequence navigation, binding assignment and validation, track
filtering and locking, retiming and trimming, and explicit keying modes so that editing a value in
the viewport has a defined effect.

The environment tools SHALL include: terrain sculpting and material painting over a
**non-destructive modifier stack**, biome and field painting, river spline authoring with live flow
and shoreline preview, lake and ocean configuration, foliage painting and rule authoring with
regional preview, and road and spline tools.

Rule-driven tools — foliage placement, field-driven terrain materials, procedural generation — SHALL
be able to show **why** a result occurred, naming the input that drove or excluded it, since a
procedural result that cannot be explained cannot be corrected.

Each SHALL be a plugin using the same panel and undo infrastructure as user plugins, so the
extension API is exercised by the engine's own tooling.

#### Scenario: The engine dogfoods its plugin API
- **WHEN** a built-in editor is implemented
- **THEN** it SHALL use only the public plugin API, so any limitation is discovered internally
  first

#### Scenario: Graph editors share infrastructure
- **WHEN** the material graph, the VFX graph, and a gameplay graph editor are implemented
- **THEN** they SHALL share the node-graph canvas, identity, serialization, diffing, undo, and
  debugging infrastructure rather than each implementing its own

#### Scenario: A procedural result is explainable
- **WHEN** a designer asks why no trees appear in a region
- **THEN** the tool SHALL name the rule input responsible

#### Scenario: Sculpting is non-destructive
- **WHEN** a designer inserts an erosion pass beneath existing sculpting
- **THEN** the sculpting SHALL be preserved and reapplied above it

#### Scenario: Timeline editors share infrastructure
- **WHEN** the sequence editor and the animation timeline are implemented
- **THEN** they SHALL share the curve surface, keying model, and identity handling rather than each
  implementing its own
