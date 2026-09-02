## ADDED Requirements

### Requirement: The project graph is authoritative
A project SHALL be described by a **declarative project manifest** naming: the project and its
version, the engine version it targets, its modules, its plugins and their versions, its content
roots, its build targets, and its per-platform overrides.

The manifest SHALL be the authority on structure. Folder layout MAY be conventional, and critical
dependencies SHALL NOT be inferred from the filesystem.

The manifest SHALL be human-readable, diffable, and schema-validated, with unknown or malformed
entries reported rather than ignored.

#### Scenario: Structure is declared
- **WHEN** a module or plugin is added to a project
- **THEN** it SHALL appear in the manifest, and its presence in a folder alone SHALL NOT include it
  in the build

#### Scenario: Manifest errors are caught
- **WHEN** a manifest entry is malformed or references something that does not exist
- **THEN** it SHALL be reported with the offending entry rather than silently ignored

### Requirement: Modules and their dependencies
A **module** SHALL be the unit of code, declaring: name, type, its public dependencies, its private
dependencies, supported platforms, and whether it supports hot reload.

Module types SHALL include at minimum: runtime, editor, developer, server, tool, and third-party.

**Public** dependencies are visible through a module's own interface; **private** dependencies are
implementation detail and SHALL NOT be transitively exposed to dependents.

A module SHALL only use interfaces from its declared dependencies. Undeclared use SHALL be a build
error, not a link-time accident.

Dependency **cycles** SHALL be detected and rejected.

#### Scenario: Undeclared dependency fails
- **WHEN** a module uses an interface from a module it does not declare
- **THEN** the build SHALL fail naming both modules

#### Scenario: Private dependencies do not leak
- **WHEN** a module depends privately on a third-party library
- **THEN** its dependents SHALL NOT be exposed to that library's headers or symbols

### Requirement: Architectural layering is enforced
The engine SHALL define **architectural layers** — foundation, runtime, rendering, gameplay, editor,
tools — and every module SHALL declare its layer.

Dependencies SHALL only point downward or within a layer. A runtime module SHALL NOT depend on an
editor module; a foundation module SHALL NOT depend on anything above it.

Violations SHALL fail the build and continuous integration, not merely be reported.

Enforcement is mechanical because dependency direction decays silently and is the most expensive
architectural property to recover once lost.

#### Scenario: A layering violation fails CI
- **WHEN** a change makes a runtime module depend on an editor module
- **THEN** continuous integration SHALL fail naming the modules and the layers

#### Scenario: Shipping excludes editor code
- **WHEN** a shipping target is built
- **THEN** no editor-layer module SHALL be reachable from it, and this SHALL follow from the graph
  rather than from a build flag

### Requirement: Plugins
A **plugin** SHALL be a distributable package containing any of: modules, content, editor
extensions, schemas, importers, build steps, and platform binaries.

A plugin SHALL declare a manifest with: a **stable plugin identifier** independent of its display
name, its version, the engine API range it supports, its modules and their types, its supported
platforms, its dependencies on other plugins, and whether it supports hot reload.

Plugin identity SHALL NOT depend on its name or path, so a plugin can be renamed or relocated
without invalidating projects that use it.

A plugin SHALL be enableable per project and per target.

#### Scenario: Renaming a plugin is safe
- **WHEN** a plugin's display name changes
- **THEN** projects referencing it SHALL continue to resolve it by identifier

#### Scenario: Incompatible engine version
- **WHEN** a plugin declares an engine API range that excludes the current engine
- **THEN** it SHALL be reported as incompatible and not loaded

### Requirement: Extension points
Plugins SHALL extend the engine only through **declared extension points**, and SHALL NOT reach into
engine internals.

Extension points SHALL include at minimum: physics, audio, and network transport backends; asset
importers and processors; render features and material nodes and closures; editor panels, property
editors, gizmos, viewport tools, and commands; build steps and platform targets; upscalers; ray
tracing backends; virtual texture producers; source control providers; and streaming and residency
producers.

Each extension point SHALL be **versioned independently** of the engine's release version, so that a
plugin targets an interface rather than a patch release.

The engine's own first-party features SHALL use the same extension points where one exists, so
limitations are discovered internally.

#### Scenario: The engine dogfoods its extension points
- **WHEN** a first-party feature is implemented at an extension point
- **THEN** it SHALL use the public interface, so any inadequacy is found before a third party meets
  it

#### Scenario: Interface versioning
- **WHEN** an extension interface changes incompatibly
- **THEN** its version SHALL increment, and plugins targeting the previous version SHALL be reported
  as incompatible rather than loaded

### Requirement: Plugin lifecycle
Plugin loading SHALL proceed through explicit, separable phases: **load**, **initialise**,
**register**, **start**, **stop**, **unregister**, **shutdown**, **unload**.

Registration SHALL be separable from start, so a plugin can register types, importers, and editor
extensions before any world exists.

Failure in any phase SHALL be contained: the plugin SHALL be reported and disabled, and the engine
SHALL continue where the plugin is not required.

Load order SHALL follow the resolved dependency graph and SHALL be deterministic.

