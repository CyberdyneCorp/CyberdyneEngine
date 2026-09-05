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

# Build everything this milestone has: the engine and its tools. Editor and shaders join at M5 and M3.
build-all *args:
    @just build-engine {{args}}

# Build the engine's command-line tools and code generators. CY_BUILD_TOOLS is on by default.
build-tools *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    # Several of M0's tools — the layer check, the header generators, the trace reader — are Python
    # scripts that recipes invoke directly and that nothing compiles. What this recipe builds is the
    # compiled half, under tools/, which CY_BUILD_TOOLS gates.
    just build-engine {{args}} -D CY_BUILD_TOOLS=ON

# Build the Rust editor workspace against the engine library. No editor exists before M5.
build-editor *args:
    @just _not-implemented build-editor 'M5 — the Rust editor workspace'

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
