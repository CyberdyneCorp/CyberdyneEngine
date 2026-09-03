# `tools/deps/` — layer 7

The dependency manifest's tooling. Everything here reads `deps/manifest.toml` and nothing here
writes it: the manifest is edited by a human proposing a dependency, and these scripts are what make
that edit take effect everywhere else.

| File | What it does | Recipe |
|---|---|---|
| `manifest.py` | Loads and validates the manifest. The reference for what a valid entry is. | — |
| `attribution.py` | Generates `THIRD_PARTY.md`; `--check` fails when it is stale. | `just maintenance-deps`, `just maintenance-deps-check` |
| `test_gating.py` | Proves a disabled feature fetches, builds and links nothing (task 1.6.5). | `just maintenance-deps-test` |

Python rather than compiled targets, so nothing here appears in the C++ build graph and `tools/`
does not `add_subdirectory()` this directory.

**The manifest is parsed twice** — here with `tomllib`, and in `cmake/dependencies.cmake`, which has
no TOML parser and reads the restricted subset the manifest's header documents. The two enforce the
same rules, `manifest.py` is the reference, and a change to either belongs in both. The subset is
small deliberately: a hand-written parser is only as trustworthy as it is short.

`attribution.py`'s output depends on the manifest and on nothing else — no timestamps, no paths, no
environment — so `--check` in CI answers exactly one question: has somebody changed the manifest
without regenerating the document?

**Governed by**: `thirdparty-dependencies`.
