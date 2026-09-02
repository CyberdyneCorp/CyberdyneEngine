## MODIFIED Requirements

### Requirement: Streaming integration seams
Virtual geometry page streaming SHALL integrate with content and texture streaming rather than
operating independently.

Three levels of streaming SHALL be distinguished: **content streaming** determines which objects
exist (see `world-partition-and-streaming`), **geometry streaming** determines their shape detail,
and **texture streaming** determines their surface detail (see `virtual-texturing`). All three SHALL
be driven from a shared notion of the viewer's region of interest and SHALL be scored and budgeted
through the shared residency policy (see `residency`).

Cell membership SHALL drive geometry **prefetching**: when a cell becomes resident, the root pages
of its geometry SHALL be requested, so that an activated cell is never visible without at least its
coarsest representation.

Geometry and texture residency SHALL be **jointly budgeted**: under memory pressure the residency
layer SHALL decide between geometry detail and texture detail by importance and visible impact,
rather than each system evicting independently.

Geometry residency decisions SHALL remain the responsibility of this system; the shared layer
supplies policy, priority, and budget, not storage.

#### Scenario: Object existence and object detail are distinct
- **WHEN** a region streams in
- **THEN** object existence SHALL be established by content streaming, and geometry and texture
  detail SHALL be streamed separately on demand

#### Scenario: Seams remain open
- **WHEN** a further paged subsystem is introduced
- **THEN** it SHALL be able to share the residency policy and budget without changes to this
  system's page table or request path

#### Scenario: An activated cell is never blank
- **WHEN** a world cell is activated
- **THEN** the root pages of its geometry SHALL already have been requested, so its objects render
  at coarse detail immediately rather than appearing later

#### Scenario: Geometry and textures compete fairly
- **WHEN** memory pressure forces a reduction
- **THEN** the residency layer SHALL weigh geometry detail against texture detail by importance,
  rather than each subsystem reducing independently
