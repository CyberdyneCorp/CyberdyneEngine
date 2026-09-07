# `deps/`

Three records, one per kind of dependency, and between them every piece of third-party software the
project depends on:

| File | What it records | Read by |
|---|---|---|
| `manifest.toml` | code the build fetches and links | `cmake/dependencies.cmake`, `tools/deps/manifest.py` |
| `host-tools.toml` | software the machine must already provide and the build never links | `tools/deps/manifest.py`, `just env-doctor` |
| `rust-crates.toml` | the crates Cargo acquires for the editor | `tools/deps/rust_crates.py` |

`tools/deps/attribution.py` generates `THIRD_PARTY.md` from all three, and
`just maintenance-deps-check` fails when the document is stale or a record has drifted from what the
build actually acquires. It runs in CI on every pull request, and `tools/deps/selftest.py` — which
the same recipe runs — proves it can still fail, against fixtures.

`manifest.toml` is the single source of truth for third-party dependencies the build fetches.
Each entry records name, version, exact commit, licence and licence file, upstream URL, the
engine-owned interface it sits behind, the feature option that gates it, whether a system copy is
acceptable, what links it, and one line of justification. The file's own header documents every
field and the format's restrictions.

- `cmake/dependencies.cmake` reads it to drive `FetchContent` with pinned commits — never a branch
  and never a tag, because a tag can be moved.
- `tools/deps/attribution.py` generates `THIRD_PARTY.md` from it; `just maintenance-deps-check`
  fails when that file is stale, and CI runs it.
- Disabling a feature means its dependency is not fetched, not built and not linked.
  `just maintenance-deps-test` proves it rather than asserting it.

## The editor's crates

`rust-crates.toml` is generated from `editor/Cargo.lock` by `just maintenance-deps-rust`, and adds
the two things Cargo does not record: a licence identifier and a justification. The eleven crates
the editor chose carry an argument written by a person; the rest name the chosen crates that reach
them, which is the transitive cost `thirdparty-dependencies` requires to be part of the decision.
Cargo.lock stays the pin — the record's versions and checksums are checked against it rather than
maintained beside it, so the two cannot disagree without the gate saying so.

It exists because it did not: M5.5 brought 386 crates into the tree and
`just maintenance-deps-check` compared `THIRD_PARTY.md` against the two files above alone, so it
reported green while every one of them was undeclared.

## Reaching a dependency from engine code

Engine code links `cy::dep::<name>` and never the upstream target. That indirection is where a
system copy and a fetched copy become the same thing, and it is the seam a replacement is made at —
the dependency policy's requirement that a library be isolated behind an engine-owned interface
starts at the build graph.

## Fetching, caching, and working offline

Every commit is pinned, so there is nothing to update once a source is present. Set `CY_DEPS_CACHE`
to share one download cache between build trees; set `FETCHCONTENT_FULLY_DISCONNECTED=ON` to turn a
would-be download into a configure error, which is how the offline claim is tested rather than
assumed.

The fetch is a shallow clone of the pinned commit. That needs a server willing to serve an arbitrary
commit, which GitHub is; a mirror that is not will fail loudly, and the fix is `GIT_SHALLOW FALSE`.

## Vendoring

Dependencies are not vendored at M0. Vendoring is a policy the specification permits for
dependencies requiring engine-specific patches, not one it requires — see `design.md` §6. When the
first patch is needed, it is a discrete file with a rationale, so that a later upgrade can decide
whether it is still required.

**Governed by**: `thirdparty-dependencies`, `build-system-and-platforms` (dependency management).
