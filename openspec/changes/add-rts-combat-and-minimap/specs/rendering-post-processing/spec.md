## ADDED Requirements

### Requirement: Topographic minimap
The engine SHALL bake a minimap of a height field as a map draws ground — the ground's colours,
hillshading by a light from the image's top left, contour lines at a chosen interval with every
fifth an index line, water shaded by its depth below a water level with its shore lined, and roads
laid over the ground — once, and SHALL draw a team's fog of war over the bake through the same field
sampler the frame's fog of war uses: a visible point as baked, an explored one grey and darkened, a
never-explored one black. A contour line SHALL lie on a texel exactly when a level of the interval
lies between its height and a neighbour's lower one. The fog SHALL be redrawn only over the cells of
the field that changed and their neighbours, giving exactly the picture a full pass gives. Units and
buildings SHALL be stamped as icons over the result.

#### Scenario: Contours where the levels cross
- **WHEN** the minimap of a map is baked at a contour interval
- **THEN** every texel a level crosses below it SHALL carry a line, and no other texel SHALL

#### Scenario: Through the fog
- **WHEN** the minimap is drawn through a team's field
- **THEN** a texel over visible ground SHALL be the bake's, one over explored ground grey, and one over never-explored ground black

#### Scenario: Redrawn where it moved
- **WHEN** a field changes in a few cells between two compositions
- **THEN** the minimap SHALL redraw only the texels over those cells and their neighbours, and be texel for texel what a full pass draws
