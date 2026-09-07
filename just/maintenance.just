# just/maintenance.just — Maintenance: clean, caches, dependency update.
#
# Destructive recipes here name exactly what they will remove and confirm before doing it, unless
# invoked with --yes for non-interactive use.
#
# THE THREE DEPENDENCY RECORDS, AND WHICH RECIPE OWNS WHICH. `thirdparty-dependencies` asks for one
# machine-readable manifest per kind of dependency, and there are three kinds:
#
#   deps/manifest.toml     code the build fetches and links. cmake/dependencies.cmake drives
#                          FetchContent from it.
#   deps/host-tools.toml   software the machine must already provide and the build never links: the
#                          reflection generator's C++ frontend, and the Rust toolchain.
#   deps/rust-crates.toml  the crates Cargo acquires for the editor, checked against
#                          editor/Cargo.lock.
#
# `maintenance-deps` regenerates THIRD_PARTY.md from all three and needs nothing installed.
# `maintenance-deps-rust` regenerates the third from the lockfile and needs cargo.
# `maintenance-deps-check` is the merge gate over all of it.

# Remove build output. Names what it will remove and confirms; --yes for CI.
maintenance-clean *args:
    @just _not-implemented maintenance-clean 2.1.5

# Regenerate THIRD_PARTY.md from the three dependency records.
maintenance-deps:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    python3 tools/deps/attribution.py

# Regenerate deps/rust-crates.toml from editor/Cargo.lock. Needs cargo and a populated registry
# cache; run it after any change to the editor's dependencies, then `just maintenance-deps`.
maintenance-deps-rust:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    python3 tools/deps/rust_crates.py --sync
    python3 tools/deps/attribution.py

# Fail if THIRD_PARTY.md no longer matches the dependency records, or if a record no longer matches
# what the build acquires. Run by CI on every pull request.
#
# TWO CHECKS, AND THE SECOND IS WHY THE FIRST IS TRUSTWORTHY. Until M6 this recipe compared
# THIRD_PARTY.md against the two C and C++ manifests and nothing else, so it reported green through
# a milestone in which the editor acquired 386 undeclared Rust crates — the document was current for
# a dependency set that was no longer the one being built. attribution.py now reads
# deps/rust-crates.toml as well and refuses to publish it until it agrees with editor/Cargo.lock,
# so a crate Cargo acquires and nobody declared fails here.
#
# The selftest runs first, and it is the same arrangement as `quality-layers`: the gate's own
# failures are proved by fixtures rather than asserted, because a gate that has quietly stopped
# firing looks exactly like a tree with nothing wrong in it. It costs a fraction of a second.
maintenance-deps-check:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    python3 tools/deps/selftest.py
    python3 tools/deps/attribution.py --check

# Prove a disabled feature fetches, builds and links nothing. Configures twice; minutes on a cold cache.
maintenance-deps-test *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    python3 tools/deps/test_gating.py {{args}}
