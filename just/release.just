# just/release.just — Release: version, changelog, artefacts, publication.
#
# The recipes are thin wrappers over tools/release/release.py. CMake and Cargo remain the build
# systems and cy_build remains the project package engine; this layer selects the public outputs,
# records their provenance and creates deterministic component archives.

# Report or set the engine and editor version together.
release-version *args:
    @python3 "{{root}}/tools/release/release.py" version {{args}}

# Assemble the changelog for a version from first-parent Git history.
release-changelog *args:
    @python3 "{{root}}/tools/release/release.py" changelog {{args}}

# Build and assemble versioned editor, runtime, SDK, tools and template archives for this platform.
release-artefacts *args:
    @python3 "{{root}}/tools/release/release.py" artifacts {{args}}

# Verify a release and print its draft GitHub publication; --execute requires --confirm v<version>.
release-publish *args:
    @python3 "{{root}}/tools/release/release.py" publish {{args}}

# Build a complete release in a temporary directory and verify its dry-run publication.
release-check:
    @python3 "{{root}}/tools/release/release.py" check

# Test release versioning, reproducible archives, verification and safe publication defaults.
release-test:
    @python3 "{{root}}/tools/release/test_release.py"

# --- Distributable games (issue #87) --------------------------------------------------------------
#
# `just package <game> <target>` turns a played game into an artefact a person downloads: the
# executable, the libraries it loads that the target does not carry, its cooked content cut into
# chunks, a signed manifest listing every file, the licences, the version and the changelog
# `release-changelog` writes. tools/ship/package.py does the work and its docstring says what each
# target's artefact holds; docs/guides/shipping.md is the guide. The release profile is the default
# because that is what ships; `--profile dev` packages the tree a developer already has, and the
# manifest says which profile it was either way. A target is packaged on its own operating system:
# `macos` on a Mac, `windows` on Windows, and asking for another is refused by name.

# Package a game as a signed, self-contained artefact. Usage: just package <rts|fps|tps|ship|all> <linux|windows|macos|host> [--profile <p>] [--key <file>] [--out <dir>]
package game target *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    # The release profile unless the arguments name another: a later --profile wins in _split.
    IFS=$'\t' read -r profile rest <<< "$(just _split --profile release {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    just build-engine --profile "${profile}"
    echo "==> package     {{game}}  target={{target}}  profile=${profile}"
    python3 tools/ship/package.py --game "{{game}}" --target "{{target}}" \
        --build-dir "${CY_BUILD_DIR:-build/${profile}}" ${rest}

# Every game this host built, packaged from the release profile into dist/releases/<version>/games,
# and refused when the tree is not the release profile.
# Package every game for this host as a release. Usage: just release-games [--key <file>]
release-games *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    version="$(python3 tools/release/release.py version | awk '/^engine/ { print $2 }')"
    just package all host --profile release --require-shipping --out "dist/releases/${version}/games" {{args}}

# smoke.ship_games by itself: each game this tree built, packaged, unpacked into a clean directory
# and launched there — its digests held to the build tree's, a tampered chunk refused, a crash
# reported. tools/ship/test_ship_games.py says what each act checks.
# Launch every packaged game from a clean directory. Usage: just package-test [--profile <p>]
package-test *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just build-engine --profile "${profile}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    python3 tools/ship/test_ship_games.py --build-dir "${build_dir}" \
        --work "${build_dir}/tools/ship/ship-games" ${rest}
