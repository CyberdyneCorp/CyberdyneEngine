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
    echo "==> cross-leg   the nightly ledger receives the publisher artifacts"
    python3 tools/ci/test_cross_leg_ledger.py

# List the jobs of every workflow and the recipes each one invokes. The gate set, from the source.
ci-jobs:
    @cd "{{root}}" && python3 tools/ci/check_workflows.py --list

# THE JOBS THAT TEST A BUILD USE THAT BUILD. Until October 2026 the `test` legs restored the build
# job's tree from the actions cache and then rebuilt all of it: a fresh checkout stamps every source
# file with the checkout's time, which is later than every object in a tree built earlier, so Ninja
# found the whole engine out of date — 4 512 steps and 14 to 17 minutes on windows-x86_64 with an
# exact cache hit. When the cache entry had been evicted (the repository's caches outgrow GitHub's
# 10 GB limit within one run), the leg rebuilt from nothing: 59 of its 60 minutes, run 37730586551.
#
# So the tree travels as an artefact of the run, which no eviction can remove, and records the
# commit it was built from; `ci-build-tree-unpack` refuses it at any other commit. The archive is
# tar through zstd because the artefact store's zip loses the modification times Ninja compares.
#
# Pack the build tree for the jobs of this run that test it. Usage: just ci-build-tree-pack [archive]
ci-build-tree-pack *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    tree="${CY_BUILD_DIR:-build/${profile}}"
    archive="${rest:-build-tree.tar.zst}"
    just _ci-build-tree-preconditions "${tree}"
    if [ ! -f "${tree}/CMakeCache.txt" ]; then
        echo "ci-build-tree-pack: ${tree} is not a configured build tree; run \`just build-engine\` first" >&2
        exit 1
    fi
    # A tree built from modified sources does not correspond to any commit, and the commit is the
    # one thing the unpacking side is allowed to trust.
    if ! git diff --quiet HEAD --; then
        echo "ci-build-tree-pack: tracked files differ from HEAD, so ${tree} was not built from a commit:" >&2
        git status --short --untracked-files=no >&2
        exit 1
    fi
    git rev-parse HEAD > "${tree}/.cy-build-tree-commit"
    rm -f "${archive}"
    echo "==> pack        ${tree} at $(git rev-parse --short HEAD) -> ${archive}"
    tar -cf - "${tree}" | zstd -T0 -3 -q -o "${archive}"
    echo "==> packed      $(wc -c < "${archive}" | tr -d ' ') bytes"

# A FRESH CHECKOUT IS NEWER THAN EVERY OUTPUT, so after unpacking, every tracked file is dated to
# 2000-01-01: the sources are the very bytes the tree was built from, and Ninja, which compares
# times rather than contents, then agrees that nothing needs rebuilding. That is only true when the
# tree is from THIS commit and no tracked file has been changed since checkout, so both are checked
# first and either refuses. Anything restored from a cache that predates this commit (the editor's
# Cargo target directory, say) must be built BEFORE this recipe runs, or its stale outputs would
# look newer than sources they were not built from; `just ci-check` holds the workflow to that order.
#
# Unpack a packed tree at the commit it was built from. Usage: just ci-build-tree-unpack [archive]
ci-build-tree-unpack *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    tree="${CY_BUILD_DIR:-build/${profile}}"
    archive="${rest:-build-tree.tar.zst}"
    just _ci-build-tree-preconditions "${tree}"
    if [ ! -f "${archive}" ]; then
        echo "ci-build-tree-unpack: no ${archive}; the job must download the build job's artefact first" >&2
        exit 1
    fi
    if [ -e "${tree}" ]; then
        echo "ci-build-tree-unpack: ${tree} already exists; refusing to merge another tree into it" >&2
        exit 1
    fi
    if ! git diff --quiet HEAD --; then
        echo "ci-build-tree-unpack: tracked files differ from HEAD, so the sources are not the ones the tree was built from:" >&2
        git status --short --untracked-files=no >&2
        exit 1
    fi
    echo "==> unpack      ${archive} -> ${tree}"
    zstd -dc "${archive}" | tar -xf -
    rm -f "${archive}"
    head="$(git rev-parse HEAD)"
    built="$(cat "${tree}/.cy-build-tree-commit" 2>/dev/null || echo "an unrecorded commit")"
    if [ "${built}" != "${head}" ]; then
        echo "ci-build-tree-unpack: ${tree} was built from ${built}, and this checkout is ${head}." >&2
        echo "  Its outputs would be trusted for sources they were not built from; removing it." >&2
        rm -rf "${tree}"
        exit 1
    fi
    git ls-files -z | xargs -0 touch -c -t 200001010000
    echo "==> unpacked    ${tree} at $(git rev-parse --short HEAD); tracked sources dated before its outputs"

# The two recipes' shared refusals: the tree must lie inside the repository, where `tar` stores the
# same relative path on every host, and the compressor must exist.
[private]
_ci-build-tree-preconditions tree:
    #!/usr/bin/env bash
    set -euo pipefail
    case "{{tree}}" in
        /*|[A-Za-z]:*|*..*)
            echo "ci-build-tree: '{{tree}}' must be a path inside the repository" >&2
            exit 2 ;;
    esac
    if ! command -v zstd >/dev/null 2>&1; then
        echo "ci-build-tree: zstd is not on PATH (apt-get install zstd, brew install zstd, choco install zstandard)" >&2
        exit 1
    fi
