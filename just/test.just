# just/test.just — Test, graduated by cost. Each recipe states its expected duration; the fast set
# exceeding its budget is a regression to correct, not a new normal.
#
# The graduation is the taxonomy in `testing-and-quality`, and it is data rather than convention:
# tests/CMakeLists.txt gives every suite its kind as a CTest label and its budget as a compiled-in
# constant, so these recipes select a suite by label and nothing here can drift away from the table.
#
# Every recipe builds first. A test recipe that runs whatever binaries happen to be lying in the
# build tree reports on a revision nobody has, and CMake and Ninja decide what is out of date — a
# recipe that tried to would be reimplementing incremental logic, which the workflow specification
# forbids.

# Unit tests: sub-millisecond, no I/O. Under a minute including the build. Run on every save.
test-unit *args:
    @just _ctest unit {{args}}

# Integration tests: subsystems together, a second each. Seconds to a couple of minutes. Before pushing.
test-integration *args:
    @just _ctest integration {{args}}

# Smoke tests: the samples, headless, asserted to exit cleanly. Thirty seconds each; a minute or two.
test-smoke *args:
    @just _ctest smoke {{args}}

# Render tests: conventions asserted on a device, and golden images. Five seconds each.
#
# They SKIP loudly rather than fail on a machine with no GPU, and the suite is not built at all
# without a rendering backend — so this recipe is green on a continuous integration machine with no
# device, and says so, which is the only arrangement under which anybody leaves it in the set.
test-render *args:
    @just _ctest render {{args}}

