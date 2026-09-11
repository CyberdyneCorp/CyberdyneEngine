## ADDED Requirements

### Requirement: A second rendering of the status record is generated, not retyped
The status record is authoritative and is reported by a recipe. Where the documentation carries a
**second rendering** of that record — a summary, a grouped list, a count — it SHALL be generated from
the record rather than written out by hand, and the status recipe SHALL **fail** when the rendering
and the record disagree.

The generated region SHALL be delimited in the document so that a reader can see which part is
rendered, and the recipe SHALL be able to rewrite it in place.

`docs/roadmap/capability-matrix.md` carried three such lists, hand-maintained from M0. They were last
retyped at M7's gate and were three milestones stale when M9's read them: eleven capabilities moved at
M8.b and three at M8.c, and none of it reached the lists. Nothing was wrong with the record; what was
wrong is that a person had to notice.

#### Scenario: A hand edit inside the generated region is caught
- **WHEN** the generated region is edited so that it no longer matches the record
- **THEN** the status recipe SHALL fail, naming the document and printing what the record says

#### Scenario: A tier change reaches the documentation
- **WHEN** a capability's tier changes in the record
- **THEN** regenerating SHALL be one recipe invocation, and the gate SHALL stay red until it happens

#### Scenario: The markers are not removable
- **WHEN** the delimiters around a generated region are deleted
- **THEN** the recipe SHALL fail rather than silently stop checking that region
