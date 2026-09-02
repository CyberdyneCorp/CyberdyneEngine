## MODIFIED Requirements

### Requirement: Distribution artefacts
The build SHALL produce: the **editor** application, **runtime libraries** for embedding, the
**C ABI headers** and ABI description, the **`CyberdyneKit` Swift package**, **runtime templates**
per platform and configuration used when packaging a game, and the **tools** — the build service and
its command-line clients, the cooker, the packager, and the shader compiler.

The build service SHALL be the execution engine behind those command-line tools, so that a
command-line build and an editor build share one dependency graph and one derived data cache (see
`build-and-packaging`).

Artefact names SHALL encode platform, architecture, configuration, and version, and every produced
build SHALL carry the provenance record defined in `build-and-packaging`.

#### Scenario: Packaging a game
- **WHEN** a game is packaged for a platform
- **THEN** the matching runtime template SHALL be combined with the cooked content packages and
  the game's Swift module

#### Scenario: Embedding the engine
- **WHEN** an application embeds the engine as a library
- **THEN** it SHALL drive the runtime through the documented entry points rather than owning
  `main()`

#### Scenario: One execution path
- **WHEN** the same build is produced from the editor and from the command line
- **THEN** both SHALL drive the same service, graph, and cache, and produce identical artefacts
