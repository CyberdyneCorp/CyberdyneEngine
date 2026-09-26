## ADDED Requirements

### Requirement: An incremental ledger run never skips a criterion it cannot read
A milestone's closing recipe SHALL keep running its full criteria set by default. An **incremental**
run MAY be requested explicitly, and it SHALL evaluate:

- every criterion the milestone itself declares;
- every earlier criterion that is new or edited since the base commit — its check differs from the
  one in the milestone's plan at that commit, or it was not in that plan;
- every earlier criterion one of whose inputs changed since the base commit;
- a fixed smoke set — the build, the formatting gate, the static analysis gate and the full test
  suite — whatever changed.

A criterion's inputs SHALL be derived from data the repository already records — the build graph, the
test registrations, the recipes' own text and the criterion's own fields — and never from a list kept
by hand. **A criterion whose inputs cannot be derived SHALL be evaluated.** The run SHALL print, for
every criterion, whether it was evaluated and why.

The base commit SHALL default to the commit of the last green full run of that milestone's ledger on
a clean tree; an incremental run SHALL NOT record a base. The full ledger SHALL run nightly and at
M11.e, so an incremental run is never the only
evaluation a criterion receives.

#### Scenario: A change to one subsystem selects that subsystem's criteria
- **WHEN** only files under `src/save/` changed since the base
- **THEN** the save criteria SHALL be evaluated and the renderer's criteria SHALL NOT be

#### Scenario: A change to a shared header selects its dependents
- **WHEN** a header included by the tests of two criteria changed
- **THEN** both criteria SHALL be evaluated

#### Scenario: A criterion whose inputs are unknown is evaluated
- **WHEN** a criterion's inputs cannot be derived — a shell script, a recipe that declares nothing, a
  build tree that is not current
- **THEN** it SHALL be evaluated, and the reason SHALL be printed

#### Scenario: The milestone's own criteria always run
- **WHEN** an incremental run is made and nothing changed
- **THEN** every criterion the milestone declares, and the smoke set, SHALL be evaluated
