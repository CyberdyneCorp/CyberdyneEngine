# STATUS AT 06:30, 2026-09-08 — READ THIS FIRST

M7's gate said NOT CLOSEABLE. Per the stop condition below, **M8.a was not launched.**

The user was awake for part of the night and chose the remedy: **B + C2 + the swept
criterion**. All three are implemented, verified and pushed as `51f1c56`:

  * the convergence envelope, at FOUR deadbands, in `rendering-architecture`
  * twelve warm-up frames discarded before the device frame is measured
  * `integration.render_arbiter_sweep` sweeps the operating point and asserts the envelope
  * plus a substring-matching bug in the artefact harness ("nominal" contains "min")

smoke.fidelity now passes 14 of 14; it was 14 of 20. All gates green, test-all 180/180.

**THE M7 LEDGER IS THE REMAINING STEP.** It was at 20 criteria with zero failures when the
reaper took it (swap is still 7 MB free). Relaunched detached, logging to
/tmp/m7ledger2.log. If it is green: close M7 (section A below), then B, C, D as written.
If it fails, read the failure before assuming it is the arbiter — it may not be.

Full analysis of the blocker and the measurements is committed at
openspec/changes/implement-m7-fidelity/BLOCKER.md

---

# Overnight runbook — authorised by the user 2026-09-07 ~21:00

Standing authorisation, in their words: when M7 finishes, launch M8.a; if M8.a finishes
before they wake, launch M8.b; update the artifact between each, including screenshots
from the milestone. Workflows run to completion as all the previous ones have.

Execute this in order. Do not skip a step because the previous one looked fine.

## A. WHEN M7's WORKFLOW COMPLETES

1. Read the gate's verdict from the task output file. If the gate agent FAILED (usage
   limit, API error), resume the workflow with resumeFromRunId rather than re-running:
   Workflow({scriptPath: ".../implement-m7-fidelity-wf_a3bce5c2-e12.js",
             resumeFromRunId: "wf_a3bce5c2-e12"})
2. Fix what the gate found that is M7's own. Regression test for each — the user's global
   rule: "Any bug found need to have a regression test added on the PR that solve the issue".
3. Mark tasks honestly in openspec/changes/implement-m7-fidelity/tasks.md. Leave unchecked
   what is not done and write the reasons under a "What this milestone did NOT close" heading,
   as M6's close did.
4. THE THREE COUPLED EDITS, IN ONE COMMIT — ci-check fails unless they land together:
     - tools/roadmap/gates.toml: milestone-m7 joins-on-close -> green
     - .github/workflows/ci.yml: the milestone job's ledger m6 -> m7
     - README.md status line
5. openspec archive implement-m7-fidelity --yes
6. Verify: just roadmap-test, ci-check, roadmap-status, quality-specs. Commit. PUSH.

## B. APPLY THE M8 SPLIT — BEFORE LAUNCHING M8.a

The change is written and committed: openspec/changes/split-m8-authorable-and-systems/
Follow ITS tasks.md. The order matters:
  1. Ladder tooling first: m8a and m8b into record.MILESTONES IN POSITION, plus the test
     that M8.a inherits M7's criteria and M8.b inherits M8.a's. This is the M5.5 rung bug
     and it must not recur.
  2. Reconcile with whatever M7's gate opened as its task 12.8 ("open the M8 change"). If it
     created a single implement-m8-*, fold it into two and delete the single one.
  3. The four plan documents: ROADMAP.md, capability-matrix.md, status.yaml, dependencies.md.
     RUN M7's NEW PLAN-CONSISTENCY TOOL (its task 12.7) over them — this change is its first
     real exercise.
  4. Archive the split change. Commit. PUSH.

## C. UPDATE BOTH ARTIFACTS

  roadmap:  https://claude.ai/code/artifact/954bf408-42c7-410f-94a2-87e0ae8edc14
            file: scratchpad/cyberdyne-roadmap.html
  buildlog: https://claude.ai/code/artifact/e5b4d967-5083-4393-81d9-7646f6f764fc
            file: scratchpad/cyberdyne-buildlog.html

For each milestone that closes: header status line, ladder cursor (CURRENT in the roadmap
page), the effort table row (hours + agent count from the journal timestamps), the
"eight of eight" gate count, and A NEW SCREENSHOT from the milestone's own artefact.
Encode with PIL at ~1500px, JPEG q74-78. Label engine output "Built"; never present a
generated illustration as engine output.

## D. LAUNCH M8.a — Authorable

Model the workflow on M7's (scriptPath in workflows/scripts/). Phases, strict file
ownership, per-agent build dirs, adversarial gate last. Carry forward every hard rule,
and these specifically:
  - A criterion you write MUST actually execute something (M6: four never ran).
  - An artefact reporting a GAP must not exit zero, and must not headline an extreme.
  - Test teardown under load (M5.5's Jolt race).
  - XTEST delivers NO key events in this X session; pointer events work.
  - Verify the process tree is empty before any timing measurement.
M8.a's scope is split tasks 4.1-4.6: primitives, the physics ECS bridge, editor-side
import, and OBJ — which is now DECIDED (user confirmed 2026-09-07: OBJ alongside FBX,
not instead of it) and specified as a delta on asset-import-pipeline in that change.
OBJ reaches only steps 1-6 and 9 of the import sequence, must REPORT the steps it did
not reach rather than warn about them, and must go through the SAME derivation key M7
unified — a second key for a third importer is the bug M7 spent its first section
repairing. Closing artefact: create a sphere, drop it on a
box, press play, watch it fall, stop, undo back to an empty world.

## E. IF M8.a CLOSES BEFORE THE USER WAKES

Repeat A (for M8.a), C, then launch M8.b — Systems. M8.b's spike is the shared graph IR
across seven consumers and it is the hardest on the ladder: give it its own phase, first,
alone, and let it veto.

## STOP CONDITIONS — do not push past these
  - A gate says a milestone is NOT closeable: commit the work, do NOT launch the next
    milestone, write up what remains and wait.
  - Usage limit hit: commit and push everything, note where it stopped. Resume needs the
    user.
  - Two consecutive workflow failures for the same reason: stop and report.
ALWAYS commit and push before launching anything new. The machine has been at swap
exhaustion for days and background waiters are reaped within minutes.
