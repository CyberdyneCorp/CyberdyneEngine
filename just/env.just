# just/env.just — Environment: dependency check and diagnosis, toolchain setup, first-time bootstrap.
#
# A failure caused by a missing or wrong-version tool belongs here, reported with the found version,
# the required version and the correction — not surfacing as a build error from a nested tool.
#
# Every version this file enforces is read from the file that owns it: the compiler matrix comes out
# of cmake/compilers.cmake, so the doctor and the configure-time check cannot disagree. A minimum
# written twice is a minimum that will be raised once.

# Diagnose the development environment: version, path and status for every tool, with the correction.
env-doctor *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"

    # ---- the tools this milestone requires -------------------------------------------------------
    # name | binary | minimum ('-' when any version does) | what it is for
    required='
    cmake    | cmake    | 3.28 | configures and builds the engine
    ninja    | ninja    | 1.11 | the generator every preset in CMakePresets.json names
    git      | git      | 2.30 | source control, and how the dependencies are fetched
    python3  | python3  | 3.10 | the generators, the layer check, and the trace tools
    just     | just     | 1.21 | this workflow
    openspec | openspec | -    | validates openspec/specs/; just quality-specs is a merge gate
    '

    # The pinned LLVM tooling. Required rather than advisory since M1, and pinned to one version
    # rather than to a minimum: clang-format 18 and 22 disagree about three sites in this tree with
    # no spelling that satisfies both, and clang-tidy 18 and 22 do not report the same findings over
    # the same sources. A merge gate whose verdict depends on which version the machine happened to
    # have is not a gate, so `just quality-format-check` and `just quality-lint` refuse a tool of
    # the wrong major and this report says so first.
    #
    # name | what the gate is | the recipes' override for it
    pinned='
    clang-format | just quality-format-check and its merge gate | CY_CLANG_FORMAT
    clang-tidy   | just quality-lint and its merge gate         | CY_CLANG_TIDY
    '

    platform="$(just _host-platform)"

    status=0
    problems=()

    # Print one report line. Status is ok, old, missing or advisory; the caller has already decided.
    report() {
        printf '  %-9s %-13s %-12s %s\n' "$1" "$2" "$3" "$4"
    }

    # The first dotted number in a tool's own version output. Pure bash: a pipeline into head under
    # `set -o pipefail` fails on SIGPIPE, which would turn a healthy tool into a doctor crash.
    first_version() {
        if [[ "$1" =~ ([0-9]+(\.[0-9]+)+) ]]; then
            printf '%s' "${BASH_REMATCH[1]}"
        fi
    }

    check_tool() {
        local name="$1" binary="$2" minimum="$3" purpose="$4" kind="$5"
        local path version
        if ! path="$(command -v "${binary}" 2>/dev/null)"; then
            report "${kind}" "${name}" "-" "not on PATH"
            just _doctor-hint "${name}" "${platform}" "${purpose}"
            if [ "${kind}" = missing ]; then
                problems+=("${name}")
                status=1
            fi
            return 0
        fi
        version="$(first_version "$("${binary}" --version 2>&1 || true)")"
        if [ -n "${minimum}" ] && [ "${minimum}" != "-" ] && \
           ! just _version-at-least "${version:-0}" "${minimum}"; then
            report old "${name}" "${version:-unknown}" "${path}  (needs ${minimum} or later)"
            just _doctor-hint "${name}" "${platform}" "${purpose}"
            problems+=("${name}")
            status=1
            return 0
        fi
        report ok "${name}" "${version:-present}" "${path}"
    }

    echo "CyberdyneEngine — environment report"
    printf '  %-9s %s\n' host "${platform} $(uname -m 2>/dev/null || echo unknown)"
    echo

    while IFS='|' read -r name binary minimum purpose; do
        [ -n "${name// /}" ] || continue
        check_tool "${name// /}" "${binary// /}" "${minimum// /}" \
                   "$(echo "${purpose}" | sed 's/^ *//; s/ *$//')" missing
    done <<< "${required}"

    # ---- the compiler ----------------------------------------------------------------------------
    # cmake/compilers.cmake is the matrix. Reading it here is what keeps `just env-doctor` and the
    # configure-time diagnostic from drifting apart.
    read -r compiler_id compiler_version compiler_path <<< "$(just _host-compiler)"
    if [ "${compiler_id}" = none ]; then
        report missing compiler "-" "no C++ compiler found (set CXX to name one)"
        just _doctor-hint compiler "${platform}" "builds the engine"
        problems+=(compiler)
        status=1
    else
        minimum="$(sed -n "s/^set(CY_MINIMUM_${compiler_id}[[:space:]]\{1,\}\([0-9.]*\)).*/\1/p" \
            cmake/compilers.cmake)"
        if [ -n "${minimum}" ] && ! just _version-at-least "${compiler_version}" "${minimum}"; then
            report old "${compiler_id}" "${compiler_version}" \
                   "${compiler_path}  (needs ${minimum} or later)"
            just _doctor-hint compiler "${platform}" "builds the engine"
            problems+=("${compiler_id}")
            status=1
        else
            report ok "${compiler_id}" "${compiler_version}" \
                   "${compiler_path}  (minimum ${minimum:-unrecorded})"
        fi
    fi

    # ---- the pinned LLVM tooling -----------------------------------------------------------------
    # Resolved through `just _tool-resolve`, which is the same code path `just quality-lint` and
    # `just quality-format-check` take. The doctor cannot report a version the gates would not run,
    # and an override that names the wrong major is reported here rather than discovered by a gate
    # that disagrees with the one continuous integration ran.
    check_pinned() {
        local name="$1" purpose="$2" override_name="$3"
        local state version path
        IFS=$'\t' read -r state version path \
            <<< "$(just _tool-resolve "${name}" "${!override_name:-}")"
        case "${state}" in
            ok)
                report ok "${name}" "${version}" "${path}  (pinned LLVM {{llvm_pin}})"
                ;;
            wrong)
                report old "${name}" "${version}" \
                       "${path}  (needs LLVM {{llvm_pin}}, pinned at {{llvm_pin_version}})"
                just _doctor-hint "${name}" "${platform}" "${purpose}"
                problems+=("${name}")
                status=1
                ;;
            *)
                report missing "${name}" "-" "not on PATH"
                just _doctor-hint "${name}" "${platform}" "${purpose}"
                problems+=("${name}")
                status=1
                ;;
        esac
    }

    echo
    while IFS='|' read -r name purpose override_name; do
        [ -n "${name// /}" ] || continue
        check_pinned "${name// /}" "$(echo "${purpose}" | sed 's/^ *//; s/ *$//')" \
                     "${override_name// /}"
    done <<< "${pinned}"
    just _doctor-window-system "${platform}"

    # ---- what this milestone deliberately does not check ------------------------------------------
    # design.md §9: the Rust and Swift toolchains are M5 and M4, the shader compiler is M3. Their
    # profile columns are reserved and unused; checking for a toolchain the build cannot use yet
    # would report a problem nobody has.
    echo
    echo "  not checked yet, and why:"
    echo "    rust, cargo     M5 — the editor workspace"
    echo "    swift           M4 — the Swift overlay over the C ABI"
    echo "    slangc          M3 — the shader toolchain"
    echo "    validation      M3 — graphics validation layers arrive with the renderer"
    echo "    platform SDKs   M9, M10 — mobile and console-class targets"

    echo
    just _report-overrides
    if [ "${status}" -eq 0 ]; then
        echo "env-doctor: the environment satisfies M0"
    else
        echo "env-doctor: ${#problems[@]} problem(s) — ${problems[*]}" >&2
    fi
    exit "${status}"

