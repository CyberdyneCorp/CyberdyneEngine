## ADDED Requirements

### Requirement: Selection and highlight outlines
The frame SHALL draw outlines around the objects a game or the editor marks, after tonemapping and
before the interface, so that an outline is the display-referred colour it was asked for, does not
bloom, and is not scaled by exposure. A mark SHALL name an object by its stable identity and SHALL
carry a kind — selected, drawn as a solid outline of a configured width, or hovered, drawn as a
softer glow — and a colour. Gameplay SHALL be able to mark an entity through a component reachable
from every scripting language the engine hosts.

An outline SHALL be drawn only outside the marked object's silhouette and within the configured
width of it, and SHALL NOT cover the object's own visible surface. Where a marked object is hidden
by other geometry, the outline beside the hidden part SHALL be drawn in an occluded style —
dimmed, and optionally dashed — and the hidden part MAY be tinted; the occluded style SHALL be
switchable off.

#### Scenario: The outline follows the silhouette
- **WHEN** several objects are marked
- **THEN** every pixel the outline changes SHALL lie within the configured width of a marked
  object's silhouette, and no pixel inside a marked object's visible surface SHALL change

#### Scenario: Unmarked objects are untouched
- **WHEN** an unmarked object is farther than the widest outline from every marked object
- **THEN** its pixels SHALL be exactly those of the frame without outlines

#### Scenario: Colour and width follow the request
- **WHEN** a selected object's outline colour or width is changed
- **THEN** the outline SHALL be that colour byte for byte in the output and exactly that many pixels
  wide

#### Scenario: A unit behind a building
- **WHEN** a selected unit is partly hidden by a building
- **THEN** the occluded style SHALL be drawn beside and over the hidden part only, and the outline
  beside the visible part SHALL be the solid selected outline

#### Scenario: Nothing marked is the frame before
- **WHEN** outlines are enabled and nothing is marked
- **THEN** the frame SHALL be byte-identical to the frame without the outline stage
