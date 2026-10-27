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

# THE NIGHTLY LEDGER RAN THE HOSTED RUNNER OUT OF DISK. Twice, on 2026-10-13 and 2026-10-14, the
# `milestone` job's runner crashed with `No space left on device` after five hours and took the log
# with it. The ubuntu-24.04 image (20261004) has 145 GB on `/` with 87 GB free; a full ledger run
# builds trees totalling several times that, which `just roadmap-milestone --reclaim-builds` now
# deletes as their last user finishes (tools/roadmap/reclaim.py). The run still holds the default
# tree, two sanitizer trees and four profile trees at once, so this recipe also removes the SDKs the
# image preinstalls and nothing here uses — Android, .NET, Haskell, CodeQL, the image's own Docker
# images — and prints `df` before and after, so the next full disk is a number in the log.
#
# CI only: it deletes system directories with sudo, and refuses to run anywhere GITHUB_ACTIONS is
# not `true`.
#
# Free the hosted runner's disk of the SDKs the image preinstalls. Usage: just ci-free-disk
ci-free-disk:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ "${GITHUB_ACTIONS:-}" != "true" ]; then
        echo "ci-free-disk: refusing outside a GitHub-hosted runner (GITHUB_ACTIONS is not 'true'): it deletes system directories" >&2
        exit 2
    fi
    echo "==> before"
    df -h /
    if mountpoint -q /mnt 2>/dev/null; then
        df -h /mnt
    fi
    # The second group is what a sharded ledger's heaviest shards still lacked (four profile rows
    # beside the default tree, or four sanitizer trees): browsers, the JVMs, PowerShell, Boost and
    # the cloud CLIs, none of which any recipe here invokes.
    for sdk in /usr/local/lib/android /usr/share/dotnet /opt/ghc /usr/local/.ghcup \
               /opt/hostedtoolcache/CodeQL \
               /usr/local/share/boost /usr/local/share/powershell /usr/local/share/chromium \
               /usr/lib/jvm /opt/microsoft /opt/google /opt/az /usr/local/lib/heroku \
               /usr/local/julia*; do
        if [ -e "${sdk}" ]; then
            echo "==> remove      ${sdk}"
            sudo rm -rf "${sdk}"
        fi
    done
    if command -v docker > /dev/null; then
        echo "==> remove      the image's Docker images"
        sudo docker image prune --all --force > /dev/null || true
    fi
    echo "==> after"
    df -h /
    # A second disk, where the image has one, is the place a run can put what does not fit on `/`.
    if mountpoint -q /mnt 2>/dev/null; then
        df -h /mnt
    fi

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
# AND IT CARRIES ITS OWN SIZE AND DIGEST, because the download can end short and still succeed. Three
# nightly runs of one commit (40059896233, 40159825948, 40266516833) lost ten jobs between them to
# "build-tree.tar.zst : Read error (39) : premature end": `actions/download-artifact` stopped partway
# through the six-gigabyte archive without its "download completed" line and without failing the
# step. `<archive>.digest` is written here; `ci-build-tree-verify` lets a job fetch it again
# (`ci-build-tree-fetch`) when what arrived is not what was packed, and `ci-build-tree-unpack`
# refuses it outright.
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
    rm -f "${archive}" "${archive}.digest"
    echo "==> pack        ${tree} at $(git rev-parse --short HEAD) -> ${archive}"
    tar -cf - "${tree}" | zstd -T0 -3 -q -o "${archive}"
    just _ci-build-tree-digest "${archive}" > "${archive}.digest"
    echo "==> packed      $(cat "${archive}.digest")"

# Whether a downloaded archive is the one the build job packed, as `intact=true` or `intact=false`
# in the step's outputs, so that the workflow can download it again. Never fails on a short or
# corrupt archive — the second download is the remedy, and `ci-build-tree-unpack` the refusal.
# Usage: just ci-build-tree-verify [archive]
ci-build-tree-verify *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r _ rest <<< "$(just _split {{args}})"
    archive="${rest:-build-tree.tar.zst}"
    if just _ci-build-tree-intact "${archive}"; then
        intact=true
    else
        intact=false
        echo "ci-build-tree-verify: ${archive} is not the archive the build job packed; fetch it again" >&2
    fi
    echo "==> verify      ${archive}: intact=${intact}"
    if [ -n "${GITHUB_OUTPUT:-}" ]; then
        echo "intact=${intact}" >> "${GITHUB_OUTPUT}"
    fi

