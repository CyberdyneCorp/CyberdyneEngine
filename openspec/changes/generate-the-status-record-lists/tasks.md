# Tasks: generate the status record's three lists

- [x] 1.1 `tools/roadmap/record.py` renders the Complete/Working/Seed lists from the record, wrapped
      the way the document wraps, with the milestone in the heading derived from the record rather
      than typed
- [x] 1.2 `just roadmap-status` fails when the generated block in `docs/roadmap/capability-matrix.md`
      does not match, and prints what the record says
- [x] 1.3 `just roadmap-status --write-lists` rewrites the block in place
- [x] 1.4 The matrix carries the markers and the generated content, and the paragraph that recorded
      the finding now records the fix
- [x] 1.5 Prove the check can fail: change one name inside the block and watch `just roadmap-status`
      go red, then regenerate
- [ ] 1.6 Archive with the change that closes the next milestone
