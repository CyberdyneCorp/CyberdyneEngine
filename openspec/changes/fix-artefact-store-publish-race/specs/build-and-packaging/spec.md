## MODIFIED Requirements

### Requirement: Artefacts are immutable
A derived artefact SHALL be identified by the hash of its content. An artefact with a given identity
SHALL NEVER change; different content receives a different identity.

Artefacts SHALL therefore be safely shareable between concurrent builds, machines, and processes
without locking the project. A writer that finds an artefact's identity already published, including
one that loses a race to publish it, SHALL report the artefact present and SHALL NOT replace the
published file.

Locking SHALL be scoped to individual outputs and manifests, never to the project as a whole.

#### Scenario: Concurrent builds do not conflict
- **WHEN** two builds run simultaneously against the same cache
- **THEN** they SHALL share artefacts safely without a global lock

#### Scenario: Identity implies content
- **WHEN** two artefacts have the same identity
- **THEN** their content SHALL be identical

#### Scenario: Two writers of one artefact
- **WHEN** two writers store the same content at once, and the second finds the identity absent but
  the first publishes it before the second does
- **THEN** both writes SHALL succeed, the second SHALL report the artefact present, and the
  published file SHALL be the first writer's, unchanged, even while it is open for reading