# Every suite: unit, integration, smoke, render, and anything registered outside them. What CI runs.
test-all *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    failed=()
    # `unclassified` is the fourth pass, and it is not a suite: it runs every test that carries none
    # of the three labels. Without it, a test registered with a bare add_test() would be run by
    # nobody — invisible to every recipe here and green in CI by never executing. Running it is the
    # honest half; the recipe also names it, because a test with no label is a test with no budget
    # and the taxonomy is where it belongs.
    for kind in unit integration smoke render unclassified; do
        # Not `set -e`: a failing suite must not hide the state of the two after it. CI reads the
        # summary below, and a developer who has broken two suites should learn that in one run.
        if ! just _ctest "${kind}" {{args}}; then
            failed+=("${kind}")
        fi
    done
    echo
    if [ ${#failed[@]} -eq 0 ]; then
        echo "test-all: unit, integration, smoke, render and the unclassified pass all green"
    else
        echo "test-all: failed — ${failed[*]}" >&2
        exit 1
    fi

# Benchmarks, compared against benchmarks/baseline.json. Pass --record to rewrite it. A minute.
test-bench *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    # --record is this recipe's, not the runner's: it rewrites the committed thresholds, which is a
    # reviewed step, so it is separated out here rather than passed through.
    record=""
    runner_args=()
    for argument in ${rest}; do
        if [ "${argument}" = "--record" ]; then
            record="--record"
        else
            runner_args+=("${argument}")
        fi
    done

    if [ "$(just _profile-column "${profile}" 3)" = "on" ]; then
        echo "note: profile '${profile}' has assertions on. Measurements from it are not shipping" >&2
        echo "      numbers — use --profile profile for a figure worth recording." >&2
    fi

    just build-engine --profile "${profile}"

    # The tool that decides a threshold, checked before it decides one. It costs milliseconds and it
    # has caught a real defect: recording used to keep only the last runner's thresholds.
    echo "==> selftest    benchmarks/tools/compare.py"
    python3 benchmarks/tools/compare.py --selftest

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    results="${build_dir}/benchmarks/results"
    mkdir -p "${results}"

    status=0
    found=0
    for runner in "${build_dir}"/benchmarks/cy_bench_*; do
        [ -x "${runner}" ] || continue
        name="$(basename "${runner}")"
        # Not everything in benchmarks/ is one of the shared harness's runners.
        # `cy_bench_jobs_throughput` is built by src/core/jobs/bench/, measures a thread pool against
        # a serial baseline rather than a fixed-iteration loop, owns its own thresholds file, and has
        # its own recipe — `just test-bench-jobs`. It has no --json option, so this loop handed it an
        # argument it rejects and the recipe exited 2 before reaching the runners after it. That has
        # been true since M1 and was invisible because the alphabetically later runner was the only
        # one it hid.
        #
        # The probe is `--list`, which every cy_add_benchmark() runner answers and nothing else does,
        # rather than a list of names here that would drift the first time a second such runner
        # appears.
        if ! "${runner}" --list >/dev/null 2>&1; then
            echo "==> ${name}: not a cy_add_benchmark() runner; it has a recipe of its own" >&2
            continue
        fi
        found=1
        echo "==> ${name}"
        "${runner}" --json "${results}/${name}.json" ${runner_args[@]+"${runner_args[@]}"}
        echo "==> thresholds  benchmarks/baseline.json"
        python3 benchmarks/tools/compare.py \
            --results "${results}/${name}.json" \
            --baseline benchmarks/baseline.json ${record} || status=$?
    done

    if [ "${found}" -eq 0 ]; then
        echo "test-bench: no benchmark runners in ${build_dir}/benchmarks/" >&2
        exit 1
    fi
    exit "${status}"

# --- Shared implementation ------------------------------------------------------------------------

# The build tree for one sanitizer. Two rules, and each was a real failure before it was one.
#
# **Never CY_BUILD_DIR itself.** A sanitized tree carries -D CY_SANITIZE and instrumented binaries,
# and handing it back as the ordinary build directory makes every later `just quality-lint` lint a
# database full of sanitizer flags and every later `just run-sample` run an instrumented binary.
# CY_BUILD_DIR selects the parent instead, so an override is honoured without an ordinary tree and a
# sanitized one ever being the same directory.
#
# **Never a comma in the path.** `--sanitizer address,undefined` named a directory
# `build/sanitize-address,undefined`, and SDL3 probes the linker with
# `-Wl,--version-script=<path>.sym`. `-Wl,` splits its argument on commas, so the linker was handed
# `<...>/sanitize-address` and reported that the platform does not support version scripts —
# configuring SDL3 failed outright. It never fired locally only because everyone who ran that gate
# had CY_BUILD_DIR set to a comma-free directory, which is the rule above being broken. Any
# character that is not a letter, a digit or a hyphen becomes a hyphen.
#
# tools/ci/test_recipes.py holds both rules.
[private]
_sanitize-dir sanitizer:
    @echo "${CY_BUILD_DIR:-build}/sanitize-$(echo '{{sanitizer}}' | tr -c 'a-zA-Z0-9-' '-' | sed 's/-*$//')"

# Build, then run one kind of suite. Private: the graduated recipes above are the interface.
#
# CY_BUILD_DIR redirects the build tree, CY_JOBS sets the parallelism, and CY_TEST_JUNIT names a
# file for the machine-readable results `testing-and-quality` requires of a CI run. Everything after
# `--profile <name>` is forwarded to ctest, so `just test-unit -R harness` and `just test-unit
# --rerun-failed` work without this recipe knowing what those options mean.
[private]
_ctest kind *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    just _report-override CY_TEST_JUNIT "${CY_TEST_JUNIT:-}"

    just build-engine --profile "${profile}"

    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    ctest_args=(--test-dir "${build_dir}" --output-on-failure)

    # A suite that matches no test is an error everywhere except `unclassified`, which is empty
    # when every test has found its place in the taxonomy. That one exception is named rather than
    # blanket, so the day a label is misspelled the run says so instead of passing. Smoke carried
    # the same exemption until task 4.1.4 landed smoke.empty_sample; it does not any more, so a
    # smoke suite that stops being registered fails here rather than reporting a vacuous pass.
    if [ "{{kind}}" = "unclassified" ]; then
        ctest_args+=(--label-exclude "^(unit|integration|smoke|render)\$" --no-tests=ignore)
    elif [ "{{kind}}" = "render" ]; then
        # The render suites are declared only when a rendering backend is compiled in
        # (tests/render/CMakeLists.txt), because every case needs a device to read back from. So an
        # empty selection here is the ordinary state of a build with CY_RENDERER_VULKAN off, and it
        # is not an error — unlike a misspelled label, which the branch below still catches.
        ctest_args+=(--label-regex "^render\$" --no-tests=ignore)
    else
        ctest_args+=(--label-regex "^{{kind}}\$" --no-tests=error)
    fi
    if [ -n "${CY_JOBS:-}" ]; then
        ctest_args+=(--parallel "${CY_JOBS}")
    fi
    if [ -n "${CY_TEST_JUNIT:-}" ]; then
        ctest_args+=(--output-junit "${CY_TEST_JUNIT}")
    fi

    if [ "{{kind}}" = "unclassified" ]; then
        echo "==> tests carrying none of the taxonomy's labels"
    else
        echo "==> {{kind}} tests"
    fi
    started=${SECONDS}
    status=0
    ctest "${ctest_args[@]}" ${rest} || status=$?
    elapsed=$((SECONDS - started))

    # The taxonomy's fast-feedback requirement is that the unit set completes in under a minute on a
    # development machine. Reported every run, because a budget nobody sees is a budget nobody keeps.
    if [ "{{kind}}" = "unclassified" ] && [ "${status}" -eq 0 ]; then
        # ctest names them on failure; on success they are silent, and silently-passing tests
        # outside the taxonomy are exactly what this pass exists to surface.
        unclassified="$(ctest --test-dir "${build_dir}" -N \
            --label-exclude "^(unit|integration|smoke|render)\$" 2>/dev/null \
            | sed -n 's/^  Test *#[0-9]*: //p')"
        if [ -n "${unclassified}" ]; then
            echo "note: these tests carry no taxonomy label, so no graduated recipe runs them:" >&2
            printf '        %s\n' ${unclassified} >&2
            echo "      Declare them with cy_add_test(NAME <n> KIND unit|integration|smoke|render ...)" >&2
            echo "      from tests/, or set LABELS on them; that is what gives them a budget." >&2
        fi
    fi

    if [ "{{kind}}" = "unit" ]; then
        echo "==> unit suite ran in ${elapsed}s of its 60 s budget"
        if [ "${elapsed}" -gt 60 ]; then
            echo "warning: the unit set is over its budget. Move what is slow into" >&2
            echo "         tests/integration/ — the taxonomy in testing-and-quality is the rule." >&2
        fi
    fi
    exit "${status}"

# The job system's throughput, checked against benchmarks/thresholds. Pass --record to rewrite them.
test-bench-jobs *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"

    # A separate recipe from `test-bench` because this benchmark is not one of the shared harness's:
    # it measures a thread pool against a serial baseline rather than a fixed-iteration loop, and it
    # owns its own thresholds (src/core/jobs/bench/thresholds.json) and its own comparison. The
    # runner exits non-zero on a regression, so this is a gate rather than a report.
    if [ "$(just _profile-column "${profile}" 3)" = "on" ]; then
        echo "note: profile '${profile}' has assertions on. Measurements from it are not shipping" >&2
        echo "      numbers — use --profile profile for a figure worth recording." >&2
    fi

    just build-engine --profile "${profile}"
    build_dir="${CY_BUILD_DIR:-build/${profile}}"
    runner="${build_dir}/benchmarks/cy_bench_jobs_throughput"
    if [ ! -x "${runner}" ]; then
        echo "test-bench-jobs: no runner at ${runner}" >&2
        echo "  It is built with the tests (CY_BUILD_TESTS); check that they are enabled." >&2
        exit 1
    fi
    echo "==> jobs throughput"
    exec "${runner}" ${rest}

# Build and run a suite under a sanitizer. Usage: just test-sanitize [--sanitizer thread|address|...] [--tests <regex>]
test-sanitize *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    IFS=$'\t' read -r profile rest <<< "$(just _split {{args}})"

    # `core-jobs-and-concurrency` asks for a data-race detector over the concurrency suites, and
    # `testing-and-quality` asks for the unit and integration suites under ASan, UBSan and TSan at
    # least nightly. This is the one recipe both mean: it configures a sanitized tree of its own —
    # a sanitized build is not interchangeable with an ordinary one, so it never reuses build/<profile>
    # — builds it, and runs the selected tests in it.
    sanitizer="thread"
    tests="."
    leaks="on"
    expecting=""
    ctest_args=()
    for argument in ${rest}; do
        case "${expecting}" in
            sanitizer) sanitizer="${argument}"; expecting=""; continue ;;
            tests)     tests="${argument}";     expecting=""; continue ;;
            leaks)     leaks="${argument}";     expecting=""; continue ;;
        esac
        case "${argument}" in
            --sanitizer) expecting=sanitizer ;;
            --sanitizer=*) sanitizer="${argument#*=}" ;;
            --tests) expecting=tests ;;
            --tests=*) tests="${argument#*=}" ;;
            --leaks) expecting=leaks ;;
            --leaks=*) leaks="${argument#*=}" ;;
            *) ctest_args+=("${argument}") ;;
        esac
    done

    # A sanitized tree is never an ordinary one, CY_BUILD_DIR or not. Reading the override as the
    # tree to build *in* configures the developer's own build directory with -D CY_SANITIZE and
    # leaves it that way: every later `just quality-lint` then lints a compile database carrying
    # sanitizer flags, and every later `just run-sample` runs an instrumented binary. That is not
    # hypothetical — it is how `just roadmap-milestone m1` turned `just roadmap-milestone m0` red on
    # the tree it had just proved green, which is the exact regression the ledger exists to catch.
    #
    # So the override selects the *parent*, and the sanitizer names a subdirectory under it. An
    # explicit CY_BUILD_DIR is still honoured — a developer who redirects the build tree gets their
    # sanitized trees beside it — and the default is unchanged. The decision is `_sanitize-dir`
    # rather than an expression here, so that tools/ci/test_recipes.py can hold it to the rule.
    build_dir="$(just _sanitize-dir "${sanitizer}")"
    just _report-override CY_BUILD_DIR "${CY_BUILD_DIR:-}"
    echo "==> sanitize    ${sanitizer}  profile=${profile}  tree=${build_dir}"
    CY_BUILD_DIR="${build_dir}" just build-engine --profile "${profile}" -D "CY_SANITIZE=${sanitizer}"

    # A sanitized binary is five to twenty times slower, which is the tool working rather than the
    # test regressing. The harness scales its per-test budget under a sanitizer already; this makes
    # the intent explicit and gives CTest room too.
    #
    # TWENTY, NOT ZERO. Through M2 this line exported 0, which does not relax the budget — it
    # switches the check off — and `tests/unit/harness/test_assertions.cpp` asserted that the scale
    # was positive, so `just test-sanitize --tests .` failed on the harness suite and the recipe
    # could not be used over the whole tree. `testing-and-quality` asks for the unit and integration
    # suites under every sanitizer at least nightly, so "the whole tree" is the case that matters.
    # Both halves are fixed (task 1.5): the harness's own suite no longer requires a positive scale,
    # and the default here is the same twenty the harness picks for itself under a sanitizer, so the
    # budget still measures something. Export CY_TEST_BUDGET_SCALE=0 deliberately to switch it off.
    export CY_TEST_BUDGET_SCALE="${CY_TEST_BUDGET_SCALE:-20}"

    # LeakSanitizer is ON by default. It was off through M0 because the trace's per-thread rings are
    # pooled for the life of the process and nothing declared them, so every suite that opened a
    # trace reported them; M1 task 2.12 closed that in src/core/diagnostics/src/lifetime.h, which
    # declares the slot and its ring at the allocation site. A leak reported here is now a defect.
    # `--leaks off` turns detection off for a bisect; it is not a state anything should be left in.
    if [ "${leaks}" = "on" ]; then
        export ASAN_OPTIONS="${ASAN_OPTIONS:-}${ASAN_OPTIONS:+:}detect_leaks=1"
    else
        export ASAN_OPTIONS="${ASAN_OPTIONS:-}${ASAN_OPTIONS:+:}detect_leaks=0"
    fi
    export UBSAN_OPTIONS="${UBSAN_OPTIONS:-}${UBSAN_OPTIONS:+:}print_stacktrace=1"

    # ThreadSanitizer maps its shadow memory at fixed addresses and refuses to start when the
    # kernel's ASLR entropy puts the executable where it wants to be — "unexpected memory mapping"
    # on a recent Linux with vm.mmap_rnd_bits at 32. `setarch -R` disables randomisation for the
    # run, which is the documented workaround and costs nothing else.
    launcher=()
    if [ "${sanitizer}" = "thread" ] && command -v setarch >/dev/null 2>&1; then
        launcher=(setarch "$(uname -m)" -R)
    fi

    echo "==> tests       ${tests}"
    "${launcher[@]}" ctest --test-dir "${build_dir}" --output-on-failure \
        --tests-regex "${tests}" --no-tests=error ${ctest_args[@]+"${ctest_args[@]}"}
