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

# Run a milestone's full exit criteria. Usage: just roadmap-milestone <id> [--list] [--ci]
roadmap-milestone *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    if [ -z "{{args}}" ]; then
        echo "usage: just roadmap-milestone <id> [--list] [--ci] [--json]" >&2
        echo "  milestones with criteria: $(ls tools/roadmap/milestones/*.toml \
            | xargs -n1 basename | sed 's/\.toml$//' | tr '\n' ' ')" >&2
        exit 2
    fi
    python3 tools/roadmap/roadmap.py milestone {{args}}

# List the permanent merge gates and any recorded override. --commands prints what CI must run.
roadmap-gates *args:
    @cd "{{root}}" && python3 tools/roadmap/roadmap.py gates {{args}}

# Test the roadmap tooling itself: drift detection, criteria loading, and the override rules.
roadmap-test *args:
    @cd "{{root}}" && python3 tools/roadmap/selftest.py {{args}}
