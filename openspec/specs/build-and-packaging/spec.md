# build-and-packaging Specification

## Purpose

Defines how source becomes a shippable, patchable product: the build graph, the derived data cache,
the build service, packaging, and patching.

A build is a **graph of derivations**, not a script. Each node declares its inputs and outputs and
carries a **derivation key** hashing content, tool versions, parameters, platform and profile; a key
already in the cache means the work does not run. Three rules make that hold, and each rules out a
specific common failure: **undeclared reads are defects**, because they produce stale outputs no
invalidation can catch; **outputs are immutable**, which is what makes caching and concurrent builds
safe rather than racy; and **derivation is deterministic**, verified in CI, because determinism is
the precondition for both cache sharing and content-addressed patching.

The cache is **disposable** — deleting it must never lose project content — and is distinct from the
asset registry, which is authoritative metadata and belongs in source control. Blurring the two
produces a project whose correctness depends on a cache.

Patching is already half-built: geometry pages, virtual texture pages, world cell payloads and
shader binaries were each made content-addressed for their own reasons, so a patch transfers the
chunks whose hashes changed. Application stages, verifies and switches atomically, because a failed
patch must leave the previous build playable.

Two questions are first-class rather than reports, because they decide whether a team can control
build size: *why is this asset in the build*, and *what references it*.

## Requirements

### Requirement: The build is a graph of derivations
Building SHALL be expressed as a **directed acyclic graph** of nodes, each a derivation with
declared inputs and outputs: compiling a module, generating code, importing an asset, cooking for a
target, compiling a shader, building a pipeline state, packaging, and producing a manifest.

A build SHALL NOT be a procedural script whose correctness depends on ordering by hand.

Independent nodes SHALL be executable in parallel, and the graph SHALL be inspectable so that a
developer can see why a node ran.

#### Scenario: Independent work runs in parallel
- **WHEN** shader compilation and texture cooking have no dependency between them
- **THEN** they SHALL execute concurrently

#### Scenario: The graph explains itself
- **WHEN** a node runs unexpectedly
- **THEN** the tooling SHALL show which input changed and which nodes depend on it

### Requirement: Derivation keys
Every node SHALL have a **derivation key** hashing: the content of its declared inputs, the versions
of the tools it invokes, its parameters, the target platform, the cook and renderer profiles, and
the relevant configuration.

If a key is present in the derived data cache, the node SHALL NOT execute.

Keys SHALL NOT incorporate timestamps, absolute paths, hostnames, or environment state that varies
between machines, since any of these defeats sharing.

#### Scenario: Cache hit skips work
- **WHEN** a node's key is already in the cache
- **THEN** its outputs SHALL be taken from the cache and the node SHALL not run

#### Scenario: A tool upgrade invalidates
- **WHEN** a cooker's version increases
- **THEN** the keys of every node invoking it SHALL change and their outputs SHALL be recooked

### Requirement: Inputs are explicit
Every node SHALL declare its inputs. **Reading an undeclared input SHALL be a defect**, because an
undeclared read produces stale outputs that no invalidation can detect.

The build SHALL be able to verify declared inputs — by sandboxing, by tracing file access, or by
auditing — in at least one supported mode, and undeclared access SHALL be reported.

Outputs SHALL likewise be declared, and a node writing outside them SHALL be reported.

#### Scenario: A hidden read is caught
- **WHEN** a cooker reads a configuration file it did not declare
- **THEN** verification SHALL report it rather than allowing silently stale outputs

#### Scenario: Stray output is caught
- **WHEN** a node writes a file outside its declared outputs
- **THEN** it SHALL be reported

### Requirement: Artefacts are immutable
A derived artefact SHALL be identified by the hash of its content. An artefact with a given identity
SHALL NEVER change; different content receives a different identity.

Artefacts SHALL therefore be safely shareable between concurrent builds, machines, and processes
without locking the project.

Locking SHALL be scoped to individual outputs and manifests, never to the project as a whole.

#### Scenario: Concurrent builds do not conflict
- **WHEN** two builds run simultaneously against the same cache
- **THEN** they SHALL share artefacts safely without a global lock

