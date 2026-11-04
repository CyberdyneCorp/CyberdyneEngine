## ADDED Requirements

### Requirement: A packaged game verifies its installation before it starts
A packaged game SHALL carry a manifest listing every file of its artefact by size and
cryptographic digest, with its cooked content split into chunks the manifest also pins, and the
manifest SHALL be signed with an established signature scheme. At startup the game SHALL verify
the signature against the one key its build trusts before reading anything the manifest says, then
verify every listed file, and SHALL refuse to start — naming each file — when a file is missing,
modified, or present without being listed, or when the signature does not hold or is by another
key. A build configured to trust no key SHALL say that it trusts the published development key.

#### Scenario: A tampered chunk is refused
- **WHEN** one byte of a content chunk is changed after packaging
- **THEN** the game does not start, exits with the refusal status, and names the chunk

#### Scenario: A missing file is reported by name
- **WHEN** a listed file is deleted from the artefact
- **THEN** the refusal names that file, and every other missing file with it

#### Scenario: A file the manifest does not list is refused
- **WHEN** a library is placed beside the executable after packaging
- **THEN** the refusal names it as not listed

#### Scenario: A manifest changed after signing is refused
- **WHEN** a manifest line is edited after it was signed
- **THEN** the signature check fails and no listed file is read

### Requirement: Distributable game artefacts
The packaging recipe SHALL produce one self-contained artefact per game and desktop target: on
Linux a tarball whose executable is stripped with its symbols archived beside it; on Windows a zip
holding the executable and its libraries; on macOS a code-signed application bundle. Each artefact
SHALL hold the game's cooked content, its Swift module and every library the module loads that the
target does not provide, the licences, and the version and changelog the release recipes produce.
A shipped game SHALL keep its settings, crash reports and unpacked content in the user's
directories, never beside its executable.

#### Scenario: A packaged game runs with no source tree
- **WHEN** the Linux artefact is unpacked into an empty temporary directory and launched there
- **THEN** the scripted run ends in the same digests as the build tree's run
- **AND** the run opens no file under the source or build tree

#### Scenario: A crash is reported where a player can find it
- **WHEN** a packaged game crashes after its launch
- **THEN** a crash report naming the signal is written to the user's crash directory
