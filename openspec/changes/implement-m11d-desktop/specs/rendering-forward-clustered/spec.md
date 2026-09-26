## MODIFIED Requirements

### Requirement: MSAA and alpha coverage
The pipeline SHALL support MSAA at 2×, 4×, and 8× for the opaque and transparent passes, with
depth and colour resolved before post-processing.

MSAA SHALL be expressed through the render graph's attachment model: a pass SHALL declare the sample
count it renders at and the point at which its colour is resolved, and the graph SHALL allocate the
multisampled target and insert the resolve there. The graph SHALL refuse a frame in which a
single-sample target is used while its multisampled counterpart holds rendering no resolve has
reached — a missing resolve, or one declared before the last multisampled write. A pass that cannot
render multisampled SHALL declare so with a reason, and a request to multisample it SHALL be refused
with that reason. A frame that declares no multisampled pass SHALL compile to the same plan it
compiled to before this requirement.

Alpha-tested materials SHALL support **alpha-to-coverage** so foliage antialiases correctly under
MSAA.

#### Scenario: MSAA with screen-space effects
- **WHEN** MSAA and SSAO are both enabled
- **THEN** depth and normals SHALL be resolved before the screen-space passes, which operate at
  single-sample resolution

#### Scenario: A missing or misplaced resolve is refused
- **WHEN** a pass reads a colour target after a multisampled pass wrote it and no resolve was
  declared after the last multisampled write
- **THEN** the graph SHALL refuse to compile the frame rather than render the unresolved target

#### Scenario: Edges are smoother at 4× than at 1×
- **WHEN** the same oblique-edged geometry is rendered at 1× and at 4× through the graph
- **THEN** the 1× image SHALL contain no partially covered pixel, the 4× image SHALL contain
  partially covered edge pixels, and the two SHALL cover the same area

### Requirement: Multi-view rendering
The pipeline SHALL support rendering multiple views in a single pass using layered render targets
and view indices, for stereo XR and cubemap capture.

Per-view matrices SHALL be indexed by view index in the shader; geometry SHALL be submitted once.

Multi-view SHALL be selected by the device's multi-view **capability**, never by backend identity.
Where the capability is absent, a pass declaring several views SHALL be recorded once per view into
the corresponding layer — the baseline path — and SHALL produce the same layers. The compiled plan
SHALL NOT depend on which path is taken.

#### Scenario: Stereo in one pass
- **WHEN** an XR view requests two sub-views on a device that reports multi-view
- **THEN** geometry SHALL be submitted once and amplified to both layers

#### Scenario: Stereo without the capability
- **WHEN** the same two-view pass runs on a device that does not report multi-view
- **THEN** the pass SHALL be recorded once per view into single-layer views of its attachments, and
  the resulting layers SHALL be identical to the multi-view result
