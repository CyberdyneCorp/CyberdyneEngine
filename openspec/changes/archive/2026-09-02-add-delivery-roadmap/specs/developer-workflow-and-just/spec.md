## MODIFIED Requirements

### Requirement: Recipe surface
The recipe set SHALL cover, at minimum, the following task categories, with consistent naming:

| Category | Covers |
|---|---|
| Environment | Dependency check and diagnosis, toolchain setup, first-time bootstrap |
| Build | Engine, editor, tools, shaders, everything; per-profile and per-platform |
| Run | Editor, runtime host, samples, headless runtime |
| Test | Unit, integration, editor headless, determinism, golden-image, performance |
| Quality | Format, lint, static analysis, ABI check, identity-manifest check, spec validation |
| Generate | ABI bindings, the Swift overlay, the Rust SDK, reflection data, documentation |
| Content | Import, cook, package, patch, validate content |
| Diagnose | Profile capture, trace inspection, crash artefact inspection, log collection |
| Roadmap | Per-capability implementation status, capability-record consistency, milestone exit criteria |
| Maintenance | Clean, cache management, dependency update, version bump |
| Release | Version, changelog, artefacts, publication |

Recipe names SHALL be predictable and consistent — the same verb SHALL mean the same thing across
categories — and every recipe SHALL carry a one-line description.

The **Roadmap** category SHALL provide at least a status recipe that reports every capability's
maturity tier and fails when the status record and the specification set disagree, and a milestone
recipe that runs a named milestone's full exit criteria and exits non-zero if any fail.

#### Scenario: A task is findable by guessing
- **WHEN** a developer guesses a recipe name from the naming pattern
- **THEN** the guess SHALL usually be correct, and `just` SHALL list the alternatives when it is not

#### Scenario: Milestone status is one command
- **WHEN** a contributor asks what is implemented and what closes the current milestone
- **THEN** the roadmap recipes SHALL answer both, without anyone reading a document to find out
