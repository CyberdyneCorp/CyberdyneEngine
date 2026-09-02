## MODIFIED Requirements

### Requirement: Streaming integration seams
Virtual geometry page streaming SHALL integrate with content streaming rather than operating
independently.

Two levels of streaming SHALL be distinguished: **content streaming** determines which objects
exist, and **geometry streaming** determines their detail. Both SHALL be driven from a shared
notion of the viewer's region of interest.

Content streaming is owned by `world-partition-and-streaming`. Cell membership SHALL drive geometry
**prefetching**: when a cell becomes resident, the root pages of its geometry SHALL be requested,
so that an activated cell is never visible without at least its coarsest representation.

Geometry residency SHALL remain the responsibility of this system: cell membership provides the
prefetch hint, and the feedback-driven residency manager decides detail.

Virtual texture streaming does not exist; the contract by which geometry and texture residency
would be jointly budgeted SHALL be specified as a seam.

#### Scenario: Object existence and object detail are distinct
- **WHEN** a region streams in
- **THEN** object existence SHALL be established by content streaming, and geometry detail SHALL be
  streamed separately on demand

#### Scenario: Seams remain open
- **WHEN** a virtual texture capability is introduced
- **THEN** it SHALL be able to share the residency budget without changes to the page table or
  request path

#### Scenario: An activated cell is never blank
- **WHEN** a world cell is activated
- **THEN** the root pages of its geometry SHALL already have been requested, so its objects render
  at coarse detail immediately rather than appearing later
