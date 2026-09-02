## MODIFIED Requirements

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
