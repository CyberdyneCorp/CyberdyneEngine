## ADDED Requirements

### Requirement: The lighting and lightmap baking editor bakes and inspects lightmaps
The lighting and lightmap baking editor SHALL bake a level's lightmaps with the engine's bake through
a registered command that runs off the interface thread as a long operation, whose progress the
unified progress surface shows and which is cancellable there and by a registered command; a
cancelled bake SHALL leave the previously cooked lightmap unchanged. The editor SHALL offer a
lightmap texel-density view as an engine debug view: every lightmapped surface drawn as a checker of
its own lightmap texels, coloured by the density its lightmap gives it against the level's target,
and every other surface distinguishable as having none.

#### Scenario: Bake and cancel from the editor
- **WHEN** a user or an agent starts a lightmap bake and then cancels it
- **THEN** the bake SHALL run as an operation reporting the texels traced, SHALL stop, and SHALL
  settle as cancelled with nothing written

#### Scenario: The density view is the engine's
- **WHEN** the lightmap density view is requested
- **THEN** the engine's frame SHALL draw a surface at the level's density green, one at several times
  it red, and a surface with no lightmap grey