# First-time setup for a fresh clone: diagnose, write a local settings file, populate dependencies.
env-bootstrap *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-overrides

    build=1
    for argument in ${rest}; do
        case "${argument}" in
            --no-build) build=0 ;;
            *) echo "env-bootstrap: unknown argument '${argument}'" >&2
               echo "  Usage: just env-bootstrap [--profile <name>] [--no-build]" >&2
               exit 2 ;;
        esac
    done

    # Idempotent by construction: every step below either reports what is already true or performs a
    # step whose second run is a no-op. Re-running after a failure is the supported way to recover.
    echo "==> environment"
    just env-doctor

    echo
    echo "==> local settings  .just.local"
    if [ -f .just.local ]; then
        echo ".just.local: already present, left alone"
    else
        cat > .just.local <<'SETTINGS'
    # Local workflow settings for this clone. Git-ignored: this file is how a contributor configures
    # local paths, and editing a shared file to do it is a defect against developer-workflow-and-just.
    #
    # Every line is NAME=value, and an exported environment variable wins over the value here.
    # `just env-overrides` reports which of these are in effect.
    #
    # CY_PROFILE=dev                 the profile a recipe uses when none is named
    # CY_BUILD_DIR=build/dev         a build tree other than the preset's build/<profile>/
    # CY_JOBS=8                      build and test parallelism
    # CY_TARGET_PLATFORM=linux       the platform builds target; must be the host until M9
    # The quality gates run at the LLVM major the justfile pins. These name a binary in an
    # unusual place; they do not exempt it from that major, and env-doctor reports one that is.
    # CY_CLANG_FORMAT=/usr/lib/llvm-22/bin/clang-format
    # CY_CLANG_TIDY=/usr/lib/llvm-22/bin/clang-tidy
    SETTINGS
        sed -i.bak 's/^    //' .just.local && rm -f .just.local.bak
        echo ".just.local: written, every setting commented out"
    fi

    echo
    if [ "${build}" -eq 0 ]; then
        echo "==> dependencies  skipped (--no-build)"
        echo "    Fetch and build them with: just build-engine --profile ${profile}"
    else
        echo "==> dependencies and first build  profile=${profile}"
        # The dependency set comes from deps/manifest.toml through cmake/dependencies.cmake, so
        # configuring is what populates it. Bootstrapping through the build recipe rather than
        # around it keeps this from becoming a second copy of the build.
        just build-engine --profile "${profile}"
    fi

    echo
    echo "env-bootstrap: ready. Next: just build-engine, just test-unit, just run-sample empty"

