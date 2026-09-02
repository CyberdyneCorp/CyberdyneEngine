# core-assets-and-io Specification

## Purpose

Defines the asset model and I/O layer: what an asset is, stable content identity, the virtual
filesystem, synchronous and streaming loads, the runtime package format, dependency tracking,
hot reload, and compression and encryption.

The engine draws the **GUID-based asset reference** model from Unity and Unreal (references
survive file moves and renames) and the **cook-on-import, load-cooked-at-runtime** pipeline from
both, rather than parsing source assets at runtime.

## Requirements

### Requirement: Asset identity
Every asset SHALL have a stable 128-bit `AssetId`, generated once when the source file is first
imported and stored in a sidecar `.meta` file next to the source.

Serialized references SHALL use `AssetId`, never file paths, so moving or renaming a source asset
does not break references.

An asset SHALL additionally have a **content hash** of its cooked payload, used for cache
validation, incremental builds, and patch generation.

#### Scenario: Asset moved on disk
- **WHEN** a texture is moved to a different folder along with its `.meta`
- **THEN** every reference SHALL continue to resolve, with no scene or material edits

#### Scenario: Duplicated meta file
- **WHEN** two source files claim the same `AssetId`
- **THEN** the importer SHALL report a collision and refuse to register the second, rather than
  silently shadowing the first

#### Scenario: Source deleted
- **WHEN** a referenced asset no longer exists
- **THEN** loading SHALL yield a typed placeholder (missing-texture, missing-mesh) and a
  diagnostic naming the referrer, rather than failing the whole load

### Requirement: Assets are cooked, not parsed at runtime
Source assets (`.png`, `.gltf`, `.wav`, `.fbx`) SHALL be **imported** into a cooked runtime
representation stored in a content-addressed cache. The runtime SHALL load only cooked assets.

Each cooked asset SHALL begin with a header containing a magic number, format version, asset
kind, content hash, and the platform/feature variant key it was cooked for.

#### Scenario: Shipping build has no source assets
- **WHEN** a game is packaged
- **THEN** the package SHALL contain cooked assets only, and the runtime SHALL contain no source
  format parsers except those explicitly retained for runtime import

#### Scenario: Version mismatch
- **WHEN** a cooked asset's format version is newer than the runtime supports
- **THEN** loading SHALL fail with a clear diagnostic rather than misparsing

#### Scenario: Platform variants
- **WHEN** a texture is cooked for desktop (BC7) and mobile (ASTC)
- **THEN** both variants SHALL be addressable by the same `AssetId` plus a variant key, and the
  package for each platform SHALL contain only its variant

### Requirement: Virtual filesystem
`VirtualFileSystem` SHALL present a single namespace over layered mount points, resolved in
priority order:

| Mount | Purpose |
|---|---|
| Package mounts | Cooked content packages (`.cypak`), including patches |
| Project mount | The project directory, in editor and development builds |
| User mount | Writable per-user location for saves, logs, and caches |
| Memory mount | In-memory files for tests and generated content |
| Remote mount | Development-time file serving from a host machine |

Paths SHALL be normalised, case-sensitive, and forward-slash separated. Path traversal outside a
mount SHALL be rejected.

#### Scenario: Patch overrides base content
- **WHEN** a patch package is mounted above a base package and both contain the same asset
- **THEN** the patch's version SHALL be served, and an entry marked as deleted in the patch SHALL
  mask the base entry entirely

#### Scenario: Development file serving
- **WHEN** a device runs with a remote mount configured
- **THEN** assets SHALL be fetched from the host machine on demand, so iteration does not require
  repackaging

### Requirement: Package format
The runtime package format (`.cypak`) SHALL consist of a header (magic, version, flags), a
directory of entries (asset id, variant key, offset, size, uncompressed size, compression
method, content hash, flags), and the payload region.

A package SHALL carry a **manifest** declaring: the build identity that produced it, the content
compatibility versions it requires (see `build-and-packaging`), its install bundle membership, and
its dependencies on other packages.

Bulk payload SHALL be addressable as **content-addressed chunks** that MAY be shared between
packages and between builds, so that patching transfers only changed chunks and duplicated content
is stored once.

Package flags SHALL support: encrypted directory, encrypted payload, deleted-entry markers for
patches, and chunk alignment for memory mapping. Where encryption is used it SHALL be applied after
compression.

Payload compression SHALL be per-entry and selectable: none, LZ4 (fast), or Zstd (dense), with
seekable framing so partial reads do not decompress the whole entry. Already-compressed data such as
block-compressed texture blocks SHALL NOT be recompressed by default.

#### Scenario: Memory-mapped read
- **WHEN** an uncompressed entry is aligned and the platform supports mapping
- **THEN** it SHALL be memory-mapped rather than copied into a buffer

#### Scenario: Partial read of a large asset
- **WHEN** a streaming system requests one mip level of a large texture
- **THEN** only the containing compression frames SHALL be read and decompressed

