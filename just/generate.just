# just/generate.just — Generate: ABI bindings, language overlays, reflection data, documentation.
#
# Generation is deterministic: identical inputs produce byte-identical outputs. A generated artefact
# without a check that regeneration reproduces it is a defect, which is what generate-check exists to
# prevent — and it is a merge gate, not advice.
#
# What exists at M1: cy_features.h and cy_modules.h, from the feature options and the module
# manifests; and the reflection metadata, from the annotated headers each module declares. The ABI
# headers and description (M4), the Swift overlay (M4) and the shader artefacts (M3) join these
# recipes as they land.
#
# M4 added `generate-swift`, below. It is separate from `generate-headers` on purpose: the Swift
# overlay is a function of the SOURCE — src/abi/include/cy/abi/cy_abi.h — and not of any build
# configuration, so it needs no configured build directory and it is committed beside the reflection
# metadata rather than written into a build tree. `just generate-swift --check` is the currency
# gate, and it also runs as `integration.swift_overlay` under `just test-all`, so a stale overlay
# fails for whoever builds the engine rather than only for whoever remembers this recipe.
#
# The two generators write to different places, for a reason worth stating. cy_features.h and
# cy_modules.h are a function of the *configuration*, so they belong in the build directory and two
# configurations of one checkout have different ones. The reflection metadata is a function of the
# *source*, and it encodes identifiers from the committed manifest, so it is committed beside it —
# an identity change is then a reviewable diff rather than something that happens in a build tree.

# Regenerate every generated artefact. Usage: just generate-headers [--profile <name>]
generate-headers *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    just _generated-inputs "${build_dir}" >/dev/null

    # --rename and --tombstone are the two reviewed manifest edits, and they belong to the
    # reflection generator alone. Split them out rather than handing every generator every flag: the
    # diagnostic that offers them names this recipe, so this recipe has to accept them.
    passthrough=()
    reflect_only=()
    argv=(${rest})
    index=0
    while [ ${index} -lt ${#argv[@]} ]; do
        case "${argv[${index}]}" in
            --rename|--tombstone)
                reflect_only+=("${argv[${index}]}" "${argv[$((index + 1))]}")
                index=$((index + 2))
                ;;
            *)
                passthrough+=("${argv[${index}]}")
                index=$((index + 1))
                ;;
        esac
    done

    echo "==> features    ${build_dir}/generated/include"
    python3 tools/gen/generate_headers.py \
        --input "${build_dir}/generated/cy_generated_inputs.txt" \
        --output-dir "${build_dir}/generated/include" ${passthrough[@]+"${passthrough[@]}"}

    echo "==> reflection  src/core/reflect/generated, identity/manifest.toml"
    reflect_inputs="$(just _reflect-inputs "${build_dir}")"
    mapfile -t reflect_args <<< "${reflect_inputs}"
    python3 tools/gen/reflect_gen.py --source-root . "${reflect_args[@]}" \
        --cache "${build_dir}/generated/cy_reflect_cache.json" \
        ${reflect_only[@]+"${reflect_only[@]}"} ${passthrough[@]+"${passthrough[@]}"}