# Fetch the build tree's artefact again through the REST API, until what arrives is what was packed.
#
# A SECOND `actions/download-artifact` IS NOT ENOUGH. On this branch's own PR run (40505846704) the
# `first light` job's two downloads both ended short, before the zip even reached the digest. So the
# fallback changes transport: `gh run download` fetches the whole zip and unpacks it from its central
# directory, which a short download fails loudly, and this repeats it up to three times. Needs
# GH_TOKEN and GITHUB_RUN_ID, which a workflow step provides. Usage: just ci-build-tree-fetch <artefact>
ci-build-tree-fetch artefact:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    archive="build-tree.tar.zst"
    staging=".build-tree-fetch"
    for attempt in 1 2 3; do
        rm -rf "${staging}"
        echo "==> fetch       {{artefact}} through the REST API (attempt ${attempt})"
        if gh run download "${GITHUB_RUN_ID:?ci-build-tree-fetch runs inside a workflow}" \
                --name "{{artefact}}" --dir "${staging}"; then
            mv -f "${staging}/${archive}" "${staging}/${archive}.digest" . 2>/dev/null || true
            if just _ci-build-tree-intact "${archive}"; then
                rm -rf "${staging}"
                echo "==> fetched     ${archive}, intact"
                exit 0
            fi
        fi
        echo "ci-build-tree-fetch: attempt ${attempt} did not deliver the packed archive" >&2
    done
    rm -rf "${staging}"
    echo "ci-build-tree-fetch: {{artefact}} did not arrive intact in three attempts" >&2
    exit 1

# `<sha256> <bytes>` of a file, on every host the legs run on.
[private]
_ci-build-tree-digest archive:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    if command -v sha256sum >/dev/null 2>&1; then
        digest="$(sha256sum "{{archive}}" | cut -d' ' -f1)"
    else
        digest="$(shasum -a 256 "{{archive}}" | cut -d' ' -f1)"
    fi
    echo "${digest} $(wc -c < "{{archive}}" | tr -d ' ')"

# Succeeds when `archive` matches the `<archive>.digest` packed beside it, and says why when not.
[private]
_ci-build-tree-intact archive:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    if [ ! -f "{{archive}}" ] || [ ! -f "{{archive}}.digest" ]; then
        echo "ci-build-tree: {{archive}} or its .digest is missing" >&2
        exit 1
    fi
    read -r packed_digest packed_bytes < "{{archive}}.digest"
    bytes="$(wc -c < "{{archive}}" | tr -d ' ')"
    if [ "${bytes}" != "${packed_bytes}" ]; then
        echo "ci-build-tree: {{archive}} is ${bytes} bytes and was packed as ${packed_bytes}" >&2
        exit 1
    fi
    read -r digest _ <<< "$(just _ci-build-tree-digest "{{archive}}")"
    if [ "${digest}" != "${packed_digest}" ]; then
        echo "ci-build-tree: {{archive}} has sha256 ${digest} and was packed as ${packed_digest}" >&2
        exit 1
    fi

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
    if ! just _ci-build-tree-intact "${archive}"; then
        echo "ci-build-tree-unpack: ${archive} is not the archive the build job packed — a download that ended short — so it is not unpacked" >&2
        exit 1
    fi
    echo "==> unpack      ${archive} -> ${tree}"
    # MSYS's tar, which Git Bash runs on Windows, emulates a symlink by copying its target, so a link
    # extracted before its target fails ("Cannot create symlink to 'zstd'", in _deps/zstd-src's test
    # data). A shortcut needs no target and no privilege; elsewhere the variable means nothing.
    zstd -dc "${archive}" | MSYS="${MSYS:+${MSYS} }winsymlinks:lnk" tar -xf -
    rm -f "${archive}" "${archive}.digest"
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
