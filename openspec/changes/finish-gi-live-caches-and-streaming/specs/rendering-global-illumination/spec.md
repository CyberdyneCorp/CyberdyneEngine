## ADDED Requirements

### Requirement: Live GI caches at a bounded host cost
The caches' per-frame arithmetic — the surface cache's card shading and the radiance cache's probe
gather — SHALL run on the device by default wherever the device GI runs, with the host keeping every
decision. The host's per-frame cost of those decisions SHALL be bounded by work proportional to what
changed and to the frame's budget, not to the scene: a frame in which nothing the probe placement or
the distance field reads changed SHALL not rescan them, the budgeted selection SHALL not sort the
whole cache, and a card that moved SHALL re-list only its own entries in the device's lookup. Every
such shortcut SHALL be held by a test to the full computation it replaces, and the host
implementation SHALL remain selectable as the oracle.

#### Scenario: A game's frame with the caches live
- **WHEN** a furnished interior runs with a light moving and an object moving through the GI
- **THEN** the host's update SHALL take under a millisecond at its median, the device SHALL shade the
  cards and gather the probes, and the GI's device cost SHALL be stated beside the cost with the
  caches held

#### Scenario: A still window
- **WHEN** a frame moves no clipmap window and invalidates nothing
- **THEN** the field's scroll and the probe placement SHALL report what a full scan reports without
  scanning

### Requirement: GI content streams with world cells
The GI's scene content — surface cards, distance-field placements, hardware-tier instances,
emitters and GI volumes — SHALL be ingested when a world cell carrying illumination becomes active
and evicted when it deactivates or is evicted, by a consumer of the world's cell events. Ingestion
SHALL be spread over frames by a stated budget, never splitting an emitter, and what an eviction
releases SHALL be charged to the same budget. The GI of streamed cells SHALL equal, within float
noise, the GI of the same cells ingested at once, and streaming SHALL NOT put a frame of the GI above
a frame deadline.

#### Scenario: A traveller crosses a streamed world
- **WHEN** cells activate ahead of a traveller and deactivate behind
- **THEN** their GI content SHALL arrive a budget a tick and leave with them, the device copies SHALL
  upload only what arrived, left or was invalidated, and the GI at the traveller SHALL be the GI of a
  system that never streamed

### Requirement: GI volumes have owners
GI volumes SHALL be placed per owner — the author's, the running game's, the streamed cells' — and
replacing one owner's volumes SHALL NOT remove or re-place another's. Placing a volume again exactly
as it was SHALL NOT retire the probes it reaches.

#### Scenario: An editor edit during Play
- **WHEN** an author edits the world's GI while a game in Play has placed a volume
- **THEN** the game's volume SHALL stay placed, whether the edit is applied in place or rebuilds the
  GI, and it SHALL leave with the game at Stop

### Requirement: Promoted emitters light the frame directly
An emitter promoted to a light SHALL contribute to the forward frame's own direct lighting, as a
light whose intensity falls with the cosine from the emitter's normal and whose flux is the
emitter's, so the frame's direct term and the GI's card shading agree wherever the emitter is small
against its distance. A frame with no promoted emitter SHALL be the frame without the feature.

#### Scenario: A small lamp over a floor
- **WHEN** a 30 cm lamp is promoted two metres over a floor
- **THEN** the frame's illuminance from it SHALL be within a stated bound of the GI's form factor,
  and the frame SHALL show the lamp's direct light

### Requirement: Simulated bodies move their GI objects in editor Play
In the editor's Play, an object the GI holds as movable SHALL follow its simulated body's world
transform, priced by its illumination class, and SHALL return to its authored transform at Stop.

#### Scenario: A body falls in Play
- **WHEN** a rigid body the GI holds falls during Play
- **THEN** its GI object SHALL be where the body is, and after Stop where the document places it
