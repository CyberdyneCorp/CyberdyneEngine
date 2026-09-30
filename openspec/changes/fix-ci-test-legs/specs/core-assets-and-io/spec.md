## MODIFIED Requirements

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

#### Scenario: Development file serving from a Windows host
- **WHEN** the remote mount's host or device runs on Windows
- **THEN** it SHALL serve and fetch over WinSock with the same protocol as the POSIX transport, and
  `remote_serving_available()` SHALL answer true
