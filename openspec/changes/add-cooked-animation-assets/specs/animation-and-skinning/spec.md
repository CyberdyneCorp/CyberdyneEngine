# animation-and-skinning Spec Delta

## ADDED Requirements

### Requirement: Cooked animation assets
Skeletons, clips and compiled animation programs SHALL have cooked records that the runtime loads by
asset id through the asset system, with no importer and no graph compiler linked. A compiled program
SHALL be written at cook time and rebuilt at load time by validating and copying it, and a record
whose indices or lengths are out of range SHALL be refused at load rather than read out of bounds.

A rig SHALL be bound from asset ids by matching the program's clip table to the given clips by name;
a clip the program names and the rig lacks SHALL be refused, naming the clip, rather than bound as
the reference pose. A clip whose tracks were cooked for another skeleton's joints SHALL be refused,
naming the joint. A missing asset SHALL be refused, naming its id.

A cooked clip SHALL be reloadable in place: after a reload, every rig and instance using it SHALL
play the new clip without being rebuilt, and a reload that does not decode or no longer matches its
skeleton SHALL be refused, keeping the working clip.

A character SHALL be cookable as a build-graph node from imported sources: its skeleton, its clips
retargeted onto that skeleton where their rigs differ, and its compiled program.

#### Scenario: Round trip
- **WHEN** a skeleton, a clip or a program is encoded, decoded and encoded again
- **THEN** the two encodings SHALL be byte-identical, and a skeleton or clip the importer cooked SHALL
  re-encode to the importer's bytes

#### Scenario: A loaded rig plays as the built one
- **WHEN** a rig bound from asset ids and the rig built in memory from the same skeleton, clips and
  program play the same requests
- **THEN** their poses and root motion SHALL be bit-identical

#### Scenario: No compiler in a loading runtime
- **WHEN** a binary loads and plays a cooked program and defines the compiler's entry point itself
- **THEN** it SHALL link, proving the runtime pulled in no compiler

#### Scenario: Missing clip
- **WHEN** a program names the clip `die` and the rig is given no clip of that name
- **THEN** binding SHALL fail with an error naming `die`

#### Scenario: Hot reload
- **WHEN** a clip a live instance is playing is re-cooked with different keys and reloaded
- **THEN** the instance SHALL play the new keys on its next evaluation without being prepared again,
  and a torn reload SHALL leave it playing the previous clip
