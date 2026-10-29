# Fix the artefact store's publish race: a second writer of one artefact never replaces it

## Why

`integration.material_cook` failed intermittently on the Windows test leg (job 122336336549 on
main, job 122918862100 on #136): in the parallel case, eight material nodes on four workers,
`report.failed` was 1 and one node's digest differed from the first run's.

The eight nodes produce only three distinct bundles (two copies of `worn_metal`, and the six
variants alternate between two roughness values), so workers routinely store the same artefact at
the same moment. `ArtefactStore::put` checked that the entry was absent, wrote a temporary, and
renamed it over the entry with `fs::write_atomic`. Both workers passed the check; the first
published the entry and the second renamed over it. On POSIX that replace always succeeds. On
Windows, `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` fails with access denied while anyone has the
destination open: the other worker's rename still holding its handle, a reader, or a virus scanner.
The losing node reported "the artefact could not be stored" for an artefact that was in the store.

## What changes

- `fs::write_atomic_no_replace`: the same temporary-then-publish as `write_atomic`, but the publish
  never replaces. `link` then unlink on POSIX, and `MoveFileEx` without
  `MOVEFILE_REPLACE_EXISTING` on Windows. It answers `AlreadyExists` and leaves the existing file
  untouched. On a POSIX filesystem without hard links it falls back to a check and then a rename.
- `ArtefactStore::put` publishes with it. A writer that loses the race reports `AlreadyPresent`,
  the outcome a warm rebuild already reports. The published file is never touched again.
- `ArtefactStore::set_publish_probe`: a test seam called between the existence check and the
  publish, so a test can put a rival writer, and an open reader, in exactly that window.
- A node whose artefact cannot be stored now says why. `integration.material_cook` prints each
  failed node's reason, and its comment no longer claims the eight artefacts all differ.

## Impact

- `build-and-packaging`, Artefacts are immutable: a writer that loses a race to publish an artefact
  reports it present, and never replaces it.
- `src/core/assets` (`file.h`, `file.cpp`), `tools/build` (`artefact_store.{h,cpp}`,
  `service.cpp`).
- Tests: `integration.assets_io` (the no-replace write), `integration.build_service` (the race made
  deterministic with the probe, and a sixteen-writer stampede), `integration.material_cook`.
