## ADDED Requirements

### Requirement: A milestone whose rungs have different artefacts is several milestones
`split-m8-authorable-and-systems` added the rule that a milestone whose closing artefact cannot be
reached without its own risk spike succeeding contains two. That rule is about **risk**. M11 is not
blocked by one spike: it is sixty-five Complete cells whose only shared property is that nothing else
on the ladder claimed them, and seventeen of them arrived at one closing gate because a gate refused
a claim rather than because anyone planned the work there.

A milestone SHALL be split where its scope cannot be judged by **one** closing artefact. The test is
the gate: **a gate that cannot name what it is looking at is not a gate**, and a milestone needing
several unrelated artefacts to demonstrate itself is several milestones sharing a number.

Splitting by count SHALL NOT be done. A rung SHALL be a coherent claim with an artefact that can
refute it, and the ordering of rungs SHALL follow what each rung's artefact depends on — an artefact
authored through an editor comes after the rung that finished the editor, because an artefact
assembled by hand around the missing part proves only the part that was not missing.

Each rung SHALL carry its own change directory, its own ledger under `tools/roadmap/milestones/`, its
own gate in `gates.toml` and its own floor in `selftest.MINIMUM_CRITERIA`, as M8's three rungs do.
The split SHALL be recorded as an **insertion** rather than a renumbering, so every existing reference
to the original milestone stays valid as the name of the group.

#### Scenario: One milestone needs several unrelated artefacts
- **WHEN** a milestone's scope cannot be demonstrated by a single closing artefact
- **THEN** it SHALL be split into rungs that each have one, and each rung SHALL gain its own ledger,
  gate and criteria floor

#### Scenario: A rung is not a bucket of rows
- **WHEN** a proposed split divides a milestone's capabilities by count or by module name
- **THEN** it SHALL be refused, because the resulting rungs have no artefact that can refute either
  of them

#### Scenario: An artefact that depends on another rung waits for it
- **WHEN** one rung's artefact would demonstrate a capability another rung is still building
- **THEN** the rungs SHALL be ordered so the artefact is produced through the finished capability
  rather than around it

### Requirement: A recorded tier is evaluated by a criterion, not argued for in a document
`delivery-roadmap` already requires that a change advancing a capability update the record in the
same commit. That is not sufficient, and M10's gate measured why: four closed milestones' columns
claimed Working for rows the status record held at Seed, task 6.5 closed the gap by **recording** all
four, and the closing gate put the record back — because no criterion in any ledger evaluated any of
the four. `_check_tiers` treats a milestone's exit tier as a **floor**, so a `seed` expectation can
never contradict a Working claim, and one of the four appeared in no ledger but `m0.toml` at all.

A capability's recorded tier SHALL be evaluated by at least one criterion that **runs something and
can fail**. A tier that is written into the record because a document argued for it, with nothing
that re-checks it, SHALL NOT be recorded, and a milestone that wants the tier SHALL write the
criterion that earns it.

Where a tier is claimed by a milestone's column and no criterion evaluates it, that SHALL be a
declared gap naming the rung that will close it, rather than a tier recorded on the strength of the
argument.

#### Scenario: A tier with no criterion is refused
- **WHEN** a milestone would record a capability at a tier no criterion in any ledger evaluates
- **THEN** the record SHALL NOT be written, and the claim SHALL be declared as a gap with its closing
  rung

#### Scenario: An exit tier is a floor and cannot contradict a claim
- **WHEN** the only criterion naming a capability expects a tier at or below the one claimed
- **THEN** that criterion SHALL NOT be counted as evaluating the claim, because a floor cannot go red
  on a tier above it
