## REMOVED Requirements

### Requirement: Pipeline compilation strategy
**Reason**: Superseded by **Pipeline state object management** in `shader-system`.

Pipeline state compilation is not specific to the Forward+ pipeline. The visibility buffer
pipeline, the mobile pipeline, and any project-supplied pipeline need identical behaviour, and a
first-use compilation hitch is not a Forward+ problem.

Nothing is lost: the successor requirement retains permutation collection, the cooked pipeline
manifest, cache warming at load with progress reporting, and the fallback pipeline used while a
missing permutation compiles asynchronously — including both original scenarios — and extends
them with a tiered derived-data cache and a distributed compilation service.
