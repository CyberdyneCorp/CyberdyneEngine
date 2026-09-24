# just/content.just — Content: import, cook, package, patch, validate.
#
# Nothing here is implemented at M0, and not because it was skipped: there is no asset pipeline to
# drive until the importer and cooker land. The recipes exist so that the category is discoverable
# and so that the first person to need one adds behaviour rather than inventing a name.
#
# Each one names the milestone that fills it and the capability that governs it, so the listing
# answers "when" as well as "not yet". A recipe that failed with "not implemented" and no more would
# be a worse answer than no recipe at all.
#
# M6 FILLED ALL FIVE. Nothing here is a placeholder any more; this header is kept because the
# argument it makes is the reason the names were right before the behaviour existed.

# --- M6 filled these three ----------------------------------------------------------------------
#
# `asset-import-pipeline` reached Working at M5 and Complete at M6, not M8: the roadmap moved it and
# these three recipes still named the old milestone. tools/import/ owns the importers, the cook cache
# and the sidecars, and tools/cook/ owns the document cook; these recipes orchestrate `cy_import_cli`
# and `cy_cook` and reimplement neither, which is what the justfile's own header requires.
#
# Every flag after the profile is passed through, so a tool's behaviour and its recipe cannot drift.

# Import source assets into the engine's own formats. Usage: just content-import [--profile <p>] --project <dir> --out <dir> [--cache <dir>] [--variant <k>] [--profile-name client|dedicated-server|editor] [--no-mint] <sources...>
content-import *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    just build-engine --profile "${profile}" -D CY_BUILD_TOOLS=ON
    # `--profile` is taken by the build profile, so the COOK profile is spelled `--profile-name`
    # here and translated. Two flags called `--profile` in one command line is the kind of ambiguity
    # that is discovered by cooking the wrong content.
    passthrough=()
    expecting=0
    for argument in ${rest}; do
        if [ "${expecting}" -eq 1 ]; then
            passthrough+=(--profile "${argument}"); expecting=0; continue
        fi
        case "${argument}" in
            --profile-name)   expecting=1 ;;
            --profile-name=*) passthrough+=(--profile "${argument#*=}") ;;
            *)                passthrough+=("${argument}") ;;
        esac
    done
    exec "${build_dir}/tools/import/cy_import_cli" ${passthrough[@]+"${passthrough[@]}"}

# Cook imported content for a target platform and profile. Usage: just content-cook [--profile <p>] --source <dir> --output <file.cypak> [--variant <key>] [--shipping] [--fail-on-conflicts]
content-cook *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    just build-engine --profile "${profile}" -D CY_BUILD_TOOLS=ON
    exec "${build_dir}/tools/cook/cy_cook" ${rest}

# THE DETERMINISM GATE, and it is a gate rather than a diagnostic.
#
# `asset-import-pipeline`: "WHEN the same source and options are imported twice THEN the cooked
# output SHALL be byte-identical." `build-and-packaging`: "Determinism SHALL be verified in
# continuous integration." design.md §1.7 says why M6 is when this stops being optional — an
# `AssetId` reaches the inside of a payload at this milestone, and `assets::mint_asset_id()` draws
# 128 random bits, so from that commit two cold builds of one project can differ and nothing would
# notice.
#
# The shape is `just generate-check`'s, which has run this way since M0: run it twice into two
# directories, diff the results, and grep them for absolute paths. A path baked into an artefact is
# not a determinism failure between two runs on ONE machine — it is a determinism failure between two
# machines, which is the one this project cannot see by re-running.

