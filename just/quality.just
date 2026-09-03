# just/quality.just — Quality: format, lint, layering, spec validation. Every recipe here is a merge
# gate, and a gate that has gone green stays green.
#
# The two source-level gates take optional paths: `just quality-format tests` formats one directory.
# With no paths they cover the engine tree, which is what CI runs — a gate that only ever ran over
# what the author remembered to name would not be one.
#
# clang-format and clang-tidy are found on PATH, or named by CY_CLANG_FORMAT and CY_CLANG_TIDY. Their
# version matters: two versions of clang-format disagree about some files, so a formatting gate is
# also a version check. `just env-doctor` reports the version it found.

# Format the source tree in place. Usage: just quality-format [paths...]
quality-format *paths:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    formatter="$(just _tool clang-format "${CY_CLANG_FORMAT:-}")"
    mapfile -t files < <(just _cpp-files {{paths}})
    if [ ${#files[@]} -eq 0 ]; then
        echo "quality-format: no C++ sources under $(just _cpp-roots {{paths}})"
        exit 0
    fi
    "${formatter}" -i "${files[@]}"
    echo "quality-format: formatted ${#files[@]} file(s) with $("${formatter}" --version)"

# Check formatting without modifying anything. Fails naming every file that differs.
quality-format-check *paths:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    formatter="$(just _tool clang-format "${CY_CLANG_FORMAT:-}")"
    mapfile -t files < <(just _cpp-files {{paths}})
    if [ ${#files[@]} -eq 0 ]; then
        echo "quality-format-check: no C++ sources under $(just _cpp-roots {{paths}})"
        exit 0
    fi
    echo "==> clang-format ${#files[@]} file(s)"
    if ! "${formatter}" --dry-run -Werror "${files[@]}"; then
        echo >&2
        echo "quality-format-check: formatting differs from .clang-format." >&2
        echo "  Fix it with: just quality-format" >&2
        exit 1
    fi
    echo "formatting matches .clang-format"

# Run clang-tidy over the engine tree. Needs a configured build; minutes on a cold run.
quality-lint *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile paths <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    linter="$(just _tool clang-tidy "${CY_CLANG_TIDY:-}")"

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    database="${build_dir}/compile_commands.json"
    if [ ! -f "${database}" ]; then
        echo "quality-lint: no compile database at ${database}." >&2
        echo "  clang-tidy needs the flags every file is compiled with; configure first:" >&2
        echo "      just build-engine --profile ${profile}" >&2
        exit 1
    fi

    # Lint what the build actually compiles, restricted to the engine's own sources: a fetched
    # dependency's translation units are in the database too, and they are not this project's to fix.
    mapfile -t files < <(just _lint-files "${database}" ${paths})
    if [ ${#files[@]} -eq 0 ]; then
        echo "quality-lint: the compile database contains no engine sources yet." >&2
        exit 1
    fi

    echo "==> clang-tidy ${#files[@]} file(s) against ${database}"
    printf '%s\n' "${files[@]}" \
        | xargs -P "${CY_JOBS:-$(nproc)}" -n 1 "${linter}" -p "${build_dir}" --quiet
    echo "clang-tidy: no findings"

# The two halves of the layer rule are checked in different places, so this recipe runs both.
#
# layercheck.py reads the tree: every #include points at its own layer or below, no SDL header
# appears outside platform/, and no target is declared outside cy_add_module(). It is the check that
# fires in practice, and it is cheap enough to run on every pull request.
#
# selftest.py runs the fixtures under tools/layercheck/fixtures/, and that is where the
# *configure-time* half is exercised: cy_add_module() refusing an upward link is proved by a project
# whose configure must fail, not asserted in prose. It also catches enforcement that has silently
# stopped firing, because a fixture that passes is a test that fails.
#
# CI invokes this recipe and nothing else, so there is no second copy of the gate to keep in step.

# Check the layer order: declared links and source-level includes, including the platform/ SDL rule.
quality-layers *args:
    cd "{{root}}" && python3 tools/layercheck/layercheck.py {{args}}
    cd "{{root}}" && python3 tools/layercheck/selftest.py

# The identity gate. `core-type-system` requires a check that "diffs the manifest and fails when an
# existing entry's identifier changes, so an accidental identity change is a red build rather than a
# corrupted save months later". This is that check, and it is a merge gate.
#
# Three failures it produces, each naming the entry and its identifier:
#
#   * a declaration in the tree with no identifier in the manifest — run `just generate-headers`;
#   * a live entry in the manifest that has left the tree — a rename or a removal, and the generator
#     will not guess which, so it names both fixes;
#   * an identifier that has moved since the revision being diffed against, or that has been
#     recycled, or a counter lowered below a number already issued.
#
# It reads the annotated headers the *build* declares, from the argument vector
# src/core/reflect/CMakeLists.txt writes at configure time, so it cannot check a different set of
# types than the build compiles.

# Check type and field identity: nothing unassigned, nothing removed silently, no identifier moved.
quality-identity *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    reflect_inputs="$(just _reflect-inputs "${build_dir}")"
    mapfile -t reflect_args <<< "${reflect_inputs}"
    echo "==> identity    identity/manifest.toml against the tree and against git"
    python3 tools/gen/reflect_gen.py --source-root . "${reflect_args[@]}" \
        --cache "${build_dir}/generated/cy_reflect_gate.json" --gate ${rest}

# Validate the specification set: openspec validate --specs --strict. Seconds.
quality-specs *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    if ! command -v openspec >/dev/null 2>&1; then
        echo "quality-specs: openspec is not on PATH." >&2
        echo "  It is the tool that validates openspec/specs/, and a specification that does not" >&2
        echo "  validate is not a specification. Install it with:" >&2
        echo "      npm install -g @fission-ai/openspec" >&2
        exit 1
    fi
    echo "==> openspec validate --specs --strict"
    openspec validate --specs --strict {{args}}

# --- Shared implementation --------------------------------------------------------------------------

# Resolve a tool: the override if one is given, otherwise the name on PATH. Fails naming both.
[private]
_tool name override:
    #!/usr/bin/env bash
    set -euo pipefail
    tool="{{override}}"
    if [ -n "${tool}" ]; then
        just _report-override "CY_$(echo {{name}} | tr 'a-z-' 'A-Z_')" "${tool}"
    else
        tool="{{name}}"
    fi
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "{{name}} is not on PATH (tried '${tool}')." >&2
        echo "  Debian and Ubuntu: sudo apt install clang-format clang-tidy" >&2
        echo "  macOS:             brew install llvm" >&2
        echo "  Windows:           winget install LLVM.LLVM" >&2
        echo "  Or name one: CY_CLANG_FORMAT=/path/to/clang-format just quality-format" >&2
        exit 1
    fi
    command -v "${tool}"

# The directories the source gates cover when none are named. bindings/ and editor/ are here so that
# they are covered the day they gain a file; a root that does not exist is skipped.
[private]
_cpp-roots *paths:
    @[ -n "{{paths}}" ] && echo "{{paths}}" || echo "src platform modules tests benchmarks samples editor bindings tools"

# Every C and C++ file under those roots, one per line. Build trees are excluded by name rather than
# by location, because a developer's build/ is not always where the presets put it.
#
# tools/layercheck/fixtures/ is excluded for the same reason tools/layercheck/layercheck.py excludes
# it from its own scan: those files are inputs to a checker rather than engine code, and several are
# deliberately malformed.
[private]
_cpp-files *paths:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    for target in $(just _cpp-roots {{paths}}); do
        [ -e "${target}" ] || continue
        find "${target}" \
            \( -type d \( -name 'build*' -o -name '.git' -o -name '_deps' \) -prune \) -o \
            \( -type d -path '*/layercheck/fixtures' -prune \) -o \
            -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' -o -name '*.c' \
                     -o -name '*.h' -o -name '*.hpp' -o -name '*.inl' \) -print
    done | sort -u

# The translation units in the compile database that belong to this repository. clang-tidy needs the
# compile command for a file, so the database decides what can be linted; the roots decide what
# should be.
[private]
_lint-files database *paths:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    roots="$(just _cpp-roots {{paths}})"
    python3 - "{{database}}" ${roots} <<'PY'
    import json, os, sys

    database, roots = sys.argv[1], tuple(root.rstrip("/") + "/" for root in sys.argv[2:])
    repository = os.getcwd()
    seen = set()
    for entry in json.load(open(database, encoding="utf-8")):
        path = os.path.relpath(os.path.realpath(entry["file"]), repository)
        if path.startswith(roots) and path not in seen:
            seen.add(path)
            print(path)
    PY