#### Scenario: Shared chunks deduplicate
- **WHEN** identical content appears in two packages
- **THEN** it SHALL be stored once as a shared content-addressed chunk

#### Scenario: Incompatible package is refused
- **WHEN** a package's declared compatibility versions do not match the runtime
- **THEN** mounting SHALL fail with a diagnostic rather than partially loading

### Requirement: Asset loading
`AssetSystem` SHALL provide:

- `load<T>(id)` — blocking load, returning `Expected<Ref<T>, Error>`
- `load_async<T>(id)` — returns a `LoadRequest` pollable for progress and completion
- `load_batch(ids)` — a single request for many assets, so dependency graphs load coherently
- `preload(ids)` / `release(ids)` — explicit residency control
- reference counting via `Ref<T>`, with a configurable retention policy (immediate,
  time-delayed, or budget-based eviction)

Loads SHALL execute on the asset I/O thread and job workers: read, decompress, deserialize, and
upload (for GPU resources) are separate stages that can overlap across requests.

#### Scenario: Dependencies load with the parent
- **WHEN** a material referencing three textures is loaded
- **THEN** the dependency graph SHALL be resolved and all four assets loaded before the request
  reports completion

#### Scenario: Duplicate request is coalesced
- **WHEN** two systems request the same asset concurrently
- **THEN** one load SHALL be performed and both requests SHALL receive the same `Ref`

#### Scenario: Cancellation
- **WHEN** a pending load is cancelled (level unloaded before it completed)
- **THEN** the request SHALL stop at the next stage boundary and release any partial results

### Requirement: Streaming
The engine SHALL support partial residency for assets that declare it — textures (mip levels),
meshes (LOD levels), and audio (streamed samples) — driven by a residency budget and per-asset
priority derived from renderer feedback and distance.

Streaming SHALL never block the frame: a not-yet-resident level SHALL fall back to the highest
resident level.

#### Scenario: Approaching a surface
- **WHEN** the camera approaches a textured surface and feedback requests a higher mip
- **THEN** the mip SHALL be scheduled for load and swapped in when ready, with the lower mip used
  meanwhile

#### Scenario: Budget exceeded
- **WHEN** the residency budget is exceeded
- **THEN** the least recently requested levels SHALL be evicted first, and the eviction SHALL be
  recorded in streaming statistics

### Requirement: Hot reload
In development builds, the asset system SHALL watch source files and cooked outputs and reload
changed assets in place, preserving existing `Ref`s.

Reload SHALL notify dependents so derived state (GPU uploads, material instances, shader
pipelines) is rebuilt.

#### Scenario: Texture edited while running
- **WHEN** a texture's source file changes in the editor
- **THEN** it SHALL be re-imported and the runtime asset updated in place, with materials
  referencing it showing the new content without a restart

#### Scenario: Reload failure keeps the old asset
- **WHEN** re-import fails (malformed file mid-write)
- **THEN** the previously loaded asset SHALL remain in use and a diagnostic SHALL be logged

### Requirement: File and directory access
`FileSystem` SHALL provide stream-based file access with `read`, `write`, `seek`, `size`,
`flush`, and memory mapping, plus directory enumeration, creation, move, copy, and delete.

Writes to the user mount SHALL be atomic where the platform allows: write to a temporary file
then rename, so an interrupted save cannot corrupt existing data.

#### Scenario: Interrupted save
- **WHEN** the process is killed during a save
- **THEN** the previous file SHALL remain intact and the temporary file SHALL be discarded on
  next start

### Requirement: Serialization formats
The engine SHALL support two serialization forms for the same reflected data
(see `serialization-and-prefabs` for the scene and prefab model):

- **Binary** — the runtime format: compact, versioned, endian-defined (little), suitable for
  memory mapping
- **Text** — a human-readable, diff-friendly format used for scenes, prefabs, and project
  configuration in source control

Both SHALL round-trip: text → binary → text SHALL preserve values and ordering.

#### Scenario: Meaningful version control diffs
- **WHEN** a designer moves one entity in a scene
- **THEN** the text diff SHALL show only that entity's transform change, with stable ordering
  elsewhere

### Requirement: Compression and cryptography
`core/compression` SHALL provide LZ4, Zstd, and Deflate. `core/crypto` SHALL provide SHA-256,
BLAKE3, HMAC, AES-GCM, and a CSPRNG, backed by a vetted third-party implementation rather than
hand-rolled primitives.

Package encryption SHALL be an option, with the documented caveat that a key shipped with a
client is obfuscation, not security.

#### Scenario: Content hash
- **WHEN** a cooked asset is written
- **THEN** its BLAKE3 content hash SHALL be recorded for cache validation and patch diffing

#### Scenario: Tampered package
- **WHEN** an entry's payload does not match its recorded hash and verification is enabled
- **THEN** the load SHALL fail with an integrity error
