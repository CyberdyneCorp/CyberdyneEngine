# just/build.just — Build: engine, editor, tools, shaders; per profile and per platform.
#
# These recipes orchestrate. CMake and Ninja decide what is out of date, Cargo decides it for the
# editor, and the Slang toolchain for shaders; a recipe here that compared timestamps would be a
# defect against `developer-workflow-and-just`.
#
# Every recipe takes `--profile <name>` and `--platform <name>`, and reports any local override in
# effect, because a build that used a different tree or a different profile from the one the
# developer assumed is the first thing to rule out when a result makes no sense.

# Configure and build the engine. Usage: just build-engine [--profile <p>] [--platform <p>] [cmake args...]
build-engine *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    split="$(just _split {{args}})"
    IFS=$'\t' read -r profile rest <<< "$split"
    config="$(just _profile-column "$profile" 2)"
    just _report-overrides

    # The target platform is selected here and nowhere else, so `just test-unit` and
    # `just run-sample` inherit it through this recipe rather than each growing their own flag.
    target=""
    cmake_args=()
    expecting=0
    for argument in ${rest}; do
        if [ "${expecting}" -eq 1 ]; then
            target="${argument}"; expecting=0; continue
        fi
        case "${argument}" in
            --platform)   expecting=1 ;;
            --platform=*) target="${argument#*=}" ;;
            *)            cmake_args+=("${argument}") ;;
        esac
    done
    target="$(just _resolve-target "${target:-${CY_TARGET_PLATFORM:-}}")"

    # Both the profile and the configuration are passed: cmake/profiles.cmake cross-checks them, so
    # the table above cannot drift away from the one in cmake/profiles.cmake without a configure
    # error. The preset owns everything else, so building through CMake directly gives the same
    # result as building through this recipe.
    configure=(--preset "${profile}" -D "CMAKE_BUILD_TYPE=${config}")
    build=(--build --preset "${profile}")
    if [ -n "${CY_BUILD_DIR:-}" ]; then
        configure+=(-B "${CY_BUILD_DIR}")
        build=(--build "${CY_BUILD_DIR}")
    fi

    # ONE BUILD AT A TIME PER TREE. Two CMake configures in one directory do not merely race, they
    # corrupt: a half-populated FetchContent checkout looks exactly like a broken dependency, and the
    # error it produces names SDL rather than the concurrency that caused it. This project lost an
    # hour to that twice — once to orphaned processes from an interrupted gate, and once to a
    # milestone ledger running while a developer built by hand — and both times the symptom was
    # blamed on the code before the cause was found.
    #
    # A second build WAITS rather than failing: it is a legitimate thing to do, it just cannot happen
    # at the same time. The wait is announced, because a build that silently blocks looks like a
    # build that hung.
    lock_dir="${CY_BUILD_DIR:-build/${profile}}"
    mkdir -p "${lock_dir}"
    exec 9>"${lock_dir}/.cy-build.lock"
    if ! flock -n 9; then
        echo "==> waiting     another build holds ${lock_dir}; waiting for it to finish" >&2
        flock 9
    fi

    echo "==> configure   profile=${profile}  configuration=${config}  platform=${target}"
    cmake "${configure[@]}" ${cmake_args[@]+"${cmake_args[@]}"}
    echo "==> build       profile=${profile}"
    cmake "${build[@]}" ${CY_JOBS:+--parallel "${CY_JOBS}"}

# Build everything this milestone has: the engine, its tools, and the editor. Shaders join at M3.
build-all *args:
    @just build-engine {{args}}
    @just build-editor {{args}}

# Build the engine's command-line tools and code generators. CY_BUILD_TOOLS is on by default.
build-tools *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    # Several of M0's tools — the layer check, the header generators, the trace reader — are Python
    # scripts that recipes invoke directly and that nothing compiles. What this recipe builds is the
    # compiled half, under tools/, which CY_BUILD_TOOLS gates.
    just build-engine {{args}} -D CY_BUILD_TOOLS=ON

