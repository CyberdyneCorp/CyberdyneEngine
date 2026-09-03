<!--
The capability-and-tier table below is required by `delivery-roadmap`: a change that implements or
advances a capability updates docs/roadmap/status.yaml in the same change, and may not claim a tier
the record does not support. `just roadmap-status` checks the record against openspec/specs/ and
fails on drift, so a table here that disagrees with the record will not merge.

Delete nothing. A section that does not apply is answered with "none" and why.
-->

## What changed, and why

<!-- One paragraph. The reviewer should be able to stop here and know what they are reading. -->

## Capability and tier

| Capability | Tier before | Tier after | Milestone |
|---|---|---|---|
| <!-- e.g. developer-workflow-and-just --> | — / seed / working / complete | — / seed / working / complete | <!-- m0 --> |

- OpenSpec change: <!-- openspec/changes/<id>/, or "none — this changes no specified behaviour" -->
- Tasks completed: <!-- the task numbers from that change's tasks.md -->
- `docs/roadmap/status.yaml` updated in this pull request: yes / no / not applicable

A tier claimed here that the record does not support is drift, and drift fails the build.

## Evidence

<!--
Name the recipes you ran and what they printed. `developer-workflow-and-just` requires that a check
gating a change is runnable locally by one command, so the evidence is those commands — not a
description of them.
-->

```
just env-doctor
just build-all
just test-all
```

## Gates

- [ ] `just quality-format-check` and `just quality-lint` pass
- [ ] `just quality-layers` passes — no target, link or include reaches upward
- [ ] `just quality-specs` passes
- [ ] `just generate-check` passes — no generated artefact is stale
- [ ] `just roadmap-status` passes — the record and the specification set agree
- [ ] A defect fixed here has a regression test that fails without the fix
- [ ] Documentation names the recipe rather than restating its command line

## Risk

<!--
What could this break that the gates would not catch, and what would you look at first if it did?
"Nothing" is an acceptable answer for a documentation change and for almost nothing else.
-->