#### Scenario: Types register before worlds exist
- **WHEN** a plugin contributes component types
- **THEN** it SHALL register them during the register phase, before any world is created

#### Scenario: A failing plugin is contained
- **WHEN** a plugin fails to initialise
- **THEN** it SHALL be disabled with a diagnostic and the engine SHALL continue if it is not
  required

### Requirement: The plugin binary boundary is the engine's C ABI
Externally distributed **binary** plugins SHALL cross the existing stable C ABI defined in
`native-abi`. A second plugin ABI SHALL NOT be introduced.

The boundary SHALL expose no standard library types, no third-party types, and no engine internal
layouts. A plugin SHALL be discovered through a documented descriptor entry point taking the host's
API version.

**First-party modules built together with the engine** MAY use native C++ interfaces, since ABI
stability across versions is unnecessary when everything is compiled from one source tree.

Plugins distributed as **source** MAY use the C++ interfaces of their declared dependencies, and
SHALL be rebuilt with the engine.

#### Scenario: One ABI, two audiences
- **WHEN** a binary plugin is distributed
- **THEN** it SHALL use the engine's existing C ABI, and no parallel plugin ABI SHALL exist to keep
  compatible

#### Scenario: No implementation detail in the boundary
- **WHEN** a binary plugin interface is defined
- **THEN** it SHALL contain no standard library or third-party types

### Requirement: Type ownership and unload safety
The type registry SHALL record the **owning module or plugin** of every registered type.

A module or plugin SHALL NOT be unloaded while live instances of the types it owns exist, unless it
supplies a declared migration or destruction policy for them.

Unloading SHALL unregister the owner's types, importers, extension registrations, and callbacks, and
SHALL verify that no registration referencing it remains.

Attempted unload with outstanding instances SHALL be refused with a diagnostic naming the types and
their counts, rather than producing dangling data.

#### Scenario: Outstanding instances prevent unload
- **WHEN** a plugin is asked to unload while entities carry its components
- **THEN** the unload SHALL be refused, naming the types and instance counts

#### Scenario: Registrations are withdrawn
- **WHEN** a plugin unloads successfully
- **THEN** every type, importer, extension, and callback it registered SHALL be withdrawn

### Requirement: Plugin resolution and lockfile
Plugin dependencies SHALL be resolved from declared version constraints — exact, minimum compatible,
or bounded range — into a concrete set.

The resolved set SHALL be written to a committed **lockfile** recording each plugin's identifier,
resolved version, and content hash, so that a build is reproducible and a change of dependency is a
reviewable diff.

Resolution SHALL fail with an explanation when constraints cannot be satisfied, naming the
conflicting requirements.

Building SHALL use the lockfile; updating it SHALL be a deliberate action.

#### Scenario: Reproducible plugin set
- **WHEN** a project is built on another machine
- **THEN** the lockfile SHALL determine exactly which plugin versions are used

#### Scenario: Unsatisfiable constraints
- **WHEN** two plugins require incompatible versions of a third
- **THEN** resolution SHALL fail naming the conflict rather than choosing arbitrarily

### Requirement: Trust tiers for extensions
Distributable extensions SHALL be classified by trust, and the classification SHALL determine what
they may do:

| Tier | May contain | Loaded |
|---|---|---|
| `DataOnly` | Assets and configuration | Freely |
| `Scripted` | Assets plus scripts running in the engine's scripting environment | Freely, within the scripting environment's limits |
| `TrustedNative` | Native binaries | Only with explicit user or project trust |

Native code loaded into the process has the process's privileges. The engine SHALL NOT claim to
sandbox it, and SHALL instead make the trust decision explicit.

Mods SHALL default to `DataOnly` or `Scripted`; loading native mod code SHALL require an explicit
trust decision that the product may disallow entirely.

#### Scenario: A data mod loads freely
- **WHEN** a mod contains only assets and configuration
- **THEN** it SHALL load without a trust prompt

#### Scenario: Native code requires trust
- **WHEN** an extension contains native binaries
- **THEN** it SHALL require an explicit trust decision, and the engine SHALL NOT describe it as
  sandboxed

### Requirement: Layered typed configuration
Configuration SHALL be **schema-defined and typed**, not arbitrary string maps, so it can be
validated, presented in the editor, migrated, and completed by tooling.

Configuration SHALL resolve through declared layers, each overriding the previous: engine defaults,
project, platform, build configuration, user or local, and command line.

Layers permitted in a shipping build SHALL be declared; a local developer override SHALL NOT be able
to alter shipping behaviour unless explicitly allowed.

The effective value of any setting SHALL be inspectable together with **which layer supplied it**.

#### Scenario: Platform override
- **WHEN** a project targets desktop and mobile
- **THEN** the mobile platform layer SHALL override the renderer profile with no code change

#### Scenario: Where did this value come from
- **WHEN** a setting has an unexpected value
- **THEN** the tooling SHALL report which layer supplied it

#### Scenario: Local overrides do not ship
- **WHEN** a shipping build is produced
- **THEN** developer-local configuration layers SHALL NOT be applied