# Cargo owns the editor's compilation, as the justfile's header says: this recipe orchestrates and
# does not reimplement it. What it adds is the two things `just` is for — mapping the workflow's
# profile onto Cargo's, and putting the output where a milestone's build trees go rather than in
# editor/target/.
#
# `--generate` rewrites the SDK bindings from the ABI description before building. It is a flag
# rather than a separate recipe because generation is a step of building the editor: the alternative
# is a developer who edits `src/abi/include/cy/abi/cy_abi.h` and discovers the mismatch as a test
# failure with no obvious cause. `cargo test -p cy-editor-sdk` fails when the committed bindings are
# stale, so the currency check runs whether or not this flag was passed.

# Build the Rust editor workspace. Usage: just build-editor [--profile <p>] [--generate] [cargo args...]
build-editor *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-overrides

    generate=""
    cargo_args=()
    for argument in ${rest}; do
        case "${argument}" in
            --generate) generate=yes ;;
            *) cargo_args+=("${argument}") ;;
        esac
    done

    if [ -n "${generate}" ]; then
        echo "==> generate    the Rust SDK bindings from src/abi/include/cy/abi/cy_abi.h"
        python3 tools/gen/rust/sdk_gen.py
    fi

    cargo_profile="$(just _cargo-profile "${profile}")"
    export CARGO_TARGET_DIR="$(just _editor-target-dir)"
    # Passed to the workspace so that `tests/profiles.rs` can assert that the binary it is running
    # in was compiled with the assertions its row of the profile table declares. Without it the
    # mapping is checked as text and never as behaviour.
    export CY_EDITOR_PROFILE="${profile}"

    echo "==> cargo build profile=${profile}  cargo-profile=${cargo_profile}  target=${CARGO_TARGET_DIR}"
    cargo build --manifest-path editor/Cargo.toml --profile "${cargo_profile}" \
        ${CY_JOBS:+--jobs "${CY_JOBS}"} ${cargo_args[@]+"${cargo_args[@]}"}

# WHY THESE LIVE HERE RATHER THAN IN just/quality.just AND just/test.just. They belong there, and
# whoever owns those files should call this recipe from them — `quality-format-check`,
# `quality-lint` and `test-all` cover C and C++ only, and an editor whose Rust is linted by nobody
# is the shape of defect this project has already paid for twice. They are here because this change
# owns just/build.just and not those two, and a gate that exists in a file nobody runs is worse than
# one that exists in the wrong file.
#
# Four gates, in increasing cost:
#   * the SDK generator's own selftest, which edits the live C header in memory and checks that the
#     generator refuses what it should. It is first because it is the cheapest and because a broken
#     generator makes every failure after it harder to read.
#   * rustfmt. Note that it formats a crate by walking the module tree from the crate root and has
#     no stable way to skip a module, so it reaches the generated bindings whatever file list it is
#     given. That is why `tools/gen/rust/sdk_gen.py` writes its output THROUGH rustfmt: the
#     formatting gate and the currency gate would otherwise disagree by construction, each undoing
#     what the other requires.
#   * clippy with the workspace's lint table, which denies pedantic. Warnings are errors.
#   * the tests, which include the layering, safety and profile-mapping checks.

# Check the editor: rustfmt, clippy and its tests. Usage: just build-editor-check [--profile <p>]
build-editor-check *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-overrides

    cargo_profile="$(just _cargo-profile "${profile}")"
    export CARGO_TARGET_DIR="$(just _editor-target-dir)"
    export CY_EDITOR_PROFILE="${profile}"

    echo "==> generator   tools/gen/rust/tests/run_tests.py"
    python3 tools/gen/rust/tests/run_tests.py

    echo "==> rustfmt     the editor's hand-written sources"
    mapfile -t sources < <(just _editor-rust-files)
    rustfmt --edition 2024 --check "${sources[@]}"
    echo "rustfmt: ${#sources[@]} file(s) match editor/rustfmt.toml"

    echo "==> clippy      profile=${profile}"
    cargo clippy --manifest-path editor/Cargo.toml --profile "${cargo_profile}" \
        --all-targets ${CY_JOBS:+--jobs "${CY_JOBS}"} ${rest}

    echo "==> cargo test  profile=${profile}"
    cargo test --manifest-path editor/Cargo.toml --profile "${cargo_profile}" \
        ${CY_JOBS:+--jobs "${CY_JOBS}"} ${rest}