# Cook twice and prove the output is byte-identical and machine-independent. Usage: just content-validate [--profile <p>] --project <dir> <sources...>
content-validate *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    just build-engine --profile "${profile}" -D CY_BUILD_TOOLS=ON

    project=""
    sources=()
    expecting=0
    for argument in ${rest}; do
        if [ "${expecting}" -eq 1 ]; then
            project="${argument}"; expecting=0; continue
        fi
        case "${argument}" in
            --project)   expecting=1 ;;
            --project=*) project="${argument#*=}" ;;
            *)           sources+=("${argument}") ;;
        esac
    done
    if [ -z "${project}" ] || [ ${#sources[@]} -eq 0 ]; then
        echo "content-validate: --project <dir> and at least one source are required" >&2
        exit 2
    fi

    scratch="$(mktemp -d)"
    trap 'rm -rf "${scratch}"' EXIT
    importer="${build_dir}/tools/import/cy_import_cli"

    # NO CACHE on either run. A cache would make the second run a copy of the first's answer, which
    # is exactly the thing E8 of the spike showed is worse than no cache at all: a non-deterministic
    # step plus a cache freezes whichever machine missed first.
    #
    # `--no-mint` ON BOTH RUNS, and this is the half of the gate that actually catches something.
    # Two runs in one working tree cannot see the minting defect on their own: the first run WRITES
    # the `.import` sidecar and the second reads it, so the ids agree and the diff passes — while two
    # cold checkouts on two machines would each mint their own. Refusing to mint turns that into a
    # failure here, on the first run, naming the asset whose sidecar is missing.
    echo "==> cook        run 1"
    "${importer}" --project "${project}" --no-mint --out "${scratch}/a" "${sources[@]}" >/dev/null
    echo "==> cook        run 2"
    "${importer}" --project "${project}" --no-mint --out "${scratch}/b" "${sources[@]}" >/dev/null

    echo "==> diff        two cold cooks of the same sources"
    if ! diff -r "${scratch}/a" "${scratch}/b" >"${scratch}/diff"; then
        echo >&2
        echo "content-validate: two cooks of the same sources produced different output." >&2
        sed 's/^/  /' "${scratch}/diff" >&2
        echo >&2
        echo "  A cooked artefact must be a pure function of its inputs, or the content-addressed" >&2
        echo "  cache serves the wrong bytes and a patch diffs against the wrong baseline." >&2
        exit 1
    fi

    echo "==> grep        absolute paths baked into the artefacts"
    root="$(pwd)"
    if grep -rlI --binary-files=text -e "${root}" -e "${scratch}" -e "${HOME}" "${scratch}/a" \
            >"${scratch}/paths" 2>/dev/null; then
        echo >&2
        echo "content-validate: a cooked artefact carries an absolute path:" >&2
        sed 's/^/  /' "${scratch}/paths" >&2
        echo >&2
        echo "  Two runs on ONE machine agree and two machines do not, which is the determinism" >&2
        echo "  failure a re-run cannot see. Every path in an artefact is project-relative." >&2
        exit 1
    fi

    count=$(find "${scratch}/a" -type f | wc -l)
    echo "content-validate: ${count} artefact(s), byte-identical across two cold cooks, no absolute paths"

# --- M6 filled these two ------------------------------------------------------------------------
#
# `build-and-packaging` reached Working at M6, not M12: the roadmap moved it and these two recipes
# still named the old milestone. tools/build/ owns the derivation graph, the derived data cache and
# the patcher; these recipes orchestrate `cy_build` and reimplement none of it, which is what the
# justfile's own header requires of every recipe here.

# Every flag after the profile is passed through to `cy_build build`, so the tool's behaviour and
# this recipe cannot drift apart.

# Run a build description's graph and write its package manifest. Usage: just content-package [--profile <p>] --description <file> --project <dir> --out <store> --cache <dir> --package <manifest>
content-package *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    # `--platform` is the package recipe's half of M11.d task 7.7 — `developer-workflow-and-just`
    # names build, test, DEPLOY and PACKAGE, and this recipe used to pass the flag through to
    # `cy_build`, which does not take it and would have failed naming an option rather than a
    # target. The resolution is `just/targets.toml`'s, so the refusal here and the refusal from
    # `just build-engine` are the same words.
    platform=()
    tool_args=()
    expecting=0
    for argument in ${rest}; do
        if [ "${expecting}" -eq 1 ]; then
            platform=(--platform "${argument}"); expecting=0; continue
        fi
        case "${argument}" in
            --platform)   expecting=1 ;;
            --platform=*) platform=(--platform "${argument#*=}") ;;
            *)            tool_args+=("${argument}") ;;
        esac
    done
    if [ "${expecting}" -eq 1 ]; then
        echo "--platform needs a name. \`just env-targets\` prints the ones that exist." >&2
        exit 2
    fi
    just build-engine --profile "${profile}" ${platform[@]+"${platform[@]}"} -D CY_BUILD_TOOLS=ON
    exec "${build_dir}/tools/build/cy_build" build ${tool_args[@]+"${tool_args[@]}"}