#### Scenario: Identity implies content
- **WHEN** two artefacts have the same identity
- **THEN** their content SHALL be identical

### Requirement: Deterministic derivation
Given identical inputs, tools, and configuration, a node SHALL produce **byte-identical output**.

Cooked artefacts SHALL contain no timestamps, absolute paths, build machine identity, or
iteration-order-dependent data.

Determinism SHALL be verified in continuous integration by producing key artefacts twice and
comparing them.

Determinism is a precondition for cache sharing, for content-addressed patching, and for
reproducing a shipped build months later.

#### Scenario: Rebuild is identical
- **WHEN** the same content is cooked twice
- **THEN** the outputs SHALL be byte-identical

#### Scenario: Non-determinism fails CI
- **WHEN** an artefact differs between two runs on identical input
- **THEN** continuous integration SHALL fail naming the artefact

### Requirement: Derived data cache
Derived artefacts SHALL be stored in a **derived data cache**, content-addressed by derivation key,
with tiers: local, a shared read-only remote, and a writable remote populated by continuous
integration.

The cache SHALL cover every kind of derived data: imported assets, cooked assets, compiled shaders,
pipeline states, virtual texture and geometry pages, navigation data, baked lighting, and compiled
material programs.

**The cache SHALL be disposable.** Deleting it SHALL never lose project content; the only
consequence SHALL be a slower next build.

The cache SHALL be distinct from the asset registry: the registry is authoritative metadata about
assets and their relationships and belongs in source control; the cache is regenerable output and
does not.

#### Scenario: Deleting the cache is safe
- **WHEN** the derived data cache is deleted entirely
- **THEN** the next build SHALL regenerate it, and no project content SHALL be lost

#### Scenario: CI populates for the team
- **WHEN** continuous integration builds a branch
- **THEN** developers on that branch SHALL fetch its artefacts rather than rebuilding

### Requirement: Build service
Build execution SHALL be provided by a **build service** that owns the dependency graph, watches
source files, maintains the cache, and schedules workers.

The editor and the command-line tools SHALL both be **clients** of that service, so that build state
survives an editor restart, is shared between tools, and does not have to be rebuilt per process.

Communication SHALL be a **structured protocol** — job started, progress, diagnostic, artefact
ready, job completed — and SHALL NOT be shell invocation with output parsing.

The service SHALL support cancellation: cancelling a build SHALL stop unstarted and cancellable work
while completed artefacts remain valid in the cache.

#### Scenario: Editor stays responsive
- **WHEN** a large cook runs
- **THEN** the editor SHALL remain interactive and SHALL show structured progress

#### Scenario: Cancellation preserves work
- **WHEN** a build is cancelled midway
- **THEN** artefacts already produced SHALL remain in the cache and SHALL not be rebuilt next time

#### Scenario: CLI and editor share state
- **WHEN** a command-line build follows an editor build
- **THEN** it SHALL reuse the same graph and cache rather than starting from nothing

### Requirement: Precise invalidation
The service SHALL watch declared inputs and invalidate only the nodes that depend on what changed,
propagating through the graph.

Invalidation SHALL be **content-based**, not timestamp-based: a file rewritten with identical content
SHALL NOT invalidate anything.

The tooling SHALL be able to explain an invalidation: which input changed and which nodes it
reached.

#### Scenario: Touching a file changes nothing
- **WHEN** a source file's timestamp changes but its content does not
- **THEN** nothing SHALL rebuild

#### Scenario: A material edit is scoped
- **WHEN** one material changes
- **THEN** only its programs, dependent pipeline states, and packages containing them SHALL rebuild

### Requirement: Structured diagnostics
Every diagnostic from every stage — compiler, importer, cooker, shader compiler, validator,
packager — SHALL share a structure carrying: severity, message, a stable diagnostic code, and a
location naming a source file and position, an asset, or a graph node.

The editor SHALL be able to navigate directly from a diagnostic to what it refers to.

Diagnostics SHALL be machine-readable so that continuous integration can summarise and compare them
between builds.