# Format the editor's hand-written Rust in place.
build-editor-format:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    mapfile -t sources < <(just _editor-rust-files)
    rustfmt --edition 2024 "${sources[@]}"
    echo "build-editor-format: formatted ${#sources[@]} file(s)"

# Install and select the Rust toolchain this repository declares, and print what it selected.
#
# WHY THIS IS A RECIPE AND NOT FOUR LINES IN A WORKFLOW. `just ci-check` refuses a workflow step that
# is not a recipe, and it is right to: a version pinned in a YAML file is a version a developer
# cannot reproduce and a reviewer has to read a workflow to discover. It is also the third place the
# minimum supported version would otherwise be written down — after `editor/Cargo.toml` and
# `rust-toolchain.toml` — and three copies of a number is two too many.
#
# So the version comes from `rust-toolchain.toml`, which is the file rustup itself reads, and
# `crates/cy-editor-app/tests/toolchain.rs` fails if it and `editor/Cargo.toml` ever disagree.
build-editor-toolchain:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    version="$(just _editor-toolchain)"
    echo "==> toolchain   ${version}, from rust-toolchain.toml"
    rustup toolchain install "${version}" --profile minimal --component rustfmt --component clippy
    rustup default "${version}"
    cargo --version
    rustc --version

# The toolchain rust-toolchain.toml selects.
[private]
_editor-toolchain:
    @grep -m1 '^channel' "{{root}}/rust-toolchain.toml" | sed 's/.*"\(.*\)".*/\1/'

# The Cargo profile a workflow profile maps to, from the profile table. Column 5.
[private]
_cargo-profile profile:
    @just _profile-column {{profile}} 5

