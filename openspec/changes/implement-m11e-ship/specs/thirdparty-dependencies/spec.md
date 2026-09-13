## ADDED Requirements

### Requirement: The intended set is settled at 1.0
The intended dependency set is a table of intents, and an intent with nothing behind it is
indistinguishable from an omission once the project claims to be finished. At **1.0** every entry in
that table SHALL be in exactly one of three states, and the state SHALL be recorded:

1. **Integrated** — declared in the dependency manifest with every field the manifest requires, and
   fetched, built and linked by a build that ran.
2. **Removed from the intent** — through an OpenSpec change against this specification, stating what
   the engine does instead and what capability, if any, is lost. The table's own rule that *"where a
   table entry is marked to evaluate, the requirement is the capability, not the library"* is the
   route for entries that name one; using it for an entry that does not is a change, not a reading.
3. **Deferred** — recorded with what is unmet, why, and the condition that brings it back, per
   `delivery-roadmap`'s deferral rule.

An entry in none of the three SHALL fail the check, naming the library.

**The size of the gap SHALL be stated as a count rather than as a phrase.** "About half the intended
set is not integrated" is a sentence that cannot go red; the number of table entries and the number
of them integrated are two figures a check can compare, and the difference is the scope.

#### Scenario: An intended library has neither an integration nor a decision
- **WHEN** the 1.0 record is evaluated and a library named in the intended set is absent from the
  manifest with no recorded removal and no recorded deferral
- **THEN** the check SHALL fail naming that library

#### Scenario: The intent is what changes
- **WHEN** the engine decides not to integrate a library the intended set names
- **THEN** an OpenSpec change against this specification SHALL remove it from the table with the
  reason and the capability consequence, rather than the table being left to disagree with the tree

### Requirement: A manifest entry is not an integration
A dependency SHALL be counted as integrated only when the engine **fetches, builds and links** it in
a build that ran. A complete manifest entry — version, tag, commit, repository, licence, interface,
scope and justification — is a declaration of intent to integrate, and the two SHALL NOT be
confused by any check, report or record.

This distinction is not theoretical. A dependency has carried a complete manifest entry across
multiple milestones while its feature option did not configure, because its upstream build required
packages the manifest did not provide; every report that counted manifest entries counted it, and
every build that enabled it failed.

Where a dependency is declared and not yet integrable, the manifest SHALL record what blocks it, and
any count of integrated dependencies SHALL exclude it.

#### Scenario: A declared dependency does not configure
- **WHEN** a dependency's feature option is enabled and the configure step fails
- **THEN** the dependency SHALL NOT be counted as integrated, and the failure SHALL name the
  dependency and what it requires

#### Scenario: The count is of what builds
- **WHEN** the number of integrated dependencies is reported
- **THEN** it SHALL be derived from what fetches, builds and links, not from the number of entries in
  the manifest

## MODIFIED Requirements

### Requirement: Attribution
The engine SHALL ship a complete attribution document generated from the manifest, and SHALL
expose it at runtime so games can display required notices without assembling them manually.

**The runtime query SHALL report exactly the dependencies linked into that build**, which is the half
a generated document cannot do: a document is generated once from the whole manifest, and a build is
a particular selection of feature options. A build with an optional dependency disabled SHALL NOT
list it, and SHALL NOT list a dependency present in the tree **only because that optional dependency
reaches it** — a transitive dependency's attribution follows the dependency that pulls it in.

**The query SHALL be available to a shipped game**, not only to the editor or the tools, because the
requirement it exists to satisfy is a licence notice displayed by the game.

**The runtime footprint rule SHALL be observable through it.** Tool-time-only dependencies — Slang,
SPIRV-Cross, the texture encoders, mesh processing, UV unwrapping, the glTF and FBX parsers, and
Recast generation — SHALL NOT appear in a shipped game's attribution, and their appearance is
evidence that they were linked.

#### Scenario: Game credits
- **WHEN** a game needs to display third-party licences
- **THEN** the runtime SHALL provide the text for exactly the dependencies linked into that build

#### Scenario: A disabled dependency is not attributed
- **WHEN** the runtime attribution query is run against a build with an optional dependency's feature
  option disabled
- **THEN** neither that dependency nor any dependency present solely because of it SHALL appear, and
  the answer SHALL differ from the same query against a build with the option enabled

#### Scenario: A tool-time dependency appears in a shipped game
- **WHEN** a shipped game's attribution lists a dependency the runtime footprint requirement declares
  tool-time only
- **THEN** the check SHALL fail naming it, because its presence in the attribution is evidence it was
  linked
