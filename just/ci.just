# just/ci.just — Continuous integration: the workflows, and the checks that keep them honest.
#
# Continuous integration invokes the recipes a developer invokes, and nothing else. That is a
# requirement of `developer-workflow-and-just`, and "a continuous integration script that duplicates
# rather than invokes recipes" is on its list of forbidden patterns — a list whose every entry has
# to be checkable. This category holds the checks that make those two entries checkable.
#
# There is no `ci-run`. A job runs recipes from other categories; a category that ran jobs would be
# the duplication itself.

# Check the workflow itself: CI duplicates no recipe logic, and doctor diagnoses a broken environment.
ci-check *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    echo "==> rules       negative fixtures, so a rule that stopped firing is caught"
    python3 tools/ci/check_workflows.py --selftest
    echo "==> workflows   .github/workflows/"
    python3 tools/ci/check_workflows.py {{args}}
    echo "==> env-doctor  against deliberately broken environments"
    python3 tools/ci/test_env_doctor.py {{args}}
    echo "==> recipes     invariants a gate downstream of a recipe depends on"
    python3 tools/ci/test_recipes.py {{args}}
    # M11.a section 4. The cross-leg comparator is the only thing between "two architectures agreed"
    # and a green tick over nothing: `m9:lockstep-cross-platform` was declared a gap rather than a
    # `where = "ci"` criterion precisely because a single-leg suite would have satisfied the latter.
    # Its refusals are checked here, with negative fixtures, for the same reason check_workflows'
    # are: a rule that stopped firing looks exactly like a tree with nothing wrong in it.
    echo "==> cross-leg   the digest comparator's refusals, against fixtures that must not pass"
    python3 tools/ci/test_cross_leg_digests.py {{args}}
    # And one layer up: the three criteria that job answers were word-greps until M11.a's gate read
    # them, so what checks them now RUNS the job's own comparison command against digests it wrote —
    # and, since the repair-2 gate showed that A COMMAND IS NOT A JOB, also evaluates every `if:`
    # between the trigger and the comparison, and runs the LEG's own publishing command to see
    # whether it routes the path its job uploads. THE REPAIR-3 GATE THEN SHOWED THAT A JOB IS NOT A
    # RUN, in one line: `continue-on-error: true` on the comparison step leaves the comparator
    # running, exiting 1 on a disagreement, and the job's conclusion SUCCESS — so the audit now also
    # asks whether a red anywhere in the chain reaches the pipeline at all. Its seventeen fixtures
    # are the dummy job the greps accepted, the repair-2 gate's three (`if: false` on the comparison
    # job, the publishing step deleted, that step replaced by an `echo`), the repair-3 gate's own
    # one-liner and the rest of its family (the comparison job forgiven, the publishing step
    # forgiven, `if-no-files-found` left at `warn`, forgiveness behind an unreadable expression), a
    # condition nobody can read, a publication with no comparison, a comparison that does not
    # `needs:` its publication, a one-leg matrix, a one-runner matrix, and a job that fails whatever
    # it is handed. Every one must be refused, and refused SAYING the thing it is about — a rule that
    # began refusing everything would otherwise read exactly like seventeen discriminating ones.
    echo "==> cross-leg   the audit behind m9:lockstep-cross-platform and two more, against a dummy job"
    python3 tools/ci/cross_leg_audit.py --selftest

# List the jobs of every workflow and the recipes each one invokes. The gate set, from the source.
ci-jobs:
    @cd "{{root}}" && python3 tools/ci/check_workflows.py --list
