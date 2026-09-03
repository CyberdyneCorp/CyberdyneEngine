# Tasks: delivery roadmap

Specification-stage change. Sections 1 to 3 are the work of this change and are complete; the
change is archived on that basis.

Section 4 records the tooling the decision implies — the status record's machine checks and the
milestone recipes. None of it can exist before there is a `justfile` and a build to hang it on, so
it is **deliberately deferred to M0**, where the workflow capability is implemented. It is listed
so the scope is not lost.

## 1. Specification

- [x] 1.1 Record the rationale, the ordering argument, the cycles, the risk concentration, and the
      rejected alternatives in `design.md`
- [x] 1.2 New `delivery-roadmap` capability: the sequencing contract, vertical slices, maturity
      tiers, the twelve-milestone ladder, executable exit criteria, gate persistence,
      retrofit-hostile invariant timing, spec-derived dependency ordering, the backend and platform
      ladder, staged third-party integration, deferred re-entry points, scheduled risk, the single
      status record, roadmap change control, and forbidden roadmap patterns
- [x] 1.3 `openspec validate --strict` passes

## 2. Cross-spec consistency

- [x] 2.1 `developer-workflow-and-just` — add the **Roadmap** recipe category, with a status recipe
      that fails on drift between the record and the specification set, and a milestone recipe that
      runs a milestone's exit criteria
- [x] 2.2 `testing-and-quality` — merge gates include the exit criteria of every closed milestone,
      so a closed milestone stays closed
- [x] 2.3 `engine-architecture` — reviewed; no change needed. Its non-goals requirement already
      records deferred scope; the roadmap adds re-entry points without contradicting it
- [x] 2.4 `xr-support` — reviewed; no change needed. Its prerequisite requirements are exactly the
      seams the roadmap's deferred-scope requirement protects
- [x] 2.5 `thirdparty-dependencies` — reviewed; no change needed. The roadmap stages *when* each
      dependency enters; the policy for *whether* it may remains that capability's
- [x] 2.6 `rhi-and-render-graph` — reviewed; no change needed. Its backend roadmap requirement and
      the roadmap's backend ladder agree on Vulkan, then Metal, then D3D12
- [x] 2.7 `project-and-plugins` — reviewed; no change needed. Layering enforcement is cited as an
      M1 invariant, not redefined

## 3. Documentation

- [x] 3.1 `docs/ROADMAP.md` — the narrative view of the ladder: the four eras, each milestone's
      entry conditions, work, closing artefact and exit criteria
- [x] 3.2 `docs/roadmap/capability-matrix.md` — every capability against every milestone, with the
      tier it reaches, and the status record's initial state
- [x] 3.3 `docs/roadmap/dependencies.md` — the dependency graphs the ordering follows, and the
      three cycles with their breaks
- [x] 3.4 `docs/roadmap/risks.md` — the risk register, the spikes each milestone opens with, and
      the deferred scope with its re-entry points and protected seams
- [x] 3.5 `docs/README.md` — documentation index
- [x] 3.6 Root `README.md` — a roadmap section linking the ladder and the matrix
- [x] 3.7 `openspec/specs/README.md` — list `delivery-roadmap` under section 7

## 4. Tooling (deferred to M0)

- [ ] 4.1 `just roadmap::status` — report every capability's tier, the milestone that last advanced
      it, and the change that did; exit non-zero when the record and `openspec/specs/` disagree
- [ ] 4.2 `just roadmap::milestone <id>` — run a milestone's full exit criteria and exit non-zero if
      any fail
- [ ] 4.3 Continuous integration runs the status check on every pull request
- [ ] 4.4 A pull request template field naming the capability and tier the change advances
- [ ] 4.5 The deferred-seam checks — multi-view rendering, runtime-driven frame timing, and
      late-latching — wired as tests when the renderer reaches M3