# Assert generated output is current and that regeneration is reproducible. Fails naming the drift.
generate-check *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    inputs="$(just _generated-inputs "${build_dir}")"

    echo "==> currency    ${build_dir}/generated/include"
    python3 tools/gen/generate_headers.py --input "${inputs}" \
        --output-dir "${build_dir}/generated/include" --check

    # Determinism is the other half of the requirement, and it cannot be checked against a single
    # output: generate twice into fresh directories and diff.
    echo "==> determinism two runs on the same input"
    scratch="$(mktemp -d)"
    trap 'rm -rf "${scratch}"' EXIT
    python3 tools/gen/generate_headers.py --input "${inputs}" --output-dir "${scratch}/first" >/dev/null
    python3 tools/gen/generate_headers.py --input "${inputs}" --output-dir "${scratch}/second" >/dev/null
    diff -r "${scratch}/first" "${scratch}/second"

    # The reflection metadata is committed, so its currency check is over the source tree. Staleness
    # here means an annotated header was edited without the generator being run — which is a merge
    # gate, not a local inconvenience: the identifiers in the generated code are the contract.
    echo "==> reflection  src/core/reflect/generated"
    reflect_inputs="$(just _reflect-inputs "${build_dir}")"
    mapfile -t reflect_args <<< "${reflect_inputs}"
    python3 tools/gen/reflect_gen.py --source-root . "${reflect_args[@]}" \
        --cache "${scratch}/reflect-check.json" --check

    # And the same two-run diff, against a copy of the manifest so that neither run can append to
    # the committed one.
    echo "==> determinism two reflection runs, two output directories"
    cp identity/manifest.toml "${scratch}/manifest-a.toml"
    cp identity/manifest.toml "${scratch}/manifest-b.toml"
    python3 tools/gen/reflect_gen.py --source-root . "${reflect_args[@]}" \
        --manifest "${scratch}/manifest-a.toml" --output-dir "${scratch}/reflect-a" \
        --cache "${scratch}/reflect-a.json" --quiet
    python3 tools/gen/reflect_gen.py --source-root . "${reflect_args[@]}" \
        --manifest "${scratch}/manifest-b.toml" --output-dir "${scratch}/reflect-b" \
        --cache "${scratch}/reflect-b.json" --quiet
    diff -r "${scratch}/reflect-a" "${scratch}/reflect-b"
    diff "${scratch}/manifest-a.toml" "${scratch}/manifest-b.toml"
    grep -rL "" "${scratch}/reflect-a" >/dev/null 2>&1 || true
    if grep -rl "${PWD}" "${scratch}/reflect-a" >/dev/null 2>&1; then
        echo "generated reflection metadata carries an absolute path" >&2
        exit 1
    fi
    echo "generation is reproducible"

# Regenerate the CyberdyneKit Swift overlay from the C ABI. Pass --check to assert it is current.
#
# `swift-scripting`: "The overlay in `CyberdyneCore` SHALL be **generated** from the same ABI
# description that produces the C headers, so the two cannot drift", and "Generated code SHALL be
# committed to the repository so consumers do not need the generator, and CI SHALL verify it is up
# to date."
#
# It needs no Swift toolchain: the generator is Python over the ABI description, and the check is a
# diff over committed text. That is deliberate — the overlay's currency is everybody's problem, not
# only that of the machines that can build Swift.
generate-swift *args:
    @cd "{{root}}" && python3 tools/gen/swift/overlay_gen.py {{args}}

# Test the generators, the feature options and the module graph: dependencies, cycles, exclusion.
generate-test *args:
    @cd "{{root}}" && python3 tools/gen/tests/run_tests.py {{args}}

# The reflection generator's argument vector, one argument per line, written by
# src/core/reflect/CMakeLists.txt at configure time. Read rather than restated: the list of
# annotated headers belongs to the module that declares them, and a second copy here would be a
# recipe generating a different set of types than the build compiles.
[private]
_reflect-inputs build_dir:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    inputs="{{build_dir}}/generated/cy_reflect_inputs.txt"
    if [ ! -f "${inputs}" ]; then
        echo "no configured build in {{build_dir}}: ${inputs} does not exist." >&2
        echo "  Configure first: just build-engine" >&2
        exit 2
    fi
    cat "${inputs}"

# The generator's input for a build directory, or a diagnostic naming what to run first.
[private]
_generated-inputs build_dir:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    inputs="{{build_dir}}/generated/cy_generated_inputs.txt"
    if [ ! -f "${inputs}" ]; then
        echo "no configured build in {{build_dir}}: ${inputs} does not exist." >&2
        echo "  Configure first: just build-engine" >&2
        exit 2
    fi
    printf '%s\n' "${inputs}"
