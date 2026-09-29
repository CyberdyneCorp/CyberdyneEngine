## ADDED Requirements

### Requirement: The lighting and lightmap baking editor bakes and inspects lightmaps
The lighting and lightmap baking editor SHALL bake a level's lightmaps with the engine's bake through
a registered command that runs off the interface thread as a long operation, whose progress the
unified progress surface shows and which is cancellable there and by a registered command; a
cancelled bake SHALL leave the previously cooked lightmap unchanged. The editor SHALL offer a
lightmap texel-density view as an engine debug view: every lightmapped surface drawn as a checker of
its own lightmap texels, coloured by the density its lightmap gives it against the level's target,
and every other surface distinguishable as having none. The editor SHALL be drawn through the shared
specialised-editor panel scaffold; because the bake edits no document, the editor SHALL declare it
as an operation outside every document rather than as an undoable command.

#### Scenario: Bake and cancel from the editor
- **WHEN** a user or an agent starts a lightmap bake and then cancels it
- **THEN** the bake SHALL run as an operation reporting the texels traced, SHALL stop, and SHALL
  settle as cancelled with nothing written

#### Scenario: The density view is the engine's
- **WHEN** the lightmap density view is requested
- **THEN** the engine's frame SHALL draw a surface at the level's density green, one at several times
  it red, and a surface with no lightmap grey
- **AND** the same surface measured against a target several times its density SHALL be drawn blue
- **AND** the view's colours SHALL be the same at any exposure

#### Scenario: The lighting editor is a scaffolded tool whose bake is its declared operation
- **WHEN** the editor registers its specialised editors
- **THEN** the lighting and lightmap baking editor SHALL be drawn through the shared panel scaffold,
  its cancel command and density view SHALL pass the scaffold's read-or-reversible check, and its
  bake SHALL be accepted only as a declared operation that is an external effect and an agent tool
  without exclusion
