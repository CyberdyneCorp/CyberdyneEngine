## ADDED Requirements

### Requirement: An artefact that claims content was authored states what was authored
A milestone artefact whose claim is that content was **authored** — through the editor, in a project,
by a person — SHALL report which part of it is authored content and which part its own code
constructs, and the report SHALL be produced by the artefact rather than written beside it.

This exists because the failure it prevents has already happened on this ladder in the adjacent form.
M8.b's closing artefact drew its silhouettes from the sample's own table rather than from the mesh
handle the frame resolved, so the picture *"would have drawn a defect correctly"*; the gate found it
and turned the fix into a check. A sample that claims a game was authored in the editor, and whose
C++ quietly builds the scene, is the same defect with the evidence one level further from the reader.

The report SHALL be concrete rather than narrative: the authored content files the artefact loads,
and the part of the artefact's own source that constructs scene content. The artefact SHALL **fail**
rather than degrade when an authored content file it claims is missing, so that the dependency on
authored content is real and not decorative.

A milestone that cannot make the claim SHALL say so in the artefact's own output. An artefact that
is honest about being half-authored is worth more to the record than one that is silent about it,
and silence is what this requirement removes as an option.

#### Scenario: The authored half is load-bearing
- **WHEN** an authored content file the artefact loads is removed
- **THEN** the artefact SHALL exit non-zero naming the missing file, rather than falling back to
  code that produces an equivalent result

#### Scenario: The split is reported, not asserted
- **WHEN** the artefact runs
- **THEN** it SHALL print the authored content it loaded and the scene content its own code
  constructed, so a reader can judge the claim without reading the sample's source

#### Scenario: A claim larger than the evidence is a finding
- **WHEN** an artefact's recorded claim says content was authored and its report shows that content
  constructed in code
- **THEN** the check SHALL fail, naming the claim and the file
