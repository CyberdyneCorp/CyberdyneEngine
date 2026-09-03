# just/generate.just — Generate: ABI bindings, language overlays, reflection data, documentation.
#
# Generation is deterministic: identical inputs produce byte-identical outputs. A generated artefact
# without a check that regeneration reproduces it is a defect, which is what generate-check exists to
# prevent — and it is a merge gate, not advice.
#
# What exists at M0: cy_features.h and cy_modules.h, from the feature options and the module
# manifests. The ABI headers and description (M4), the Swift overlay (M4), the reflection metadata
# (M1) and the shader artefacts (M3) join these recipes as they land.

# Regenerate every generated artefact. Usage: just generate-headers [--profile <name>]
generate-headers *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    just _generated-inputs "${build_dir}" >/dev/null
    python3 tools/gen/generate_headers.py \
        --input "${build_dir}/generated/cy_generated_inputs.txt" \
        --output-dir "${build_dir}/generated/include" ${rest}

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
    echo "generation is reproducible"

# Test the generators, the feature options and the module graph: dependencies, cycles, exclusion.
generate-test *args:
    @cd "{{root}}" && python3 tools/gen/tests/run_tests.py {{args}}

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