# Report the local overrides in effect and every override this workflow understands.
env-overrides:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    echo "Local overrides — set them in .just.local, or export them for one shell."
    echo "An exported variable wins over the file; neither requires editing a shared workflow file."
    echo
    printf '  %-18s %s\n' \
        CY_PROFILE          'the profile a recipe uses when none is named' \
        CY_BUILD_DIR        'a build tree other than the preset. build/<profile>/' \
        CY_JOBS             'build and test parallelism' \
        CY_TARGET_PLATFORM  'the platform builds target; must be the host until M9' \
        CY_TEST_JUNIT       'a file for machine-readable test results' \
        CY_CLANG_FORMAT     'the clang-format binary the quality gates use' \
        CY_CLANG_TIDY       'the clang-tidy binary the quality gates use'
    echo
    active="$(just _overrides)"
    if [ -z "${active}" ]; then
        echo "in effect: none — this clone is running the defaults"
    else
        echo "in effect:"
        printf '  %s\n' ${active}
    fi

# --- Shared implementation ------------------------------------------------------------------------

# linux, macos or windows: the key the install hints are indexed by.
[private]
_host-platform:
    #!/usr/bin/env bash
    set -euo pipefail
    case "$(uname -s 2>/dev/null || echo unknown)" in
        Linux*)                     echo linux ;;
        Darwin*)                    echo macos ;;
        MINGW* | MSYS* | CYGWIN*)   echo windows ;;
        *)                          echo unknown ;;
    esac

