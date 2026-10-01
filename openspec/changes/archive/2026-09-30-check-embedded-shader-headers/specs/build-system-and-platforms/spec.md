## MODIFIED Requirements

### Requirement: Compiler support
The engine SHALL support: **Clang 17+**, **GCC 13+**, and **MSVC 19.38+** (Visual Studio 2022
17.8), each with full C++20 support for the features the engine uses.

The build SHALL enable a strict warning set and treat warnings as errors in CI, with a documented,
narrowly-scoped suppression mechanism for third-party headers.

#### Scenario: Warning as error
- **WHEN** a change introduces a warning in engine code
- **THEN** CI SHALL fail

#### Scenario: Third-party warnings suppressed
- **WHEN** a dependency's headers produce warnings
- **THEN** they SHALL be included as system headers so they do not fail the build

#### Scenario: A dependency's macro warns where it is expanded
- **WHEN** a dependency's macro raises a warning at its expansion site in engine code, where
  including the dependency as a system header does not reach it
- **THEN** the suppression SHALL be scoped to that macro's expansion and to compilers that know the
  warning, and the project's warning flags SHALL NOT change
