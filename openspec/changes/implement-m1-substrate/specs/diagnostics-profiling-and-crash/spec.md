## MODIFIED Requirements

### Requirement: Privacy classification
Every field captured by diagnostics, logging, or crash reporting SHALL carry a **privacy
classification**: public, developer, potentially personal, sensitive, or secret.

Credentials, authentication tokens, private communications, and personal files SHALL NEVER be
captured automatically.

Redaction SHALL be enforceable because logging is structured: a field's classification determines
whether it is included in an artefact that leaves the machine.

Artefacts intended to leave a player's machine SHALL declare what classifications they may contain,
and a project SHALL be able to tighten that.

**A source location is classified data, not a name.** Any value carrying a filesystem path —
including one the compiler injects through `__FILE__` or an equivalent — SHALL be a classified
field, reachable by the writer's redaction. Registering such a value as an event or scope *name*
places it structurally beyond redaction, and SHALL NOT be done.

Compiler flags that strip source prefixes are a **mitigation and not the mechanism**: they do not
exist on every toolchain, and a privacy system that depends on one is only as good as the compiler
that happened to build the artefact.

#### Scenario: A build path cannot reach an artefact
- **WHEN** any produced trace, log or crash artefact is inspected for strings
- **THEN** it SHALL contain no absolute path from the build machine, and the check SHALL be a gate
  rather than a review

#### Scenario: The mechanism does not depend on the toolchain
- **WHEN** the engine is built with a compiler offering no source-prefix rewriting
- **THEN** source locations SHALL still be redactable, because they are classified fields

#### Scenario: Nothing sensitive leaves by default
- **WHEN** a crash artefact is prepared for upload
- **THEN** fields classified sensitive or secret SHALL be excluded

#### Scenario: Classification is enforceable
- **WHEN** a new field is logged
- **THEN** it SHALL carry a classification, and an unclassified field SHALL be reported
