# Hand the CI build tree to the jobs that test it

## Why

The windows-x86_64 `test` job used about 59 minutes of its 60-minute timeout on a cold build
(issue #85). Run 37730586551 shows where the time went. The build job's cache entry had been
evicted, so the test job configured for 4 minutes and rebuilt the engine for 36 minutes. The editor
build took 8 minutes and the tests 4. Even on an exact cache hit (runs 37779466929, 37548099658 and
37697927753), the first `just build-engine` inside `just test-all` ran 4 400 to 4 500 Ninja steps,
which took 14 to 17 minutes. A fresh checkout gives every source a later time than every restored
object, so Ninja rebuilt everything. linux-x86_64 had the same problem, with its `test` job at 52 to
55 minutes.

Evictions happen because of the cache keys. Twelve build-tree caches had a key containing
`github.sha`, so every push saved a new entry of 3.6 to 5 GB per job. GitHub caps a repository's
caches at 10 GB, and the repository was using 15.6 GB in four entries. A test leg's cache entry
could be evicted before the leg restored it.

## What Changes

- `just ci-build-tree-pack` writes the commit into the build tree and packs the tree as
  `build-tree.tar.zst`, a tar archive compressed with zstd. The artifact store's zip format would
  lose file modification times. Packing refuses a tree built over modified tracked files.
- `just ci-build-tree-unpack` unpacks the tree and refuses it if it was built at another commit or
  if tracked files have changed. It then backdates every tracked file to 2000-01-01, so Ninja treats
  the tree as current.
- Each `build` leg uploads its tree as the artifact `build-tree-<label>` with a one-day retention.
  The four `test` legs and the Linux jobs that need the build (`world`, `render`, `playable`,
  `authorable`, `scale`, `agent`) download and unpack it instead of using the cache. Each of these
  jobs builds the editor first, because its Cargo cache can come from an older commit.
- The build-tree and editor cache keys no longer include the commit. `cross-leg-publish`,
  `quality`, `generated` and `identity` only restore the build leg's entry and never save one.
- `check_workflows.py` checks this structure, with negative fixtures in `--selftest`.

## Impact

- Affected specs: `developer-workflow-and-just`.
- Affected code: `.github/workflows/ci.yml`, `just/ci.just`, `tools/ci/check_workflows.py`,
  `tools/ci/test_recipes.py`, `docs/guides/building.md`, `tools/ci/README.md`.