# The correction for a missing or too-old tool, for this host. Never merely "it is absent".
#
# clang-format and clang-tidy give the same command on all three hosts, and it is a `pip install` of
# an exact version rather than a distribution package. That is the point of the pin: `apt install
# clang-format` on Ubuntu 24.04 is 18.1.3, `brew install llvm` is whatever is current, and the two
# do not format this tree the same way. The PyPI wheels carry the upstream LLVM binaries for all
# three platforms at a version anyone can name, need no root, and are the same command
# .github/workflows/ci.yml runs — which is what `developer-workflow-and-just` means by continuous
# integration using the same pinned versions as developers.
[private]
_doctor-hint tool platform purpose:
    #!/usr/bin/env bash
    set -euo pipefail
    # tool | linux | macos | windows
    hints='
    cmake        | sudo apt install cmake                | brew install cmake  | winget install Kitware.CMake
    ninja        | sudo apt install ninja-build          | brew install ninja  | winget install Ninja-build.Ninja
    git          | sudo apt install git                  | xcode-select --install | winget install Git.Git
    python3      | sudo apt install python3              | brew install python | winget install Python.Python.3.12
    just         | see https://just.systems/man/en/packages.html | brew install just | winget install Casey.Just
    openspec     | npm install -g @fission-ai/openspec    | npm install -g @fission-ai/openspec | npm install -g @fission-ai/openspec
    compiler     | sudo apt install g++-13               | xcode-select --install | Visual Studio 2022 17.8 or later, or winget install LLVM.LLVM
    clang-format | pip install clang-format=={{llvm_pin_version}} | pip install clang-format=={{llvm_pin_version}} | pip install clang-format=={{llvm_pin_version}}
    clang-tidy   | pip install clang-tidy=={{llvm_pin_version}}   | pip install clang-tidy=={{llvm_pin_version}}   | pip install clang-tidy=={{llvm_pin_version}}
    '
    column=2
    case '{{platform}}' in
        linux)   column=2 ;;
        macos)   column=3 ;;
        windows) column=4 ;;
    esac
    fix="$(printf '%s\n' "${hints}" | awk -F'|' -v tool='{{tool}}' -v n="${column}" \
        '{ key = $1; gsub(/[ \t]/, "", key) }
         key == tool { v = $n; gsub(/^[ \t]+|[ \t]+$/, "", v); print v }')"
    echo "             {{purpose}}."
    if [ -n "${fix}" ]; then
        echo "             install it with: ${fix}"
    else
        echo "             no recorded install command for this host; install it and re-run."
    fi

# The compiler CMake will pick, as `id version path`, or `none - -`.
[private]
_host-compiler:
    #!/usr/bin/env bash
    set -euo pipefail
    candidates=(${CXX:-} c++ g++ clang++ cl)
    for candidate in "${candidates[@]}"; do
        [ -n "${candidate}" ] || continue
        path="$(command -v "${candidate}" 2>/dev/null)" || continue
        # cl has no --version; it writes its banner to stderr when invoked with no input.
        output="$("${candidate}" --version 2>&1 || "${candidate}" 2>&1 || true)"
        case "${output}" in
            *'Apple clang'*)   id=AppleClang ;;
            *clang*|*Clang*)   id=Clang ;;
            *'Free Software Foundation'*|*'(GCC)'*|*g++*) id=GNU ;;
            *'Microsoft (R) C/C++'*) id=MSVC ;;
            *)                 id=Unknown ;;
        esac
        version=unknown
        if [[ "${output}" =~ ([0-9]+(\.[0-9]+)+) ]]; then
            version="${BASH_REMATCH[1]}"
        fi
        echo "${id} ${version} ${path}"
        exit 0
    done
    echo "none - -"

# True when version {{found}} is at least {{minimum}}. Dotted components, numerically, left to right.
[private]
_version-at-least found minimum:
    @awk -v a='{{found}}' -v b='{{minimum}}' 'BEGIN { \
        na = split(a, x, "."); nb = split(b, y, "."); \
        n = (na > nb ? na : nb); \
        for (i = 1; i <= n; i++) { \
            u = (i <= na ? x[i] + 0 : 0); v = (i <= nb ? y[i] + 0 : 0); \
            if (u > v) exit 0; \
            if (u < v) exit 1; \
        } \
        exit 0 }'

# The window-system headers SDL3 compiles its desktop backends against. Advisory: without them the
# engine still builds and the headless display server still runs, which is what CI uses.
[private]
_doctor-window-system platform:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ '{{platform}}' != linux ]; then
        exit 0
    fi
    found=()
    [ -f /usr/include/X11/Xlib.h ] && found+=(x11) || true
    [ -f /usr/include/wayland-client.h ] && found+=(wayland) || true
    if [ "${#found[@]}" -eq 0 ]; then
        printf '  %-9s %-13s %-12s %s\n' advisory 'window system' '-' \
            'no X11 or Wayland headers'
        echo "             SDL3 will build with no desktop backend; only --headless will run."
        echo "             install them with: sudo apt install libx11-dev libwayland-dev"
    else
        printf '  %-9s %-13s %-12s %s\n' ok 'window system' "${found[*]}" \
            'SDL3 can build a desktop backend'
    fi
