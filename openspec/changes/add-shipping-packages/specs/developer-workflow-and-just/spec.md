## ADDED Requirements

### Requirement: One recipe packages a game for a target
`just package <game> <target>` SHALL package a game for a desktop target from the build tree of
the host it runs on, defaulting to the release profile, and SHALL refuse a target that is not the
host's operating system by name rather than emitting the host's binaries under another target's
name. `just release-games` SHALL package every game the host built from the release profile and
SHALL refuse a tree of any other profile.

#### Scenario: Another operating system's artefact is asked for
- **WHEN** `just package rts macos` runs on Linux
- **THEN** the recipe fails saying a macOS artefact is packaged on macOS
