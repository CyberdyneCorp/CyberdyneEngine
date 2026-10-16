## ADDED Requirements

### Requirement: A ledger longer than one CI job runs in shards and merges into one verdict
When the closed milestones' exit criteria do not fit inside the time limit of one continuous
integration job, continuous integration SHALL split them across several jobs (shards) and merge
their results into one verdict. Every shard SHALL evaluate the same flat plan as an unsharded run,
narrowed to the criteria the tooling assigns it, with the same `where = "ci"` and declared-gap rules.
The assignment SHALL be a function of the tree, so that the merge can recompute it. Criteria that
share a build tree other than the default one SHOULD share a shard, so that the tree is built once.

The merge SHALL refuse a shard record that is missing, reported twice, made at another commit, made
for another milestone, shard count or `--ci` setting, or that evaluated a set of criteria other than
the one assigned to its shard. A missing shard SHALL fail the verdict, never shrink it. The merge job
SHALL run when a shard fails or times out, and each shard job SHALL set a timeout below the hosting
limit, so that an overrun is reported by the job rather than cancelled by the platform.

#### Scenario: A shard never reports
- **WHEN** one shard job is cancelled, crashes or uploads no record
- **THEN** the merge SHALL fail and name the missing shard, whatever the other shards reported

#### Scenario: The workflow loses a shard
- **WHEN** a change removes a shard from the matrix, changes the count in only one place, makes the
  merge skippable when a shard fails, or runs the full ledger in one job
- **THEN** `just ci-check` SHALL fail and name the job
