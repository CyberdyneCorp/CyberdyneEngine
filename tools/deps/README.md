# `tools/deps/` — layer 7

The dependency records' tooling. It reads the three files under `deps/` and writes only the one
that is generated: `deps/manifest.toml` and `deps/host-tools.toml` are edited by a human proposing a
dependency, `deps/rust-crates.toml` is regenerated from `editor/Cargo.lock`, and these scripts are
what make such an edit take effect everywhere else.

| File | What it does | Recipe |
|---|---|---|
| `manifest.py` | Loads and validates `manifest.toml` and `host-tools.toml`. The reference for what a valid entry is. | — |
| `rust_crates.py` | Loads `rust-crates.toml`, checks it against `editor/Cargo.lock` and the licence policy; `--sync` regenerates it. | `just maintenance-deps-rust` |
| `attribution.py` | Generates `THIRD_PARTY.md` from all three records; `--check` fails when it is stale. | `just maintenance-deps`, `just maintenance-deps-check` |
| `selftest.py` | Proves those gates can still fail, against fixtures — including the undeclared crate that got past them through M5.5. | `just maintenance-deps-check` |
| `test_gating.py` | Proves a disabled feature fetches, builds and links nothing (task 1.6.5). | `just maintenance-deps-test` |

Python rather than compiled targets, so nothing here appears in the C++ build graph and `tools/`
does not `add_subdirectory()` this directory.

**The manifest is parsed twice** — here with `tomllib`, and in `cmake/dependencies.cmake`, which has
no TOML parser and reads the restricted subset the manifest's header documents. The two enforce the
same rules, `manifest.py` is the reference, and a change to either belongs in both. The subset is
small deliberately: a hand-written parser is only as trustworthy as it is short.

`attribution.py`'s output depends on the three records and on nothing else — no timestamps, no
paths, no environment — so `--check` in CI answers exactly one question: has somebody changed a
record without regenerating the document?

**The Rust record is checked against a lockfile rather than validated in isolation**, because that
is the failure that actually happened: 386 crates entered the tree at M5.5 and the attribution gate
compared `THIRD_PARTY.md` against the two C and C++ manifests alone, so it stayed green while every
one of them was undeclared. `rust_crates.py` therefore refuses to publish an attribution document
until the record and `editor/Cargo.lock` name the same crates at the same versions and checksums.
Its check needs Python and a file read; only `--sync` needs `cargo`.

**Governed by**: `thirdparty-dependencies`.
