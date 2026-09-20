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
