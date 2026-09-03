# just/content.just — Content: import, cook, package, patch, validate.
#
# Nothing here is implemented at M0, and not because it was skipped: there is no asset pipeline to
# drive until the importer and cooker land. The recipes exist so that the category is discoverable
# and so that the first person to need one adds behaviour rather than inventing a name.
#
# Each one names the milestone that fills it and the capability that governs it, so the listing
# answers "when" as well as "not yet". A recipe that failed with "not implemented" and no more would
# be a worse answer than no recipe at all.

# Import source assets into the engine's own formats. No asset pipeline exists before M8.
content-import *args:
    @just _not-implemented content-import 'M8 — asset-import-pipeline'

# Cook imported content for a target platform and profile. No cooker exists before M8.
content-cook *args:
    @just _not-implemented content-cook 'M8 — asset-import-pipeline'

# Validate cooked content against the schemas and budgets. Nothing to validate before M8.
content-validate *args:
    @just _not-implemented content-validate 'M8 — asset-import-pipeline'

# Package cooked content into shippable archives. No packaging exists before M12.
content-package *args:
    @just _not-implemented content-package 'M12 — build-and-packaging'

# Produce a patch between two packaged builds. No patching exists before M12.
content-patch *args:
    @just _not-implemented content-patch 'M12 — build-and-packaging'
