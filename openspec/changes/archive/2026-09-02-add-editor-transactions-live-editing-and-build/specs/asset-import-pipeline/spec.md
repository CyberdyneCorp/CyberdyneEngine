## MODIFIED Requirements

### Requirement: Cook cache
Cooked outputs SHALL be stored in the engine's **derived data cache** (see `build-and-packaging`):
content-addressed by derivation key, shared between developers and continuous integration, with
local, shared read-only remote, and CI-writable remote tiers.

The import cache SHALL NOT be a separate mechanism from the cache used for shaders, material
programs, geometry and texture pages, and other derived data; there SHALL be one cache covering all
derived data.

The derivation key SHALL include the source content, the importer and processor versions, the import
settings, the target platform, and the cook profile.

**The cache is disposable.** Deleting it SHALL never lose project content; the only consequence
SHALL be a slower next build. It is distinct from the asset registry, which is authoritative
metadata and belongs in source control.

#### Scenario: Shared cache hit
- **WHEN** a developer pulls a branch whose assets CI already cooked
- **THEN** the cooked artefacts SHALL be fetched from the shared cache rather than re-imported
  locally

#### Scenario: Importer version bump
- **WHEN** an importer's version increases
- **THEN** all assets it handles SHALL be re-cooked, since the version is part of the derivation key

#### Scenario: Deleting the cache loses nothing
- **WHEN** the cache is deleted
- **THEN** the project SHALL remain complete and the next build SHALL regenerate the derived data