# The content audit — M11.d task 7.4.
#
# `build-and-packaging` asks four questions of a build and `tools/build/` could answer all four:
#
#     why is this in the build?      the reference chain from a declared delivery root
#     what references this?          the dependents
#     size by ...                    category, asset, plugin, world region and install bundle
#     cook and compile time by ...   stage, with cache hit rates
#
# NO RECIPE REACHED ANY OF THEM. `cy_build audit` and `cy_build explain` have existed since M6 and
# nothing in `just --list` named them, so the audit was a capability with no workflow — which
# `developer-workflow-and-just` calls a defect in as many words: "a documented procedure that has no
# recipe" is the other half of the rule that forbids a workflow reimplementing a tool.
#
#   just content-audit --description <file> --node <name>
#       why that node is in the build, and what references it
#   just content-audit --description <file> --node <name> --package <manifest>
#       the same, plus size by category, by asset and by install bundle
#   just content-audit --description <file> --source <path>
#       which nodes a change to that source would reach — `cy_build explain`
#
# Time by stage is printed by `just content-package` on every run rather than living here, because
# it is a property of a build that just happened and this recipe reads a build that already did.

# Why is this in the build, what references it, and what does it cost. Usage: just content-audit [--profile <p>] --description <file> (--node <name> | --source <path>) [--package <manifest>]
content-audit *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"

    # Which of the two questions was asked. Naming neither is an error rather than a default: a
    # default here would print one node's chain and look like an answer to the other question.
    asked=""
    for argument in ${rest}; do
        case "${argument}" in
            --node | --node=*)     asked="audit" ;;
            --source | --source=*) asked="${asked:-explain}" ;;
        esac
    done
    if [ -z "${asked}" ]; then
        echo "content-audit: name --node <name> or --source <path>." >&2
        echo "  --node    why that node is in the build, and what references it" >&2
        echo "  --source  which nodes a change to that file would reach" >&2
        exit 2
    fi

    just build-engine --profile "${profile}" -D CY_BUILD_TOOLS=ON
    exec "${build_dir}/tools/build/cy_build" "${asked}" ${rest}

# `--apply` switches from producing a patch to applying one. A patch application is staged, verified
# and switched atomically; interrupting it leaves the previous build playable, which is what
# `integration.build_patch` asserts by killing the process at every stage.

# Diff two package manifests into a patch, or --apply one. Usage: just content-patch [--profile <p>] --from <a> --to <b> --out <patch> | --apply --install <dir> --patch <file> --artefacts <store>
content-patch *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    just build-engine --profile "${profile}" -D CY_BUILD_TOOLS=ON

    command=patch
    passthrough=()
    for argument in ${rest}; do
        if [ "${argument}" = "--apply" ]; then
            command=apply
        else
            passthrough+=("${argument}")
        fi
    done
    exec "${build_dir}/tools/build/cy_build" "${command}" ${passthrough[@]+"${passthrough[@]}"}

# A PROJECT TO AUTHOR IN. The templates are the editor's (`cy_editor_services::Template`), so this
# recipe builds the editor and asks it rather than keeping a second copy of what a project is. The
# directory is relative to where `just` was invoked, and the recipe refuses one that already holds a
# project rather than overwriting its manifest.

# Create a project to author in, from an editor template. Usage: just content-new-project <directory> [--template empty|swift-gameplay] [--profile <p>]
content-new-project *args:
    #!/usr/bin/env bash
    set -euo pipefail
    caller="{{invocation_directory()}}"
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    argv=(${rest})
    if [ "${#argv[@]}" -eq 0 ] || [[ "${argv[0]}" == --* ]]; then
        echo "usage: just content-new-project <directory> [--template empty|swift-gameplay]" >&2
        exit 2
    fi
    directory="${argv[0]}"
    case "${directory}" in
        /*) ;;
        *) directory="${caller}/${directory}" ;;
    esac

    just build-editor --profile "${profile}"
    "$(just _editor-binary "${profile}" cyberdyne-editor)" --new-project "${directory}" "${argv[@]:1}"
    echo "==> next        just run-editor-live --project ${directory}"
