## ADDED Requirements

### Requirement: Continuous integration tests the build it made
A continuous integration job that tests a build SHALL use the tree that the run's build job
produced. It SHALL NOT rebuild that tree. The tree SHALL reach the job as an artifact of the run,
not as a repository cache entry, because other entries can evict a cache entry before the job reads
it.

The tree SHALL record the commit it was built from. A job SHALL refuse a tree built at a different
commit, and SHALL refuse a tree when tracked files have changed since checkout. Only after both
checks pass SHALL a job make the tree's outputs current against the checkout. Making outputs current
SHALL happen after the job builds anything restored from an older cache.

Cache keys for build trees SHALL depend on the inputs that make the cached work reusable. They SHALL
NOT include the commit, so a push does not save a new entry. A job that reuses another job's entry
SHALL restore it without saving one.

#### Scenario: The cache entry was evicted
- **WHEN** the repository cache no longer holds the build job's entry by the time a test job starts
- **THEN** the test job SHALL still test the tree the build job built, without compiling it again

#### Scenario: A tree from another commit
- **WHEN** a job is given a build tree that records a different commit from its checkout
- **THEN** it SHALL refuse the tree, say which commit built it, and leave every file time unchanged

#### Scenario: The workflow loses the hand-off
- **WHEN** a change makes a job that needs the build restore `build/dev` from the cache, or build
  the editor after unpacking the tree
- **THEN** `just ci-check` SHALL fail and name the job
