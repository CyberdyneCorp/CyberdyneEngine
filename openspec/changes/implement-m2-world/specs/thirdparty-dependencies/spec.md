# thirdparty-dependencies (delta)

M1's closing gate found a dependency the manifest does not know about, and the manifest's own
requirement did not oblige it to. The reflection generator parses annotated C++ with libclang: it
needs the `clang` Python bindings at a pinned version and a matching libclang shared library, and
`tools/gen/reflect/parse.py` carries a hard-coded list of paths to look for it at. Neither is in
`deps/manifest.toml` and neither appears in `THIRD_PARTY.md`, because both were read as build-time
tooling rather than as dependencies — and a clone without them configures, builds, and silently
compiles the committed metadata without ever regenerating it.

The same reading would exempt the pinned clang-format and clang-tidy the quality gates run at, and
any future shader compiler or code generator. A dependency that decides what a build produces is a
dependency whether or not it is linked, so the requirement below says which properties the manifest
records for one, and the doctor is where its absence is reported.

## MODIFIED Requirements

### Requirement: Dependency manifest
Every dependency SHALL be recorded in a single machine-readable manifest containing: name,
version, commit hash, upstream URL, licence identifier, licence file path, whether it is optional
and which feature gates it, whether a system version may be used, and a one-line justification.

The manifest SHALL be the source of truth for the build, the licence report, and the security
audit.

**Build-time tooling that determines what the build produces** — a code generator's compiler
frontend, a formatter or static analyser a merge gate runs, a shader compiler — SHALL be recorded in
the manifest too, at a pinned version, and marked as build-time so that the runtime licence report
does not list it. Its absence from a developer's machine SHALL be reported by the environment
diagnostic with the command that installs the pinned version, rather than by the silent use of a
committed artefact that was never regenerated.

#### Scenario: Licence report
- **WHEN** a game is packaged
- **THEN** a licence report SHALL be generated from the manifest, listing every dependency
  actually linked into that build, and excluding build-time tooling

#### Scenario: Dependency audit
- **WHEN** a security advisory affects a dependency
- **THEN** the manifest SHALL identify the pinned version and which features would be affected

#### Scenario: A generator's frontend is missing
- **WHEN** a clone lacks the version of a generator's compiler frontend the manifest pins
- **THEN** the environment diagnostic SHALL report it with the correction, and the generator SHALL
  fail rather than leave committed generated output unregenerated
