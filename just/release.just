# just/release.just — Release: version, changelog, artefacts, publication.
#
# Nothing here is implemented at M0. The engine has no release process yet because it has nothing to
# release; the category exists so that the first release does not invent one in a shell history.
#
# When it lands, the shape is already fixed by the specifications it will have to satisfy: a release
# is a `release` profile build whose identity the diagnostics record and whose artefacts the
# packaging capability produces. None of that is a decision this milestone gets to make early.

# Report or set the engine version. No release process exists before M12.
release-version *args:
    @just _not-implemented release-version 'M12 — build-and-packaging'

# Assemble the changelog for a version from the changes that landed in it. No release before M12.
release-changelog *args:
    @just _not-implemented release-changelog 'M12 — build-and-packaging'

# Build the release artefacts for every supported platform. No release before M12.
release-artefacts *args:
    @just _not-implemented release-artefacts 'M12 — build-and-packaging'

# Publish a built release. Irreversible: it will confirm before it does anything. Not before M12.
release-publish *args:
    @just _not-implemented release-publish 'M12 — build-and-packaging'
