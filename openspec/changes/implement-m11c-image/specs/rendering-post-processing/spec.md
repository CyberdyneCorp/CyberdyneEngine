## ADDED Requirements

### Requirement: The chain in the picture is the chain in the frame
This specification fixes the chain's order and its colour space, and every requirement below it is
about a stage. **None of it constrains what a published picture actually ran**, and the project's
largest published capture is produced by a target that links neither the post chain nor the assembled
frame — so a caption claiming tone mapping and anti-aliasing would have been wrong with nothing able
to detect it.

A frame that is captured for publication SHALL emit the list of post stages it actually executed,
with their order and their quality level, produced by the frame rather than written by hand.

A published capture SHALL be accompanied by that stage list, and where a caption states that a stage
ran, the statement SHALL be checkable against it.

A capture intended for publication SHALL be taken with the budget arbiter pinned, so the published
frame is not one the arbiter degraded mid-capture and then described as authored quality.

#### Scenario: A caption claims a stage the frame did not run
- **WHEN** a published capture's caption names a post stage absent from the frame's emitted stage
  list
- **THEN** the check SHALL fail naming the stage, rather than the difference resting on a reader's
  judgement

#### Scenario: The chain is removed and the picture changes
- **WHEN** the same frame is captured with the post chain and with none
- **THEN** the two captures SHALL differ, and a capture that is unchanged SHALL be reported as
  evidence that the picture is not passing through the chain

#### Scenario: A capture is taken pinned
- **WHEN** a publication capture is taken while the arbiter is unpinned
- **THEN** the capture SHALL be refused, because a degraded frame published as an authored one is a
  claim about quality the engine chose not to render
