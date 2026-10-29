# Tasks

## 1. Find the cause

- [x] 1.1 Read the failing Windows logs: the parallel case of `integration.material_cook`, one node
  failed in the first run and its digest differed in the second.
- [x] 1.2 Print each node's output digest: eight nodes, three distinct artefacts. Two workers store
  the same artefact at once.
- [x] 1.3 Trace `ArtefactStore::put`: the losing writer renamed over the winner's published file,
  which Windows refuses while the file is open.

## 2. Fix

- [x] 2.1 `fs::write_atomic_no_replace` (POSIX `link`, Windows `MoveFileEx` without replace).
- [x] 2.2 `ArtefactStore::put` publishes with it and reports a lost race as `AlreadyPresent`.
- [x] 2.3 A storage failure's reason reaches the node result. The material case prints the reason of
  each failed node.

## 3. Tests that fail without the fix

- [x] 3.1 `integration.assets_io`: a second no-replace write is refused and the first file stands.
- [x] 3.2 `integration.build_service`: the probe puts a rival publish and an open reader in the
  race window. The losing put answers `AlreadyPresent` and the entry is the rival's file, by inode.
- [x] 3.3 `integration.build_service`: sixteen writers of one artefact over eight rounds. Every put
  succeeds and exactly one writes.
- [x] 3.4 Red on the old publish (`evidence/falsification.txt`).
- [ ] 3.5 The Windows test leg green on repeated runs (CI only).