# Where the editor's build output goes.
#
# `build/editor/` by default, never editor/target/: a build tree belongs under build/ with the
# others, so that `just build-clean` and a milestone's per-label trees mean the same thing for
# Cargo as they do for CMake. CY_BUILD_DIR redirects it the way it redirects the engine's.
#
# AN ABSOLUTE CY_BUILD_DIR IS A PATH, NOT A FRAGMENT. This line read
# `{{root}}/${CY_BUILD_DIR:-build}/editor` unconditionally, so an absolute override produced
# `<root>/<root>/<override>/editor` — a second copy of the tree written INSIDE the source
# directory, three gigabytes of it, while every consumer looked for the editor under the override
# itself. M10's gate found it: `just build-editor` reported success in twenty-five seconds and
# `integration.editor_session_selftest`, `smoke.editor_session` and `smoke.authoring` then failed
# with "cyberdyne-editor is not at <override>/editor/debug/cyberdyne-editor. Build it with: just
# build-editor" — the recipe that had just run. Nothing else joined: `build-engine` hands the
# override to `cmake -B`, and `_ctest`, `run-*` and `_sanitize-dir` are relative to the root they
# cd into. tools/ci/test_recipes.py holds this.
[private]
_editor-target-dir:
    #!/usr/bin/env bash
    set -euo pipefail
    directory="${CY_BUILD_DIR:-build}"
    case "${directory}" in
        /*) echo "${directory}/editor" ;;
        *)  echo "{{root}}/${directory}/editor" ;;
    esac

# The files rustfmt is pointed at.
#
# It reaches more than these: rustfmt walks each crate's module tree from its root, so naming
# src/lib.rs also formats every `mod` below it, including the generated bindings. The list excludes
# src/generated/ anyway, because naming a file rustfmt would reach regardless says something untrue
# about who owns it — and the generator, which writes its output through rustfmt, is what keeps the
# two gates agreeing.
[private]
_editor-rust-files:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    find editor -type d \( -name target -o -name generated \) -prune -o \
        -type f -name '*.rs' -print | sort

# M11.c tasks 1.5-1.7. `shader-system`'s pipeline step 4 is "SPIR-V retained (Vulkan), or translated (MSL
# for Metal, DXIL for D3D12)"; until this rung the front end emitted only the first and this recipe
# refused with a milestone label. M11.d's first task is a Metal and a D3D12 backend and cannot start
# without the other two, which is why both rungs' ledgers carry the same claim from opposite sides.
#
# EMITTING A FILE IS NOT THE CLAIM, and this recipe is written so that it cannot become one. What it
# runs compiles the SAME graph for each target and compares what the artefacts DECLARE — the entry
# point, the stage, the workgroup, and the parameters with their kinds. A stub that wrote the SPIR-V
# out twice under two names passes a file-exists check and fails every one of those. See
# tools/shaders/README.md.
#
# `--targets` prints the table and compiles nothing, which is what a caller asking "what can this
# machine do" wants; the table is PROBED — each target is answered by compiling a shader for it.

# Compile the Slang shader set for every target this build emits and compare what they declare.
build-shaders *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-overrides

    list_only=""
    tool_args=()
    for argument in ${rest}; do
        case "${argument}" in
            --targets) list_only=yes ;;
            *) tool_args+=("${argument}") ;;
        esac
    done

    # The tool is part of the engine's build, so this recipe builds it the way every other recipe
    # builds what it runs rather than asking the developer to have done it first.
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    just build-engine --profile "${profile}" > /dev/null
    tool="${build_dir}/tools/shaders/cy_shaderc"
    if [ ! -x "${tool}" ]; then
        echo "build-shaders: ${tool} was not built." >&2
        echo "  The shader front end is behind CY_SHADER_SLANG, which is off in the Profile and" >&2
        echo "  Shipping profiles because \`shader-system\` requires a shipping build to contain no" >&2
        echo "  Slang compiler. Use --profile dev, or configure with -D CY_SHADER_SLANG=ON." >&2
        exit 1
    fi

    if [ -n "${list_only}" ]; then
        echo "==> targets     ${tool}"
        exec "${tool}" targets
    fi

    echo "==> shaders     profile=${profile}  ${tool}"
    exec "${tool}" build ${tool_args[@]+"${tool_args[@]}"}

# Remove build output. Names exactly what it removes and confirms; --yes for CI. Scoped by target.
build-clean *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-overrides

    assume_yes=""
    targets=()
    for argument in ${rest}; do
        case "${argument}" in
            --yes | -y) assume_yes=yes ;;
            -*) echo "build-clean: unknown option '${argument}'" >&2
                echo "  Usage: just build-clean [--profile <p>] [--yes] [target...]" >&2
                exit 2 ;;
            *)  targets+=("${argument}") ;;
        esac
    done

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    if [ ! -d "${build_dir}" ]; then
        echo "build-clean: nothing to remove — ${build_dir} does not exist"
        exit 0
    fi

    # Scoped: naming targets removes what ninja built for them and for their inputs, and leaves the
    # rest of the tree — and the configuration — alone. Asking ninja is the whole implementation; a
    # recipe that deleted the files it believed were a target's would be reimplementing dependency
    # tracking, which the workflow specification forbids.
    if [ "${#targets[@]}" -gt 0 ]; then
        echo "==> clean       ${targets[*]}  in ${build_dir}"
        just _confirm "remove what ninja built for ${targets[*]} and its inputs" "${assume_yes}"
        ninja -C "${build_dir}" -t clean "${targets[@]}"
        exit 0
    fi

    echo "==> clean       ${build_dir}"
    echo "    this removes the whole build tree, including the fetched dependency sources:"
    du -sh "${build_dir}" 2>/dev/null || true
    just _confirm "remove ${build_dir} and everything under it" "${assume_yes}"
    rm -rf "${build_dir}"
    echo "build-clean: removed ${build_dir}"

# --- Shared implementation ------------------------------------------------------------------------

# The platform a build targets, or a diagnostic saying why this host cannot produce it.
#
# `developer-workflow-and-just` requires that a target which cannot be built on the current host is
# reported with what is required, rather than failing somewhere inside CMake. At M0 the answer is
# always the host: cross-compilation needs a toolchain file, and the first ones arrive with the
# mobile targets at M9. The seam is here so that adding one is a table entry.
[private]
_resolve-target requested:
    #!/usr/bin/env bash
    set -euo pipefail
    host="$(just _host-platform)"
    requested='{{requested}}'
    if [ -z "${requested}" ] || [ "${requested}" = "${host}" ]; then
        echo "${host}"
        exit 0
    fi
    echo "cannot build for '${requested}' on a ${host} host." >&2
    echo "  This milestone builds for the host only. Cross-compilation needs a CMake toolchain" >&2
    echo "  file and a platform SDK, and the first of those arrive with the mobile targets at M9." >&2
    echo "  Build for ${host}, or run this on a ${requested} host." >&2
    exit 2

# Reclaim the build directories nothing is using.
# Usage: just build-reap [--older-than <minutes>] [--keep <name>] [--apply]
#
# WHY THIS EXISTS. Hard rule 11 gives every agent its own build directory and NOTHING collects them.
# Twice now the tree has had to be cleared by hand — 218 GB, then 892 GB across 106 directories — and
# the second time the shortage had already blocked a criterion: `m10:four-profiles` needs four engine
# trees plus four `test-all` passes and there was no room for them. A build tree is pure derived state,
# rebuildable from a committed source tree, so the only question a reaper has to answer correctly is
# WHICH ONES ARE IN USE.
#
# It answers that three ways, and every one of them has to say yes before a directory is removed:
#   * no process has its working directory inside it (/proc/*/cwd),
#   * it has not been written to inside --older-than minutes (default 120), which is what protects a
#     tree a build is writing to from OUTSIDE this machine's process table, and
#   * it is not named by --keep.
#
# DRY RUN BY DEFAULT. It prints what it would remove and stops; `--apply` is what deletes. That is the
# opposite of most cleanup tools and deliberate: the cost of keeping a stale tree is disk, and the cost
# of removing a live one is somebody's multi-hour run.
build-reap *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"

    # BIND THE RECIPE'S ARGUMENTS TO $@ BEFORE PARSING THEM. Without this the loop below reads an
    # empty positional list, every flag is silently ignored, and the recipe reports a dry run whatever
    # it was asked for — which is what the self-test caught: --keep, --older-than and --apply all did
    # nothing. `just` interpolates {{args}} as text, it does not set $@.
    set -- {{args}}

    minutes=120
    apply=0
    keep=()
    while (($#)); do
        case "$1" in
            --older-than) minutes="$2"; shift 2 ;;
            --keep)       keep+=("$2");  shift 2 ;;
            --apply)      apply=1;       shift ;;
            *) echo "build-reap: unknown argument '$1'" >&2; exit 2 ;;
        esac
    done

    if [[ ! -d build ]]; then
        echo "build-reap: no build/ directory; nothing to reap"
        exit 0
    fi

    # A directory whose name a running process sits inside is live whatever its timestamp says.
    active=()
    while IFS= read -r cwd; do
        case "$cwd" in
            */build/*) active+=("$(basename "${cwd#*/build/}" | cut -d/ -f1)") ;;
        esac
    done < <(readlink /proc/*/cwd 2>/dev/null || true)

    reaped=0
    kept=0
    freed=0
    for path in build/*/; do
        name="$(basename "$path")"
        skip=""
        for k in ${keep[@]+"${keep[@]}"}; do
            [[ "$name" == "$k" ]] && skip="named by --keep"
        done
        for a in ${active[@]+"${active[@]}"}; do
            [[ "$name" == "$a" ]] && skip="a process is inside it"
        done
        if [[ -z "$skip" ]] && [[ -n "$(find "$path" -maxdepth 0 -mmin "-${minutes}" 2>/dev/null)" ]]; then
            skip="written to within ${minutes} minutes"
        fi

        size="$(du -sm "$path" 2>/dev/null | cut -f1)"
        if [[ -n "$skip" ]]; then
            printf '  keep    %-28s %6s MB  (%s)\n' "$name" "${size:-?}" "$skip"
            kept=$((kept + 1))
            continue
        fi
        printf '  reap    %-28s %6s MB\n' "$name" "${size:-?}"
        reaped=$((reaped + 1))
        freed=$((freed + ${size:-0}))
        if ((apply)); then
            rm -rf -- "$path"
        fi
    done

    if ((apply)); then
        echo "build-reap: removed ${reaped} directory(ies), ${freed} MB; kept ${kept}"
    else
        echo "build-reap: would remove ${reaped} directory(ies), ${freed} MB; kept ${kept}"
        echo "build-reap: this was a DRY RUN — pass --apply to remove them"
    fi
