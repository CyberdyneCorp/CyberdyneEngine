# just/roadmap.just — Roadmap: what is implemented, and what closes the current milestone.
#
# These answer both questions without anyone reading a document to find out, and they fail rather
# than report: drift between docs/roadmap/status.yaml and openspec/specs/ is a build failure, and a
# milestone whose criteria do not all pass is not closed.
#
# The record, the criteria and the gate set are data — docs/roadmap/status.yaml,
# tools/roadmap/milestones/<id>.toml, tools/roadmap/gates.toml. M1 through M11 add a criteria file
# each and change no code.

# Report every capability's tier, the milestone that last advanced it, and the change that did so.
roadmap-status *args:
    @cd "{{root}}" && python3 tools/roadmap/roadmap.py status {{args}}

# Regenerate docs/roadmap/open-debts.md from the ledgers, the record and the archive. --check verifies.
roadmap-debts *args:
    @cd "{{root}}" && python3 tools/roadmap/debts.py {{args}}

# Usage: just roadmap-milestone <id> [--list] [--ci] [--json]
#   [--jobs n]  independent criteria at a time. DEFAULT 1 — concurrency is opt-in, because at
#               min(8, cores) it changed verdicts: five criteria failed that pass when run alone,
#               on a wall-clock watchdog that does not subtract I/O waiting. Set --jobs n or
#               CY_LEDGER_JOBS to opt in. What each criterion holds is in tools/roadmap/
#               schedule.py; one whose needs cannot be read runs alone.
#   [--incremental [--changed-since <commit>] [--build-dir <tree>]]  only the rung's own criteria,
#               the earlier ones whose inputs changed since <commit> (default: the last green FULL
#               run of this rung), and the smoke set — build, format, lint, test-all. A criterion
#               whose inputs cannot be read is always run. With --list it prints the selection and
#               why. NOT the default: the full ledger runs nightly and at M11.e, and only a green
#               full run on a clean tree records the baseline. tools/roadmap/incremental.py.
#
# Run a milestone's full exit criteria, or with --incremental only what a change can have moved.
roadmap-milestone *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    if [ -z "{{args}}" ]; then
        echo "usage: just roadmap-milestone <id> [--list] [--ci] [--json] [--jobs n]" >&2
        echo "       [--incremental [--changed-since <commit>] [--build-dir <tree>]]" >&2
        echo "  milestones with criteria: $(ls tools/roadmap/milestones/*.toml \
            | xargs -n1 basename | sed 's/\.toml$//' | tr '\n' ' ')" >&2
        exit 2
    fi
    # MARK THE TREE THIS LEDGER BUILDS IN, so that `just build-reap` keeps it between two runs: a
    # reaped ledger tree turned M11.c's fifth close into a 7 h 28 m build from empty. Only a
    # redirected tree is marked — with CY_BUILD_DIR unset the recipes use the developer's own
    # build/<profile>. Deleting the marker is how the tree is let go; build-reap says so.
    if [ -n "${CY_BUILD_DIR:-}" ] && [[ " {{args}} " != *" --list "* ]]; then
        mkdir -p "${CY_BUILD_DIR}"
        printf '%s\n' "the tree of \`just roadmap-milestone {{args}}\`, $(date -u +%FT%TZ); delete this file to let build-reap reclaim it" \
            > "${CY_BUILD_DIR}/.cy-keep"
    fi
    python3 tools/roadmap/roadmap.py milestone {{args}}

# List the permanent merge gates and any recorded override. --commands prints what CI must run.
roadmap-gates *args:
    @cd "{{root}}" && python3 tools/roadmap/roadmap.py gates {{args}}

# Test the roadmap tooling itself: drift detection, criteria loading, and the override rules.
roadmap-test *args:
    @cd "{{root}}" && python3 tools/roadmap/selftest.py {{args}}

# Show which criteria have been shown able to FAIL. --record writes tools/roadmap/falsifiability.toml.
#
# A criterion nobody has shown can fail is not a check, and this repository has now found seven of
# them. `prove` breaks what each criterion names — in a sandboxed copy of the tracked tree, never in
# the tree — and requires it to go red; `audit` reads the shapes that cannot go red whatever the
# tree contains; `check` is what `just roadmap-test` runs on every pull request.
roadmap-falsify *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    # `${argv[@]+"${argv[@]}"}` rather than `"${argv[@]}"`: an empty array under `set -u` is an
    # unbound variable on bash 4.3, which is what `just roadmap-falsify` with no arguments is.
    argv=({{args}})
    case "${argv[0]:-prove}" in
        audit|prove|check) python3 tools/roadmap/falsify.py ${argv[@]+"${argv[@]}"} ;;
        *)                 python3 tools/roadmap/falsify.py prove ${argv[@]+"${argv[@]}"} ;;
    esac
