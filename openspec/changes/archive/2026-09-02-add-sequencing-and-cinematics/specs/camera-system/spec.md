## MODIFIED Requirements

### Requirement: Camera cuts
A **camera cut** SHALL be a typed event carrying its cause — possession change, vehicle entry,
cinematic start, death, photo mode, teleport, or scripted — and SHALL be published to the systems
whose assumptions it breaks: temporal history, illumination convergence, shadow caching, texture and
geometry residency, and world streaming.

A cut SHALL invalidate the view's temporal history; changing a camera's target SHALL NOT
automatically be a cut.

**Anticipated cuts** — a cinematic's cut list, a scripted teleport — SHALL be announcable in advance
and SHALL become deadlines through the residency layer, so the destination is prepared rather than
discovered.

`sequencing-and-cinematics` is the principal producer of anticipated cuts: a compiled sequence knows
its camera track and its cut list, and SHALL publish upcoming cuts and future camera bounds ahead of
reaching them. A sequence SHALL drive cameras through the **camera stack** — rig selection, blends,
lens, and priority — and SHALL NOT write camera transforms.

#### Scenario: A cut does not smear
- **WHEN** a cinematic cuts between viewpoints
- **THEN** temporal history SHALL be invalidated and no accumulation SHALL blend across the cut

#### Scenario: A known cut is prepared
- **WHEN** a cinematic declares an upcoming cut
- **THEN** content at the destination SHALL be requested against that deadline

#### Scenario: A cinematic camera is still a camera
- **WHEN** a sequence takes control of the view
- **THEN** it SHALL contribute to the camera stack at a declared priority, and the camera system
  SHALL evaluate the result
