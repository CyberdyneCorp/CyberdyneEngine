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
[private]
_editor-target-dir:
    @echo "{{root}}/${CY_BUILD_DIR:-build}/editor"

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

# Compile the Slang shader set for a profile. No shader toolchain exists before M3.
build-shaders *args:
    @just _not-implemented build-shaders 'M3 — the renderer and its shader toolchain'

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
