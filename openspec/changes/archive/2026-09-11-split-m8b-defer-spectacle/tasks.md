# Tasks: defer VFX, cinematics and inference to M8.c

Applied as it was written, because the deferral had to land before M8.b was relaunched — a milestone
built against a scope it no longer has is the thing this change exists to prevent.

- [x] 1.1 `m8c` added to `record.MILESTONES` in position, between `m8b` and `m9`
- [x] 1.2 The rung test extended: `m8b` sits between `m8a` and `m8c`, and `m8c` between `m8b` and
      `m9`, with `criteria.rung` increasing across all four
- [x] 1.3 All three milestone readers admit `M8.c` — the matrix column header, the ROADMAP section
      heading and the load-table row label. The M8 split taught this the hard way: three patterns for
      one grammar, and a heading none of them recognised failed silently in all three
- [x] 2.1 `docs/ROADMAP.md`: M8.b's work table loses three rows, its artefact loses the cinematic and
      the particles, and its determinism-firewall criterion moves out. A new **M8.c · Spectacle**
      section carries all four
- [x] 2.2 `docs/roadmap/capability-matrix.md`: an M8.c column, the three rows moved into it, and the
      load summary split
- [x] 2.3 `docs/roadmap/dependencies.md`: an M8.c subgraph, with `visual-scripting` still feeding
      both consumers across the boundary — deferring them does not detach them
- [x] 2.4 The plan-consistency checks run over all four documents: **0 document problems, 0
      dependency problems**
- [x] 3.1 `implement-m8b-systems` re-planned: section 1 marked done with the spike's finding, section
      2 rewritten as the three layers the spike prescribed with its anchor digests, sections 8 and 10
      reduced, and the artefact's two words removed
- [x] 3.2 Open the M8.c change. Deliberately left for M8.b's closing gate, which opens the next rung
      as its own task — opening it here would mean two changes claiming the same rung, and the M8
      split already had to reconcile exactly that