#### Scenario: Click to the cause
- **WHEN** a cook reports an error on an asset
- **THEN** selecting the diagnostic SHALL open that asset at the relevant place

#### Scenario: New warnings are detectable
- **WHEN** a build introduces warnings
- **THEN** continuous integration SHALL be able to diff diagnostics against the previous build by
  code and location

### Requirement: Distributed execution
The build service SHALL support executing nodes on **remote workers** — other machines on a network
or in a cloud pool — in addition to local ones.

Because nodes have explicit inputs, deterministic keys, and immutable outputs, remote execution SHALL
require no additional correctness mechanism: inputs are fetched by hash and outputs stored by hash.

Distribution SHALL be optional and SHALL degrade to local execution when no workers are available.

Node kinds SHALL declare whether they are distributable, since some tools are licensed or
platform-bound.

#### Scenario: Shader compilation scales out
- **WHEN** a project has hundreds of thousands of shader variants
- **THEN** their compilation SHALL be distributable across workers

#### Scenario: No workers, no failure
- **WHEN** no remote workers are reachable
- **THEN** the build SHALL execute locally with no change in result

### Requirement: Packages and install bundles
Cooked content SHALL be packaged into containers, and logical asset identity SHALL remain
independent of physical package placement.

A shipping product SHALL NOT be distributed as one file per asset.

Content SHALL be assignable to **install bundles** — base, campaign, multiplayer, high-resolution
textures, per-language content, per-region content — so that platforms can install or download
subsets.

Bundle assignment SHALL be a declared policy, and the build SHALL report each bundle's size and
contents.

#### Scenario: Partial install
- **WHEN** a platform supports installing a subset
- **THEN** the base bundle SHALL be playable without optional bundles present

#### Scenario: Bundle sizes are visible
- **WHEN** a build completes
- **THEN** each bundle's size and top contributors SHALL be reported

### Requirement: Chunk-level patching
Patching SHALL operate on **content-addressed chunks**, not whole packages. A patch SHALL transfer
only chunks whose hashes changed, plus a manifest.

The engine's page-oriented formats — virtual texture pages, geometry pages, world cell payloads,
shader binaries — are already content-addressed, and patch granularity SHALL follow from that rather
than from a separate delta mechanism.

Binary delta encoding of individual changed chunks MAY be applied where measurement shows it
beneficial, and SHALL NOT be assumed.

A patch manifest SHALL record: the build it produces, the builds it may be applied to, added and
removed chunks with hashes and sizes, and bundle membership.

#### Scenario: A small change is a small patch
- **WHEN** one texture changes in a large game
- **THEN** the patch SHALL contain the affected pages and a manifest, not the containing package

#### Scenario: Applicability is explicit
- **WHEN** a patch is offered
- **THEN** its manifest SHALL state which builds it applies to

### Requirement: Patch application is staged and atomic
Patch application SHALL: download required chunks, **verify** them against their hashes, stage the
new manifest, and switch atomically.

A failed or interrupted patch SHALL leave the previous build intact and playable. Content SHALL
NEVER be overwritten destructively in place.

Verification failure SHALL abort the patch and report which chunk failed.

#### Scenario: Interrupted patch is harmless
- **WHEN** patching is interrupted by a crash or power loss
- **THEN** the previous build SHALL remain playable

#### Scenario: Corrupt download is caught
- **WHEN** a downloaded chunk fails verification
- **THEN** the patch SHALL abort and report the chunk rather than installing it

### Requirement: Manifest integrity and content protection
A shipping content manifest SHALL be **signed**, and the runtime SHALL be able to verify it where a
product requires it — for downloadable content, patches, and remotely delivered content in
particular.

Where content is encrypted, encryption SHALL be applied **after** compression, and the engine SHALL
use established cryptographic implementations and platform facilities rather than bespoke ones.

Content protection SHALL NOT be described as anti-tamper or anti-cheat, which are different problems
with different threat models.

#### Scenario: Downloadable content is verified
- **WHEN** downloadable content is mounted
- **THEN** its manifest signature SHALL be verified before its assets are made available

#### Scenario: No invented cryptography
- **WHEN** encryption or signing is implemented
- **THEN** established implementations SHALL be used

