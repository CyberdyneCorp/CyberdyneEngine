## ADDED Requirements

### Requirement: Game module builds survive a crashing toolchain
The game-module build driver SHALL choose SwiftPM's build system rather than inherit the
toolchain's default, and SHALL run a Swift command again, a bounded number of times and with the
reason logged, when a crash signal ended it. A command that exited with an error SHALL NOT be
retried, and neither SHALL one stopped by SIGINT, SIGTERM or SIGKILL. Continuous integration SHALL
build game modules with the Swift version the repository pins, in every job that builds them on a
platform where the version can be installed.

#### Scenario: The toolchain crashes while planning
- **WHEN** `swift build` dies of SIGSEGV before compiling the module
- **THEN** the driver SHALL run it again and say on stderr that it did so and why, and SHALL stop
  after the bound

#### Scenario: The module does not compile
- **WHEN** `swift build` exits non-zero because the sources do not compile
- **THEN** the driver SHALL report the failure without running the build again

#### Scenario: A job builds with an unpinned Swift
- **WHEN** a workflow job builds the tree on Linux x86_64 without installing the pinned Swift, or
  installs another version
- **THEN** `just ci-check` SHALL fail and name the job