### Requirement: Content compatibility
A content manifest SHALL declare the versions it was produced against: engine content version,
schema version, renderer profile, shader interchange version, and the identity of the project and
plugin set.

The runtime SHALL refuse content produced against an incompatible version rather than
misinterpreting it.

**Shipping cooked content SHALL NOT be migrated at runtime.** Migration is an authoring-time
mechanism for tagged data; cooked content is recooked instead.

#### Scenario: Incompatible content is refused
- **WHEN** downloadable content built against an older engine is mounted
- **THEN** it SHALL be rejected with a diagnostic rather than partially loaded

#### Scenario: No runtime migration of cooked data
- **WHEN** cooked content's schema differs from the runtime's
- **THEN** the load SHALL fail; the content SHALL be recooked rather than migrated in place

### Requirement: Downloadable content and mounting
Downloadable content SHALL be a signed package set with a declared dependency on a base build,
mounted at runtime to extend the asset registry.

Mounting SHALL resolve by **stable asset identity**, so downloadable content extends the project
without path collisions.

Override of existing assets by mounted content SHALL be **opt-in per project** and, where allowed,
governed by a declared mount priority. Arbitrary global replacement SHALL NOT be the default.

Unmounting SHALL be supported and SHALL leave the base content unaffected.

#### Scenario: Content extends without collision
- **WHEN** downloadable content adds assets
- **THEN** they SHALL be addressed by identity and SHALL NOT conflict with base content

#### Scenario: Override is deliberate
- **WHEN** mounted content would replace a base asset
- **THEN** it SHALL do so only where the project has enabled override, according to mount priority

### Requirement: Pre-build validation
Before a shipping build is produced, the pipeline SHALL validate and SHALL fail on: missing or
broken asset references, unresolved prefab override conflicts, duplicate persistent identities,
content referencing editor-only data, non-shipping modules reachable from a shipping target, plugin
version incompatibilities, package dependency cycles, uncooked assets referenced by cooked content,
and shaders unsupported on a target profile.

Each check SHALL be individually configurable as an error or a warning, and errors SHALL fail
continuous integration.

Validation SHALL run **before** packaging, so a problem is not discovered after producing tens of
gigabytes.

#### Scenario: Editor data does not ship
- **WHEN** cooked content references editor-only data
- **THEN** validation SHALL fail naming the reference chain

#### Scenario: Problems are found early
- **WHEN** a shipping build has a broken reference
- **THEN** it SHALL fail during validation rather than after packaging

### Requirement: Content audit
The build SHALL answer two questions about any asset, from the dependency graph:

- **Why is this in the build?** — the reference chain from a declared root
- **What references this?** — its dependents

The build SHALL report **size by category, asset, plugin, world region, and install bundle**, with
drill-down, so that growth is attributable.

The build SHALL report **cook and compile time by stage** together with cache hit rates, so that a
slow incremental build is diagnosable rather than accepted.

#### Scenario: Tracing an unexpected inclusion
- **WHEN** a large asset appears in a build unexpectedly
- **THEN** the audit SHALL show the reference chain that pulled it in

#### Scenario: A slow incremental build is explained
- **WHEN** an incremental build takes far longer than expected
- **THEN** the report SHALL show which stages ran and why their cache keys missed

### Requirement: Build provenance and symbols
Every produced build SHALL record **provenance**: a build identity, the engine and project source
revisions, the plugin lockfile hash, the build and cook configuration, toolchain versions, and the
content manifest hash.

Shipping binaries SHALL be stripped, with **symbols archived separately** and retrievable by build
identity, so a crash report carrying that identity can be symbolicated.

Continuous integration SHALL archive a reproducibility bundle — manifests, lockfile, configuration,
artefact hashes, and symbols — so a release can be investigated and reproduced later.

#### Scenario: A crash months later
- **WHEN** a crash report from a shipped build arrives
- **THEN** its build identity SHALL locate the archived symbols and configuration needed to
  interpret it

#### Scenario: Reproducing a release
- **WHEN** a shipped build must be reproduced
- **THEN** the archived provenance SHALL identify every input required
